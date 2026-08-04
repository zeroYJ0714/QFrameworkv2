#include "tst_Baseline.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfoList>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QMainWindow>
#include <QMutex>
#include <QMutexLocker>
#include <QPluginLoader>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWaitCondition>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "FrameworkConfig.h"
#include "InProcessUiModule.h"
#include "InProcessNonUiModule.h"
#include "Logger.h"
#include "LayoutManager.h"
#include "ManagedDockWidget.h"
#include "MessageBus.h"
#include "PluginManager.h"
#include "ProcessProtocol.h"
#include "ProcessSupervisor.h"
#include "ProcessWindowHost.h"
#include "QFrameworkPlugin.h"
#include "QFrameworkVersion.h"
#include "SingleInstanceGuard.h"
#include "StyleManager.h"
#include "ProtocolVersion.h"
#include "MessageTopics.h"
#include "Generated/image_messages.pb.h"
#include "Generated/log_messages.pb.h"
#include "Generated/status_messages.pb.h"

namespace
{
#ifdef Q_OS_WIN
QSize nativeWindowClientSize(quintptr windowId)
{
    const HWND handle = reinterpret_cast<HWND>(windowId);
    RECT rect = {};
    if (handle == nullptr || !IsWindow(handle) || !GetClientRect(handle, &rect))
        return QSize();
    return QSize(rect.right - rect.left, rect.bottom - rect.top);
}
#endif

class FakeHost : public qframework::ModuleHost
{
public:
    bool publishFromModule(const QString& moduleId,
                           const QString& topic,
                           const QByteArray& data) override
    {
        lastModuleId = moduleId;
        lastTopic = topic;
        lastData = data;
        ++publishCount;
        return true;
    }

    void logFromModule(qframework::LogLevel level,
                       const QString& moduleId,
                       const QString& text) override
    {
        Q_UNUSED(level)
        lastModuleId = moduleId;
        lastText = text;
    }

    QString lastModuleId;
    QString lastTopic;
    QByteArray lastData;
    QString lastText;
    int publishCount = 0;
};

class TestNonUiModule : public qframework::InProcessNonUiModule
{
public:
    using qframework::InProcessNonUiModule::InProcessNonUiModule;

    QStringList publishedTopics() const override
    {
        return QStringList() << QStringLiteral("TEST_TOPIC");
    }
};

struct ReceivedMessage
{
    QString topic;
    QString sender;
    QByteArray data;
};

class BusTestModule : public qframework::InProcessNonUiModule
{
public:
    BusTestModule(const QStringList& published, const QStringList& subscribed)
        : published_(published),
          subscribed_(subscribed)
    {
    }

    QStringList publishedTopics() const override { return published_; }
    QStringList subscribedTopics() const override { return subscribed_; }

    void onMessage(const QString& topic,
                   const QString& senderModuleId,
                   const QByteArray& data) override
    {
        QMutexLocker locker(&mutex_);
        ReceivedMessage message;
        message.topic = topic;
        message.sender = senderModuleId;
        message.data = data;
        received_.append(message);
        changed_.wakeAll();
    }

    bool waitForCount(int count, int timeoutMs)
    {
        QElapsedTimer timer;
        timer.start();
        QMutexLocker locker(&mutex_);
        while (received_.size() < count) {
            const int remaining = timeoutMs - static_cast<int>(timer.elapsed());
            if (remaining <= 0 || !changed_.wait(&mutex_, static_cast<unsigned long>(remaining)))
                break;
        }
        return received_.size() >= count;
    }

    QVector<ReceivedMessage> received() const
    {
        QMutexLocker locker(&mutex_);
        return received_;
    }

private:
    QStringList published_;
    QStringList subscribed_;
    mutable QMutex mutex_;
    QWaitCondition changed_;
    QVector<ReceivedMessage> received_;
};

QString fixedConfigPath()
{
    const QByteArray configured = qgetenv("QFRAMEWORK_CONFIG_PATH");
    if (!configured.isEmpty())
        return QString::fromUtf8(configured);
    return QDir::cleanPath(QCoreApplication::applicationDirPath()
                           + QStringLiteral("/../../../../config/QFramework.ini"));
}

QString pluginPath(const QString& moduleId)
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("Plugins/%1/%1.dll").arg(moduleId));
}

qframework::ModuleConfig pluginConfig(const QString& moduleId,
                                      qframework::ModuleType type,
                                      bool enabled = true)
{
    qframework::ModuleConfig config;
    config.id = moduleId;
    config.type = type;
    config.enabled = enabled;
    config.filePath = pluginPath(moduleId);
    config.displayName = moduleId;
    return config;
}

QString processModulePath(const QString& moduleId)
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("Plugins/%1/%1.exe").arg(moduleId));
}

qframework::ModuleConfig processModuleConfig(const QString& moduleId,
                                             qframework::ModuleType type)
{
    qframework::ModuleConfig config;
    config.id = moduleId;
    config.type = type;
    config.filePath = processModulePath(moduleId);
    config.displayName = moduleId;
    return config;
}

int stateSignalCount(const QSignalSpy& spy,
                      const QString& moduleId,
                      const QString& state)
{
    int count = 0;
    for (const QList<QVariant>& arguments : spy) {
        if (arguments.size() >= 2 &&
            arguments.at(0).toString() == moduleId &&
            arguments.at(1).toString() == state) {
            ++count;
        }
    }
    return count;
}

bool hasSupervisorArguments(int argc, char* argv[])
{
    for (int index = 1; index < argc; ++index) {
        if (QByteArray(argv[index]) == QByteArrayLiteral("--qframework-server"))
            return true;
    }
    return false;
}

QString supervisorArgumentValue(const QStringList& arguments, const QString& name)
{
    const int index = arguments.indexOf(name);
    if (index < 0 || index + 1 >= arguments.size())
        return QString();
    return arguments.at(index + 1);
}

bool writeFaultProcessFrame(QLocalSocket* socket, const QJsonObject& frame)
{
    if (socket == nullptr)
        return false;
    const QByteArray encoded = qframework::process::encodeFrame(frame);
    if (socket->write(encoded) != encoded.size())
        return false;
    socket->flush();
    return socket->bytesToWrite() == 0 || socket->waitForBytesWritten(1000);
}

int runFaultProcessClient(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = QCoreApplication::arguments();
    const QString serverName = supervisorArgumentValue(
        arguments,
        QStringLiteral("--qframework-server"));
    const QString token = supervisorArgumentValue(
        arguments,
        QStringLiteral("--qframework-token"));
    const QString moduleId = supervisorArgumentValue(
        arguments,
        QStringLiteral("--qframework-module-id"));
    const QString moduleType = supervisorArgumentValue(
        arguments,
        QStringLiteral("--qframework-module-type"));
    if (serverName.isEmpty() || token.isEmpty() || moduleId.isEmpty() || moduleType.isEmpty())
        return 20;

    QLocalSocket socket;
    socket.connectToServer(serverName);
    if (!socket.waitForConnected(2000))
        return 21;

    if (moduleId == QStringLiteral("RegistrationTimeoutModule"))
        return application.exec();

    QJsonObject registration;
    registration.insert(QStringLiteral("type"), QStringLiteral("register"));
    registration.insert(QStringLiteral("moduleId"), moduleId);
    registration.insert(QStringLiteral("moduleType"), moduleType);
    registration.insert(
        QStringLiteral("token"),
        moduleId == QStringLiteral("InvalidTokenModule")
            ? QStringLiteral("invalid-token")
            : token);
    registration.insert(QStringLiteral("publishedTopics"), QJsonArray());
    registration.insert(QStringLiteral("subscribedTopics"), QJsonArray());
    if (!writeFaultProcessFrame(&socket, registration))
        return 22;

    if (moduleId == QStringLiteral("InvalidTokenModule"))
        return application.exec();

    QByteArray inputBuffer;
    QElapsedTimer timer;
    timer.start();
    bool accepted = false;
    while (!accepted && timer.elapsed() < 2000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (socket.bytesAvailable() == 0)
            socket.waitForReadyRead(50);
        inputBuffer.append(socket.readAll());
        for (;;) {
            QJsonObject frame;
            QString error;
            const qframework::process::FrameResult result =
                qframework::process::takeFrame(&inputBuffer, &frame, 1024 * 1024, &error);
            if (result == qframework::process::FrameResult::Incomplete)
                break;
            if (result == qframework::process::FrameResult::Invalid)
                return 23;
            if (frame.value(QStringLiteral("type")).toString() ==
                    QStringLiteral("registerAck") &&
                frame.value(QStringLiteral("accepted")).toBool()) {
                accepted = true;
                break;
            }
        }
    }
    if (!accepted)
        return 24;

    // 模拟子进程在 onStart() 前等待 Visual Studio 附加。
    if (moduleId == QStringLiteral("DebuggerWaitModule"))
        QTest::qWait(600);

    QJsonObject started;
    started.insert(QStringLiteral("type"), QStringLiteral("started"));
    if (!writeFaultProcessFrame(&socket, started))
        return 25;

    // 故意不读取后续 ping，监督器应按心跳超时结束该进程。
    return application.exec();
}

qframework::MessageBusConfig faultBusConfig()
{
    qframework::MessageBusConfig config;
    config.defaultQueueCapacity = 8;
    config.maxMessageBytes = 1024 * 1024;
    config.sharedMemoryThresholdBytes = 256;
    config.shutdownDrainTimeoutMs = 200;
    return config;
}

qframework::ProcessConfig faultProcessConfig()
{
    qframework::ProcessConfig config;
    config.registrationTimeoutMs = 250;
    config.heartbeatIntervalMs = 50;
    config.heartbeatTimeoutMs = 250;
    config.stopTimeoutMs = 200;
    config.restartDelayMs = 50;
    config.restartWindowMs = 1000;
    config.maxRestartCount = 0;
    return config;
}

qframework::ModuleConfig faultProcessModuleConfig(const QString& moduleId)
{
    qframework::ModuleConfig config;
    config.id = moduleId;
    config.type = qframework::ModuleType::ProcessNonUi;
    config.enabled = true;
    config.filePath = QCoreApplication::applicationFilePath();
    config.displayName = moduleId;
    return config;
}

bool faultSignalContains(const QSignalSpy& spy,
                         const QString& moduleId,
                         const QString& detail)
{
    for (const QList<QVariant>& arguments : spy) {
        if (arguments.size() >= 2 &&
            arguments.at(0).toString() == moduleId &&
            arguments.at(1).toString().contains(detail)) {
            return true;
        }
    }
    return false;
}
}

void BaselineTest::qtAndFrameworkVersions()
{
    QVERIFY(QT_VERSION >= QT_VERSION_CHECK(5, 15, 2));
    QCOMPARE(QString::fromUtf8(qframework::frameworkVersion()),
             QStringLiteral("0.1.0-baseline"));
    QCOMPARE(QString::fromUtf8(qframework::protobufRuntimeVersion()),
             QStringLiteral("3.21.12"));
}

void BaselineTest::moduleLifecycleDefaults()
{
    TestNonUiModule module;
    QVERIFY(module.publishedTopics().contains(QStringLiteral("TEST_TOPIC")));
    QVERIFY(module.subscribedTopics().isEmpty());
    QVERIFY(module.onStart());
    QVERIFY(!module.publish(QStringLiteral("TEST_TOPIC"), QByteArray("before")));

    FakeHost host;
    module.bindHost(QStringLiteral("TestModule"), &host);
    module.setRunning(true);
    QVERIFY(module.publish(QStringLiteral("TEST_TOPIC"), QByteArray("payload")));
    QCOMPARE(host.publishCount, 1);
    QCOMPARE(host.lastModuleId, QStringLiteral("TestModule"));
    QCOMPARE(host.lastTopic, QStringLiteral("TEST_TOPIC"));
    QCOMPARE(host.lastData, QByteArray("payload"));
    module.setRunning(false);
    QVERIFY(!module.publish(QStringLiteral("TEST_TOPIC"), QByteArray("after")));
}

void BaselineTest::protobufRoundTrip()
{
    qframework::protocols::ImageFrame image;
    image.set_sequence(42);
    image.set_width(2);
    image.set_height(1);
    image.set_format("GRAY8");
    image.set_payload("ab");

    std::string bytes;
    QVERIFY(image.SerializeToString(&bytes));
    qframework::protocols::ImageFrame decoded;
    QVERIFY(decoded.ParseFromString(bytes));
    QCOMPARE(static_cast<qulonglong>(decoded.sequence()), 42ULL);
    QCOMPARE(QString::fromStdString(decoded.format()), QStringLiteral("GRAY8"));
    QCOMPARE(QByteArray::fromStdString(decoded.payload()), QByteArray("ab"));

    qframework::protocols::ModuleStatus status;
    status.set_module_id("TestModule");
    status.set_state(qframework::protocols::MODULE_STATE_RUNNING);
    QVERIFY(status.IsInitialized());
    QVERIFY(QString::fromUtf8(QFRAMEWORK_IMAGE_RAW) != QString::fromUtf8(QFRAMEWORK_STATUS));
}

void BaselineTest::configIsReadOnlyAndResolvesPaths()
{
    const QString path = fixedConfigPath();
    QFile file(path);
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
    const QByteArray before = file.readAll();
    file.close();

    qframework::FrameworkConfig config;
    QString error;
    QVERIFY2(config.load(path, &error), qPrintable(error));
    const QVector<qframework::ModuleConfig> modules = config.modules();
    QCOMPARE(modules.size(), 4);
    QCOMPARE(modules.at(0).id, QStringLiteral("InProcessUiExample"));
    QCOMPARE(modules.at(1).id, QStringLiteral("InProcessNonUiExample"));
    QVERIFY(modules.at(0).filePath.endsWith(QStringLiteral("Plugins/InProcessUiExample/InProcessUiExample.dll")));
    QVERIFY(config.logging().directory.endsWith(QStringLiteral("Logs")));
    QCOMPARE(config.messageBus().topics.value(QStringLiteral("QFRAMEWORK_IMAGE_RAW")).policy,
             qframework::QueuePolicy::Latest);

    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray after = file.readAll();
    file.close();
    QCOMPARE(after, before);
}

void BaselineTest::loggerRollsAndCapturesQtMessages()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    qframework::Logger& logger = qframework::Logger::instance();
    logger.uninstallQtMessageHandler();
    logger.stop();
    QString error;
    QVERIFY(logger.start(QDir(temporary.path()).filePath(QStringLiteral("Logs")), 256, &error));
    logger.installQtMessageHandler();
    logger.log(qframework::LogLevel::Info, QStringLiteral("TestModule"), QStringLiteral("first message"));
    qWarning("captured warning");
    for (int i = 0; i < 12; ++i)
        logger.log(qframework::LogLevel::Debug, QStringLiteral("TestModule"), QStringLiteral("record-%1").arg(i));
    logger.flush();
    logger.uninstallQtMessageHandler();
    logger.stop();

    const QFileInfoList files = QDir(QDir(temporary.path()).filePath(QStringLiteral("Logs")))
        .entryInfoList(QStringList() << QStringLiteral("QFramework_*.log"), QDir::Files);
    QVERIFY(files.size() >= 2);
    QByteArray all;
    for (const QFileInfo& info : files) {
        QFile logFile(info.absoluteFilePath());
        QVERIFY(logFile.open(QIODevice::ReadOnly));
        all += logFile.readAll();
    }
    QVERIFY(all.contains("[INFO] [TestModule]"));
    QVERIFY(all.contains("[WARNING] [Unknown]"));
    QVERIFY(all.contains("thread:"));
}

void BaselineTest::messageBusPoliciesAndOrdering()
{
    qframework::MessageBusConfig config;
    config.defaultQueueCapacity = 8;
    config.maxMessageBytes = 64;
    config.shutdownDrainTimeoutMs = 2000;

    qframework::TopicConfig latest;
    latest.queueCapacity = 1;
    latest.maxMessageBytes = 64;
    latest.policy = qframework::QueuePolicy::Latest;
    config.topics.insert(QStringLiteral("LATEST"), latest);

    qframework::TopicConfig reliable;
    reliable.queueCapacity = 1;
    reliable.maxMessageBytes = 64;
    reliable.policy = qframework::QueuePolicy::Reliable;
    config.topics.insert(QStringLiteral("RELIABLE"), reliable);

    qframework::TopicConfig small = reliable;
    small.maxMessageBytes = 4;
    config.topics.insert(QStringLiteral("SMALL"), small);

    BusTestModule publisher(
        QStringList() << QStringLiteral("ORDER") << QStringLiteral("LATEST")
                      << QStringLiteral("RELIABLE") << QStringLiteral("SMALL"),
        QStringList() << QStringLiteral("ORDER"));
    BusTestModule subscriber(
        QStringList(),
        QStringList() << QStringLiteral("ORDER") << QStringLiteral("LATEST")
                      << QStringLiteral("RELIABLE") << QStringLiteral("SMALL"));

    qframework::MessageBus bus(config);
    QString error;
    QVERIFY2(bus.registerModule(QStringLiteral("Publisher"), &publisher, &error), qPrintable(error));
    QVERIFY2(bus.registerModule(QStringLiteral("Subscriber"), &subscriber, &error), qPrintable(error));
    const QStringList emptyTopics;
    BusTestModule duplicate(emptyTopics, emptyTopics);
    QString duplicateError;
    QVERIFY(!bus.registerModule(QStringLiteral("Publisher"), &duplicate, &duplicateError));
    QCOMPARE(duplicateError,
             QString::fromUtf8(u8"模块 ID 重复：Publisher"));
    QVERIFY(bus.setModuleRunning(QStringLiteral("Publisher"), true));
    QVERIFY(bus.setModuleRunning(QStringLiteral("Subscriber"), true));

    QVERIFY(publisher.publish(QStringLiteral("ORDER"), QByteArray("1")));
    QVERIFY(publisher.publish(QStringLiteral("ORDER"), QByteArray("2")));
    QVERIFY(publisher.publish(QStringLiteral("ORDER"), QByteArray("3")));
    QVERIFY(publisher.publish(QStringLiteral("LATEST"), QByteArray("old")));
    QVERIFY(publisher.publish(QStringLiteral("LATEST"), QByteArray("new")));
    QVERIFY(publisher.publish(QStringLiteral("RELIABLE"), QByteArray("first")));
    QVERIFY(!publisher.publish(QStringLiteral("RELIABLE"), QByteArray("second")));
    QVERIFY(!publisher.publish(QStringLiteral("SMALL"), QByteArray("12345")));
    QVERIFY(!publisher.publish(QStringLiteral("UNDECLARED"), QByteArray("x")));

    bus.setDeliveryEnabled(true);
    QVERIFY(publisher.waitForCount(3, 2000));
    QVERIFY(subscriber.waitForCount(5, 2000));
    const QVector<ReceivedMessage> selfMessages = publisher.received();
    QCOMPARE(selfMessages.at(0).sender, QStringLiteral("Publisher"));
    QCOMPARE(selfMessages.at(0).data, QByteArray("1"));
    QCOMPARE(selfMessages.at(1).data, QByteArray("2"));
    QCOMPARE(selfMessages.at(2).data, QByteArray("3"));

    const QVector<ReceivedMessage> subscriberMessages = subscriber.received();
    bool sawLatest = false;
    for (const ReceivedMessage& message : subscriberMessages) {
        if (message.topic == QStringLiteral("LATEST")) {
            sawLatest = true;
            QCOMPARE(message.data, QByteArray("new"));
        }
    }
    QVERIFY(sawLatest);
    const qframework::ModuleQueueStats stats = bus.queueStats(QStringLiteral("Subscriber"));
    QVERIFY(stats.dropped >= 1);
    QVERIFY(stats.rejected >= 1);
    QCOMPARE(stats.delivered, 5ULL);

    bus.beginShutdown();
    QVERIFY(!publisher.publish(QStringLiteral("ORDER"), QByteArray("after")));
    QVERIFY(bus.stopQueues(2000));
    QVERIFY(bus.unregisterModule(QStringLiteral("Publisher"), false));
    QVERIFY(bus.unregisterModule(QStringLiteral("Subscriber"), false));
}

void BaselineTest::pluginLoaderAndModuleIntegration()
{
    const QString uiPath = pluginPath(QStringLiteral("InProcessUiExample"));
    const QString nonUiPath = pluginPath(QStringLiteral("InProcessNonUiExample"));
    QVERIFY2(QFileInfo::exists(uiPath), qPrintable(uiPath));
    QVERIFY2(QFileInfo::exists(nonUiPath), qPrintable(nonUiPath));

    QPluginLoader metadataLoader(uiPath);
    const QJsonObject metadata = metadataLoader.metaData();
    QCOMPARE(metadata.value(QStringLiteral("IID")).toString(),
             QString::fromLatin1(QFRAMEWORK_PLUGIN_IID));
    QCOMPARE(metadata.value(QStringLiteral("MetaData")).toObject()
                 .value(QStringLiteral("ModuleId")).toString(),
             QStringLiteral("InProcessUiExample"));

    qframework::MessageBusConfig busConfig;
    busConfig.defaultQueueCapacity = 32;
    busConfig.shutdownDrainTimeoutMs = 2000;
    qframework::MessageBus bus(busConfig);
    qframework::PluginManager manager(&bus);
    QSignalSpy stateSpy(&manager, &qframework::PluginManager::moduleStateChanged);

    // 顺序故意与配置文件不同，验证框架严格按传入配置顺序加载和启动。
    QVector<qframework::ModuleConfig> modules;
    modules.append(pluginConfig(QStringLiteral("InProcessNonUiExample"),
                                qframework::ModuleType::InProcessNonUi));
    modules.append(pluginConfig(QStringLiteral("InProcessUiExample"),
                                qframework::ModuleType::InProcessUi));
    QStringList errors;
    QVERIFY2(manager.loadAndStart(modules, &errors), qPrintable(errors.join('\n')));
    QCOMPARE(manager.runningModuleIds(),
             QStringList() << QStringLiteral("InProcessNonUiExample")
                           << QStringLiteral("InProcessUiExample"));
    QCOMPARE(stateSpy.size(), 4);
    QCOMPARE(stateSpy.at(0).at(0).toString(), QStringLiteral("InProcessNonUiExample"));
    QCOMPARE(stateSpy.at(0).at(1).toString(), QStringLiteral("Loaded"));
    QCOMPARE(stateSpy.at(1).at(0).toString(), QStringLiteral("InProcessUiExample"));
    QCOMPARE(stateSpy.at(1).at(1).toString(), QStringLiteral("Loaded"));
    QCOMPARE(stateSpy.at(2).at(0).toString(), QStringLiteral("InProcessNonUiExample"));
    QCOMPARE(stateSpy.at(2).at(1).toString(), QStringLiteral("Running"));
    QCOMPARE(stateSpy.at(3).at(0).toString(), QStringLiteral("InProcessUiExample"));
    QCOMPARE(stateSpy.at(3).at(1).toString(), QStringLiteral("Running"));

    qframework::InProcessUiModule* uiModule =
        manager.uiModule(QStringLiteral("InProcessUiExample"));
    QVERIFY(uiModule != nullptr);
    QTRY_COMPARE(uiModule->property("receivedMessageCount").toInt(), 1);
    manager.shutdown(2000);
    QVERIFY(manager.runningModuleIds().isEmpty());

    qframework::MessageBus disabledBus(busConfig);
    qframework::PluginManager disabledManager(&disabledBus);
    QVector<qframework::ModuleConfig> disabledModules;
    disabledModules.append(pluginConfig(QStringLiteral("DisabledMissingModule"),
                                        qframework::ModuleType::InProcessNonUi,
                                        false));
    disabledModules.append(pluginConfig(QStringLiteral("InProcessUiExample"),
                                        qframework::ModuleType::InProcessUi));
    errors.clear();
    QVERIFY2(disabledManager.loadAndStart(disabledModules, &errors),
             qPrintable(errors.join('\n')));
    QCOMPARE(disabledManager.runningModuleIds(),
             QStringList() << QStringLiteral("InProcessUiExample"));
    disabledManager.shutdown(2000);

    // 元数据类型错误只隔离当前模块，后续有效模块仍可启动。
    qframework::MessageBus isolatedBus(busConfig);
    qframework::PluginManager isolatedManager(&isolatedBus);
    QVector<qframework::ModuleConfig> isolatedModules;
    isolatedModules.append(pluginConfig(QStringLiteral("InProcessUiExample"),
                                        qframework::ModuleType::InProcessNonUi));
    isolatedModules.append(pluginConfig(QStringLiteral("InProcessNonUiExample"),
                                        qframework::ModuleType::InProcessNonUi));
    errors.clear();
    QVERIFY(!isolatedManager.loadAndStart(isolatedModules, &errors));
    QVERIFY(!errors.isEmpty());
    QCOMPARE(isolatedManager.runningModuleIds(),
             QStringList() << QStringLiteral("InProcessNonUiExample"));
    isolatedManager.shutdown(2000);
}

void BaselineTest::processProtocolFraming()
{
    QJsonObject source;
    source.insert(QStringLiteral("type"), QStringLiteral("ping"));
    source.insert(QStringLiteral("sequence"), 42);
    const QByteArray encoded = qframework::process::encodeFrame(source);
    QVERIFY(encoded.size() > 4);

    QByteArray partial = encoded.left(2);
    QJsonObject decoded;
    QString error;
    QCOMPARE(qframework::process::takeFrame(&partial, &decoded, 1024, &error),
             qframework::process::FrameResult::Incomplete);
    partial.append(encoded.mid(2));
    QCOMPARE(qframework::process::takeFrame(&partial, &decoded, 1024, &error),
             qframework::process::FrameResult::Ready);
    QCOMPARE(decoded.value(QStringLiteral("type")).toString(), QStringLiteral("ping"));
    QCOMPARE(decoded.value(QStringLiteral("sequence")).toInt(), 42);
    QVERIFY(partial.isEmpty());

    QByteArray oversized = QByteArray::fromHex("00010000");
    QCOMPARE(qframework::process::takeFrame(&oversized, &decoded, 32, &error),
             qframework::process::FrameResult::Invalid);
}

void BaselineTest::processIpcAndSupervision()
{
    const QString processUiPath = processModulePath(QStringLiteral("ProcessUiExample"));
    const QString processNonUiPath = processModulePath(QStringLiteral("ProcessNonUiExample"));
    QVERIFY2(QFileInfo::exists(processUiPath), qPrintable(processUiPath));
    QVERIFY2(QFileInfo::exists(processNonUiPath), qPrintable(processNonUiPath));

    qframework::MessageBusConfig busConfig;
    busConfig.defaultQueueCapacity = 32;
    busConfig.maxMessageBytes = 1024 * 1024;
    busConfig.sharedMemoryThresholdBytes = 256;
    busConfig.shutdownDrainTimeoutMs = 2000;
    qframework::ProcessConfig processConfig;
    processConfig.registrationTimeoutMs = 5000;
    processConfig.heartbeatIntervalMs = 100;
    processConfig.heartbeatTimeoutMs = 1000;
    processConfig.stopTimeoutMs = 2000;
    processConfig.restartDelayMs = 100;
    processConfig.restartWindowMs = 5000;
    processConfig.maxRestartCount = 2;

    qframework::MessageBus bus(busConfig);
    BusTestModule observer(
        QStringList(),
        QStringList() << QString::fromLatin1(QFRAMEWORK_LOG_DISPLAY));
    QString error;
    QVERIFY2(bus.registerModule(QStringLiteral("ProcessObserver"), &observer, &error),
             qPrintable(error));
    QVERIFY(bus.setModuleRunning(QStringLiteral("ProcessObserver"), true));

    qRegisterMetaType<quintptr>("quintptr");
    qframework::ProcessSupervisor supervisor(&bus, busConfig, processConfig);
    QSignalSpy stateSpy(&supervisor, &qframework::ProcessSupervisor::moduleStateChanged);
    QSignalSpy windowSpy(&supervisor, &qframework::ProcessSupervisor::windowHandleReady);
    qframework::ProcessWindowHost windowHost;
    windowHost.resize(720, 400);
    windowHost.show();
    QCoreApplication::processEvents();
    QVector<qframework::ModuleConfig> modules;
    modules.append(processModuleConfig(QStringLiteral("ProcessNonUiExample"),
                                       qframework::ModuleType::ProcessNonUi));
    modules.append(processModuleConfig(QStringLiteral("ProcessUiExample"),
                                       qframework::ModuleType::ProcessUi));
    QStringList errors;
    QVERIFY2(supervisor.startAll(modules, &errors), qPrintable(errors.join('\n')));
    QCOMPARE(supervisor.runningModuleIds(),
             QStringList() << QStringLiteral("ProcessNonUiExample")
                           << QStringLiteral("ProcessUiExample"));
    bus.setDeliveryEnabled(true);

    QTRY_VERIFY_WITH_TIMEOUT(observer.received().size() >= 2, 5000);
    bool foundLargeMessage = false;
    const QVector<ReceivedMessage> received = observer.received();
    for (const ReceivedMessage& message : received) {
        qframework::protocols::LogDisplayMessage logMessage;
        if (logMessage.ParseFromArray(message.data.constData(), message.data.size()) &&
            logMessage.text().size() > 2000) {
            foundLargeMessage = true;
        }
    }
    QVERIFY(foundLargeMessage);
    QTRY_VERIFY_WITH_TIMEOUT(windowSpy.size() >= 1, 5000);
    const quintptr initialWindowId = static_cast<quintptr>(
        windowSpy.at(0).at(1).toULongLong());
    QVERIFY(initialWindowId != 0);
    QVERIFY2(windowHost.attachWindow(initialWindowId, &error), qPrintable(error));
    QVERIFY2(supervisor.showWindow(QStringLiteral("ProcessUiExample"),
                                   windowHost.width(),
                                   windowHost.height(),
                                   &error),
             qPrintable(error));
#ifdef Q_OS_WIN
    QTRY_COMPARE_WITH_TIMEOUT(nativeWindowClientSize(initialWindowId),
                              windowHost.size(),
                              1000);
#endif

    QTest::qWait(1200);
    QCOMPARE(supervisor.state(QStringLiteral("ProcessNonUiExample")),
             QStringLiteral("Running"));
    QCOMPARE(supervisor.state(QStringLiteral("ProcessUiExample")),
             QStringLiteral("Running"));

    const int priorAutoRestartRunningSignals = stateSignalCount(
        stateSpy,
        QStringLiteral("ProcessNonUiExample"),
        QStringLiteral("Running"));
    QVERIFY(supervisor.terminate(QStringLiteral("ProcessNonUiExample")));
    QTRY_VERIFY_WITH_TIMEOUT(
        stateSignalCount(stateSpy,
                         QStringLiteral("ProcessNonUiExample"),
                         QStringLiteral("Running")) > priorAutoRestartRunningSignals,
        5000);
    QCOMPARE(supervisor.state(QStringLiteral("ProcessNonUiExample")),
             QStringLiteral("Running"));

    const int priorWindowCount = windowSpy.size();
    windowHost.showPlaceholder(QString::fromUtf8(u8"等待重新附加"));
    QVERIFY2(supervisor.restart(QStringLiteral("ProcessUiExample"), &error),
             qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(windowSpy.size() > priorWindowCount, 5000);
    QCOMPARE(supervisor.state(QStringLiteral("ProcessUiExample")),
             QStringLiteral("Running"));
    const quintptr restartedWindowId = static_cast<quintptr>(
        windowSpy.last().at(1).toULongLong());
    QVERIFY(restartedWindowId != 0);
    QVERIFY2(windowHost.attachWindow(restartedWindowId, &error), qPrintable(error));
    QVERIFY2(supervisor.showWindow(QStringLiteral("ProcessUiExample"),
                                   windowHost.width(),
                                   windowHost.height(),
                                   &error),
             qPrintable(error));
#ifdef Q_OS_WIN
    QTRY_COMPARE_WITH_TIMEOUT(nativeWindowClientSize(restartedWindowId),
                              windowHost.size(),
                              1000);
#endif

    windowHost.showPlaceholder(QString::fromUtf8(u8"测试结束"));
    supervisor.shutdown();
    QVERIFY(supervisor.runningModuleIds().isEmpty());
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(2000));
    QVERIFY(bus.unregisterModule(QStringLiteral("ProcessObserver"), false));
}

void BaselineTest::processRejectsInvalidToken()
{
    const qframework::MessageBusConfig busConfig = faultBusConfig();
    const qframework::ProcessConfig processConfig = faultProcessConfig();
    qframework::MessageBus bus(busConfig);
    qframework::ProcessSupervisor supervisor(&bus, busConfig, processConfig);
    QSignalSpy faultSpy(&supervisor, &qframework::ProcessSupervisor::moduleFault);

    const QString moduleId = QStringLiteral("InvalidTokenModule");
    QVector<qframework::ModuleConfig> modules;
    modules.append(faultProcessModuleConfig(moduleId));
    QStringList errors;
    QVERIFY(!supervisor.startAll(modules, &errors));
    QCOMPARE(supervisor.state(moduleId), QStringLiteral("Failed"));
    QVERIFY(errors.join('\n').contains(QString::fromUtf8(u8"子进程注册信息校验失败")));
    QVERIFY(faultSignalContains(faultSpy,
                                moduleId,
                                QString::fromUtf8(u8"子进程注册信息校验失败")));

    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(200));
}

void BaselineTest::processRegistrationTimeout()
{
    const qframework::MessageBusConfig busConfig = faultBusConfig();
    const qframework::ProcessConfig processConfig = faultProcessConfig();
    qframework::MessageBus bus(busConfig);
    qframework::ProcessSupervisor supervisor(&bus, busConfig, processConfig);
    QSignalSpy faultSpy(&supervisor, &qframework::ProcessSupervisor::moduleFault);

    const QString moduleId = QStringLiteral("RegistrationTimeoutModule");
    QVector<qframework::ModuleConfig> modules;
    modules.append(faultProcessModuleConfig(moduleId));
    QStringList errors;
    QVERIFY(!supervisor.startAll(modules, &errors));
    QCOMPARE(supervisor.state(moduleId), QStringLiteral("Failed"));
    QVERIFY(errors.join('\n').contains(QString::fromUtf8(u8"子进程注册超时")));
    QVERIFY(faultSignalContains(faultSpy,
                                moduleId,
                                QString::fromUtf8(u8"子进程注册超时")));

    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(200));
}

void BaselineTest::processHeartbeatTimeout()
{
    const qframework::MessageBusConfig busConfig = faultBusConfig();
    const qframework::ProcessConfig processConfig = faultProcessConfig();
    qframework::MessageBus bus(busConfig);
    qframework::ProcessSupervisor supervisor(&bus, busConfig, processConfig);
    QSignalSpy faultSpy(&supervisor, &qframework::ProcessSupervisor::moduleFault);

    const QString moduleId = QStringLiteral("HeartbeatTimeoutModule");
    QVector<qframework::ModuleConfig> modules;
    modules.append(faultProcessModuleConfig(moduleId));
    QStringList errors;
    QVERIFY2(supervisor.startAll(modules, &errors), qPrintable(errors.join('\n')));
    QCOMPARE(supervisor.state(moduleId), QStringLiteral("Running"));
    QTRY_VERIFY_WITH_TIMEOUT(supervisor.state(moduleId) == QStringLiteral("Failed"), 3000);
    QVERIFY(faultSignalContains(faultSpy,
                                moduleId,
                                QString::fromUtf8(u8"子进程心跳超时")));

    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(200));
}

void BaselineTest::processDebuggerWaitDefersHeartbeat()
{
    const qframework::MessageBusConfig busConfig = faultBusConfig();
    const qframework::ProcessConfig processConfig = faultProcessConfig();
    qframework::MessageBus bus(busConfig);
    qframework::ProcessSupervisor supervisor(&bus, busConfig, processConfig);
    QSignalSpy faultSpy(&supervisor, &qframework::ProcessSupervisor::moduleFault);

    const QString moduleId = QStringLiteral("DebuggerWaitModule");
    qframework::ModuleConfig module = faultProcessModuleConfig(moduleId);
    module.waitForDebugger = true;
    module.debuggerWaitTimeoutMs = 1000;
    QVector<qframework::ModuleConfig> modules;
    modules.append(module);
    QStringList errors;
    QElapsedTimer elapsed;
    elapsed.start();
    QVERIFY2(supervisor.startAll(modules, &errors), qPrintable(errors.join('\n')));
    QVERIFY(elapsed.elapsed() >= 500);
    QCOMPARE(supervisor.state(moduleId), QStringLiteral("Running"));
    QVERIFY(faultSpy.isEmpty());

    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(200));
}

void BaselineTest::singleInstancePerDirectory()
{
    QTemporaryDir firstDirectory;
    QTemporaryDir secondDirectory;
    QVERIFY(firstDirectory.isValid());
    QVERIFY(secondDirectory.isValid());

    qframework::SingleInstanceGuard first;
    qframework::SingleInstanceGuard duplicate;
    qframework::SingleInstanceGuard independent;
    QString error;
    QCOMPARE(first.acquire(firstDirectory.path(), &error),
             qframework::SingleInstanceResult::Acquired);
    QCOMPARE(duplicate.acquire(firstDirectory.path(), &error),
             qframework::SingleInstanceResult::AlreadyRunning);
    QCOMPARE(independent.acquire(secondDirectory.path(), &error),
             qframework::SingleInstanceResult::Acquired);
}

void BaselineTest::layoutPersistenceAndDockingRules()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString layoutPath = directory.filePath(QStringLiteral("workspace.qflayout"));
    const QString damagedPath = directory.filePath(QStringLiteral("damaged.qflayout"));

    QMainWindow window;
    window.setObjectName(QStringLiteral("LayoutTestWindow"));
    window.resize(900, 600);
    qframework::ManagedDockWidget firstDock(QStringLiteral("First"), &window);
    qframework::ManagedDockWidget secondDock(QStringLiteral("Second"), &window);
    firstDock.setObjectName(QStringLiteral("ModuleDock.First"));
    secondDock.setObjectName(QStringLiteral("ModuleDock.Second"));
    firstDock.setWidget(new QWidget);
    secondDock.setWidget(new QWidget);
    window.addDockWidget(Qt::LeftDockWidgetArea, &firstDock);
    window.addDockWidget(Qt::RightDockWidgetArea, &secondDock);
    window.show();
    firstDock.show();
    secondDock.hide();
    QCoreApplication::processEvents();

    qframework::LayoutManager manager(&window);
    manager.registerModuleDock(QStringLiteral("First"), &firstDock);
    manager.registerModuleDock(QStringLiteral("Second"), &secondDock);
    QString error;
    QVERIFY2(manager.saveLayout(layoutPath, &error), qPrintable(error));

    QFile layoutFile(layoutPath);
    QVERIFY(layoutFile.open(QIODevice::ReadOnly));
    QJsonDocument layoutDocument = QJsonDocument::fromJson(layoutFile.readAll());
    layoutFile.close();
    QVERIFY(layoutDocument.isObject());
    QJsonObject root = layoutDocument.object();
    QJsonObject modules = root.value(QStringLiteral("modules")).toObject();
    QJsonObject missingModule;
    missingModule.insert(QStringLiteral("visible"), true);
    modules.insert(QStringLiteral("MissingModule"), missingModule);
    root.insert(QStringLiteral("modules"), modules);
    QVERIFY(layoutFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(layoutFile.write(QJsonDocument(root).toJson()),
             QJsonDocument(root).toJson().size());
    layoutFile.close();

    firstDock.hide();
    secondDock.show();
    QStringList unavailableModules;
    QVERIFY2(manager.loadLayout(layoutPath, &error, &unavailableModules),
             qPrintable(error));
    QCOMPARE(unavailableModules, QStringList() << QStringLiteral("MissingModule"));
    QVERIFY(firstDock.isVisible());
    QVERIFY(!secondDock.isVisible());
    QCOMPARE(manager.activeFilePath(), QFileInfo(layoutPath).absoluteFilePath());

    firstDock.setFloating(true);
    QCoreApplication::processEvents();
    QVERIFY(!firstDock.isFloating());
    QCOMPARE(window.dockWidgetArea(&firstDock), Qt::LeftDockWidgetArea);

    const QByteArray stateBeforeFailure = window.saveState(1);
    const QString activeBeforeFailure = manager.activeFilePath();
    QFile damagedFile(damagedPath);
    QVERIFY(damagedFile.open(QIODevice::WriteOnly));
    QCOMPARE(damagedFile.write("{ damaged"), qint64(9));
    damagedFile.close();
    QVERIFY(!manager.loadLayout(damagedPath, &error));
    QCOMPARE(window.saveState(1), stateBeforeFailure);
    QCOMPARE(manager.activeFilePath(), activeBeforeFailure);
}

void BaselineTest::styleSheetReloadAndFailureRecovery()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString validPath = directory.filePath(QStringLiteral("valid.qss"));
    const QString invalidPath = directory.filePath(QStringLiteral("invalid.qss"));
    const QString missingPath = directory.filePath(QStringLiteral("missing.qss"));
    const QString originalStyleSheet = qApp->styleSheet();

    QFile validFile(validPath);
    QVERIFY(validFile.open(QIODevice::WriteOnly));
    const QByteArray firstStyle("QWidget { color: #20252b; }");
    QCOMPARE(validFile.write(firstStyle), qint64(firstStyle.size()));
    validFile.close();

    qframework::StyleManager manager;
    QSignalSpy changedSpy(&manager, &qframework::StyleManager::styleSheetChanged);
    QString error;
    QVERIFY2(manager.loadStyleSheet(validPath, &error), qPrintable(error));
    QCOMPARE(qApp->styleSheet(), QString::fromLatin1(firstStyle));
    QCOMPARE(changedSpy.size(), 1);

    QVERIFY(validFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray secondStyle("QWidget { color: #334455; background: #f4f6f8; }");
    QCOMPARE(validFile.write(secondStyle), qint64(secondStyle.size()));
    validFile.close();
    QVERIFY2(manager.reloadStyleSheet(&error), qPrintable(error));
    QCOMPARE(qApp->styleSheet(), QString::fromLatin1(secondStyle));
    QCOMPARE(changedSpy.size(), 2);

    QVERIFY(!manager.loadStyleSheet(missingPath, &error));
    QCOMPARE(qApp->styleSheet(), QString::fromLatin1(secondStyle));
    QCOMPARE(manager.currentFilePath(), QFileInfo(validPath).absoluteFilePath());

    QFile invalidFile(invalidPath);
    QVERIFY(invalidFile.open(QIODevice::WriteOnly));
    const QByteArray invalidStyle("QWidget { color: red;");
    QCOMPARE(invalidFile.write(invalidStyle), qint64(invalidStyle.size()));
    invalidFile.close();
    QVERIFY(!manager.loadStyleSheet(invalidPath, &error));
    QCOMPARE(qApp->styleSheet(), QString::fromLatin1(secondStyle));
    QCOMPARE(changedSpy.size(), 2);

    qApp->setStyleSheet(originalStyleSheet);
}

int main(int argc, char* argv[])
{
    if (hasSupervisorArguments(argc, argv))
        return runFaultProcessClient(argc, argv);

    QApplication application(argc, argv);
    BaselineTest test;
    return QTest::qExec(&test, argc, argv);
}
