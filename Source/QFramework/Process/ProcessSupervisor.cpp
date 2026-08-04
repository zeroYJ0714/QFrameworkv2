#include "ProcessSupervisor.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcessEnvironment>
#include <QSharedMemory>
#include <QThread>
#include <QUuid>

#include <cstring>

#include "Logger.h"
#include "MessageBus.h"
#include "ModuleEndpoint.h"
#include "ProcessProtocol.h"
#include "ProcessRuntime.h"

namespace qframework
{
namespace
{
const QString kRegister = QStringLiteral("register");
const QString kRegisterAck = QStringLiteral("registerAck");
const QString kStarted = QStringLiteral("started");
const QString kStartFailed = QStringLiteral("startFailed");
const QString kMessage = QStringLiteral("message");
const QString kLog = QStringLiteral("log");
const QString kPing = QStringLiteral("ping");
const QString kPong = QStringLiteral("pong");
const QString kStop = QStringLiteral("stop");
const QString kStopAck = QStringLiteral("stopAck");
const QString kWindowReady = QStringLiteral("windowReady");
const QString kShowWindow = QStringLiteral("showWindow");
const QString kWindowWidth = QStringLiteral("windowWidth");
const QString kWindowHeight = QStringLiteral("windowHeight");
const QString kSharedAck = QStringLiteral("sharedAck");
const QString kDebugWaitTimeout = QStringLiteral("debugWaitTimeout");
const QString kStyleSheet = QStringLiteral("styleSheet");

QString processTypeName(ModuleType type)
{
    if (type == ModuleType::ProcessUi)
        return QStringLiteral("ProcessUi");
    if (type == ModuleType::ProcessNonUi)
        return QStringLiteral("ProcessNonUi");
    return QString();
}

bool isProcessType(ModuleType type)
{
    return type == ModuleType::ProcessUi || type == ModuleType::ProcessNonUi;
}

QString makeServerName(const QString& moduleId)
{
    return QStringLiteral("QFramework_%1_%2_%3")
        .arg(moduleId,
             QString::number(QCoreApplication::applicationPid()),
             QUuid::createUuid().toString(QUuid::Id128));
}

QString makeSharedKey(const QString& moduleId)
{
    return QStringLiteral("QFrameworkShared_%1_%2")
        .arg(moduleId, QUuid::createUuid().toString(QUuid::Id128));
}

int maxFrameBytes(int maxMessageBytes)
{
    const qint64 value = qMax<qint64>(64 * 1024,
                                      static_cast<qint64>(maxMessageBytes) * 2);
    return static_cast<int>(qMin<qint64>(value, 128 * 1024 * 1024));
}

bool jsonStringList(const QJsonValue& value, QStringList* result)
{
    if (result == nullptr || !value.isArray())
        return false;
    result->clear();
    const QJsonArray array = value.toArray();
    for (const QJsonValue& item : array) {
        if (!item.isString())
            return false;
        result->append(item.toString());
    }
    return true;
}

QString detailWithPrefix(const QString& prefix, const QString& detail)
{
    if (detail.isEmpty())
        return prefix;
    return prefix + QStringLiteral(": ") + detail;
}
}

class ProcessBridge final : public ModuleEndpoint
{
public:
    ProcessBridge(ProcessSupervisor* supervisor,
                  const QString& moduleId,
                  const QStringList& publishedTopics,
                  const QStringList& subscribedTopics)
        : supervisor_(supervisor),
          moduleId_(moduleId),
          publishedTopics_(publishedTopics),
          subscribedTopics_(subscribedTopics)
    {
    }

    QStringList publishedTopics() const override { return publishedTopics_; }
    QStringList subscribedTopics() const override { return subscribedTopics_; }

    void onMessage(const QString& topic,
                   const QString& senderModuleId,
                   const QByteArray& data) override
    {
        QMetaObject::invokeMethod(
            supervisor_,
            "sendMessageToChild",
            Qt::QueuedConnection,
            Q_ARG(QString, moduleId_),
            Q_ARG(QString, topic),
            Q_ARG(QString, senderModuleId),
            Q_ARG(QByteArray, data));
    }

private:
    ProcessSupervisor* supervisor_;
    QString moduleId_;
    QStringList publishedTopics_;
    QStringList subscribedTopics_;
};

struct ProcessSupervisor::Entry
{
    ModuleConfig config;
    QString state;
    QString lastError;
    QString serverName;
    QString token;
    QLocalServer* server = nullptr;
    QLocalSocket* socket = nullptr;
    QProcess* process = nullptr;
    ProcessBridge* bridge = nullptr;
    QByteArray inputBuffer;
    QHash<QString, QSharedMemory*> outgoingShared;
    qint64 registrationDeadlineMs = 0;
    qint64 lastPongMs = 0;
    qint64 lastPingMs = 0;
    qint64 restartAtMs = 0;
    qint64 restartWindowStartMs = 0;
    int restartCount = 0;
    bool registered = false;
    bool running = false;
    bool stopping = false;
    bool faulted = false;
    bool stopAcknowledged = false;
};

ProcessSupervisor::ProcessSupervisor(MessageBus* messageBus,
                                     const MessageBusConfig& messageBusConfig,
                                     const ProcessConfig& processConfig,
                                     QObject* parent)
    : QObject(parent),
      messageBus_(messageBus),
      messageBusConfig_(messageBusConfig),
      processConfig_(processConfig),
      supervisionTimer_(new QTimer(this)),
      shuttingDown_(false)
{
    const int interval = qMax(50, qMin(processConfig_.heartbeatIntervalMs, 250));
    supervisionTimer_->setInterval(interval);
    connect(supervisionTimer_, &QTimer::timeout,
            this, &ProcessSupervisor::onSupervisionTick);
    supervisionTimer_->start();
}

ProcessSupervisor::~ProcessSupervisor()
{
    shutdown();
    qDeleteAll(entries_);
    entries_.clear();
}

bool ProcessSupervisor::startAll(const QVector<ModuleConfig>& modules,
                                 QStringList* errors)
{
    shuttingDown_ = false;
    if (!supervisionTimer_->isActive())
        supervisionTimer_->start();
    bool allSucceeded = true;
    for (const ModuleConfig& config : modules) {
        if (!config.enabled || !isProcessType(config.type))
            continue;
        if (findEntry(config.id) != nullptr) {
            allSucceeded = false;
            if (errors != nullptr)
                errors->append(QString::fromUtf8(u8"子进程模块 ID 重复：%1").arg(config.id));
            continue;
        }
        Entry* entry = new Entry;
        entry->config = config;
        entries_.append(entry);
        QString error;
        const int waitTimeout = qMax(1000,
                                     processConfig_.registrationTimeoutMs +
                                     (config.waitForDebugger
                                          ? config.debuggerWaitTimeoutMs
                                          : 0) + 1000);
        if (!startEntry(entry, &error) || !waitForRunning(entry, waitTimeout, &error)) {
            allSucceeded = false;
            if (errors != nullptr)
                errors->append(error);
        }
    }
    return allSucceeded;
}

bool ProcessSupervisor::stop(const QString& moduleId, QString* errorMessage)
{
    Entry* entry = findEntry(moduleId);
    if (entry == nullptr) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"未找到子进程模块：%1").arg(moduleId);
        return false;
    }
    if (entry->process == nullptr || entry->process->state() == QProcess::NotRunning) {
        destroyRuntime(entry);
        entry->faulted = false;
        entry->stopping = false;
        entry->restartAtMs = 0;
        emitState(entry, QStringLiteral("Stopped"), QString());
        return true;
    }

    entry->stopping = true;
    emitState(entry, QStringLiteral("Stopping"), QString());
    if (entry->socket != nullptr && entry->socket->state() == QLocalSocket::ConnectedState) {
        QJsonObject frame;
        frame.insert(QStringLiteral("type"), kStop);
        sendFrame(entry, frame);
    }

    QElapsedTimer timer;
    timer.start();
    const int timeout = qMax(1, processConfig_.stopTimeoutMs);
    while (entry->process != nullptr &&
           entry->process->state() != QProcess::NotRunning &&
           timer.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    bool graceful = entry->process == nullptr ||
                    entry->process->state() == QProcess::NotRunning;
    if (!graceful && entry->process != nullptr) {
        entry->process->terminate();
        if (!entry->process->waitForFinished(250))
            entry->process->kill();
        entry->process->waitForFinished(1000);
        graceful = false;
    }
    destroyRuntime(entry);
    entry->faulted = false;
    entry->restartAtMs = 0;
    emitState(entry, QStringLiteral("Stopped"), graceful ? QString() : QString::fromUtf8(u8"停止超时，已强制结束"));
    if (!graceful && errorMessage != nullptr)
        *errorMessage = QString::fromUtf8(u8"子进程停止超时");
    return graceful;
}

bool ProcessSupervisor::restart(const QString& moduleId, QString* errorMessage)
{
    Entry* entry = findEntry(moduleId);
    if (entry == nullptr)
        return false;
    stop(moduleId, errorMessage);
    entry->restartCount = 0;
    entry->restartWindowStartMs = 0;
    entry->restartAtMs = 0;
    entry->faulted = false;
    QString error;
    if (!startEntry(entry, &error) || !waitForRunning(entry,
                                                       qMax(1000,
                                                            processConfig_.registrationTimeoutMs +
                                                            (entry->config.waitForDebugger
                                                                 ? entry->config.debuggerWaitTimeoutMs
                                                                 : 0) + 1000),
                                                       &error)) {
        if (errorMessage != nullptr)
            *errorMessage = error;
        return false;
    }
    return true;
}

bool ProcessSupervisor::showWindow(const QString& moduleId, QString* errorMessage)
{
    return showWindow(moduleId, 0, 0, errorMessage);
}

bool ProcessSupervisor::showWindow(const QString& moduleId,
                                   int width,
                                   int height,
                                   QString* errorMessage)
{
    Entry* entry = findEntry(moduleId);
    if (entry == nullptr) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"未找到子进程模块：%1").arg(moduleId);
        return false;
    }
    if (entry->config.type != ModuleType::ProcessUi) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"模块不是子进程 UI：%1").arg(moduleId);
        return false;
    }
    if (!entry->registered || !entry->running) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"子进程 UI 尚未就绪：%1").arg(moduleId);
        return false;
    }

    QJsonObject frame;
    frame.insert(QStringLiteral("type"), kShowWindow);
    if (width > 0 && height > 0) {
        frame.insert(kWindowWidth, width);
        frame.insert(kWindowHeight, height);
    }
    if (sendFrame(entry, frame))
        return true;
    if (errorMessage != nullptr)
        *errorMessage = QString::fromUtf8(u8"无法向子进程发送窗口显示请求：%1").arg(moduleId);
    return false;
}

bool ProcessSupervisor::terminate(const QString& moduleId)
{
    Entry* entry = findEntry(moduleId);
    if (entry == nullptr || entry->process == nullptr ||
        entry->process->state() == QProcess::NotRunning)
        return false;
    entry->stopping = false;
    entry->process->kill();
    return true;
}

void ProcessSupervisor::shutdown()
{
    if (shuttingDown_ && entries_.isEmpty())
        return;
    shuttingDown_ = true;
    if (supervisionTimer_ != nullptr)
        supervisionTimer_->stop();
    for (int index = entries_.size() - 1; index >= 0; --index)
        stop(entries_.at(index)->config.id, nullptr);
}

QStringList ProcessSupervisor::runningModuleIds() const
{
    QStringList result;
    for (const Entry* entry : entries_) {
        if (entry->running)
            result.append(entry->config.id);
    }
    return result;
}

QString ProcessSupervisor::state(const QString& moduleId) const
{
    const Entry* entry = findEntry(moduleId);
    return entry == nullptr ? QString() : entry->state;
}

void ProcessSupervisor::applyStyleSheet(const QString& styleSheet)
{
    styleSheet_ = styleSheet;
    for (Entry* entry : entries_) {
        if (entry->config.type != ModuleType::ProcessUi || !entry->registered)
            continue;
        QJsonObject frame;
        frame.insert(QStringLiteral("type"), kStyleSheet);
        frame.insert(QStringLiteral("styleSheet"), styleSheet_);
        sendFrame(entry, frame);
    }
}

ProcessSupervisor::Entry* ProcessSupervisor::findEntry(const QString& moduleId) const
{
    for (Entry* entry : entries_) {
        if (entry->config.id == moduleId)
            return entry;
    }
    return nullptr;
}

ProcessSupervisor::Entry* ProcessSupervisor::findEntryByServer(QObject* object) const
{
    for (Entry* entry : entries_) {
        if (entry->server == object)
            return entry;
    }
    return nullptr;
}

ProcessSupervisor::Entry* ProcessSupervisor::findEntryBySocket(QObject* object) const
{
    for (Entry* entry : entries_) {
        if (entry->socket == object)
            return entry;
    }
    return nullptr;
}

ProcessSupervisor::Entry* ProcessSupervisor::findEntryByProcess(QObject* object) const
{
    for (Entry* entry : entries_) {
        if (entry->process == object)
            return entry;
    }
    return nullptr;
}

bool ProcessSupervisor::startEntry(Entry* entry, QString* errorMessage)
{
    if (entry == nullptr)
        return false;
    destroyRuntime(entry);
    entry->state = QStringLiteral("Starting");
    entry->lastError.clear();
    entry->faulted = false;
    entry->stopping = false;
    entry->restartAtMs = 0;
    entry->serverName = makeServerName(entry->config.id);
    entry->token = QUuid::createUuid().toString(QUuid::Id128);

    const QFileInfo fileInfo(entry->config.filePath);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"子进程文件不存在：%1").arg(entry->config.filePath);
        handleFault(entry, errorMessage == nullptr ? QStringLiteral("file missing") : *errorMessage);
        return false;
    }

    entry->server = new QLocalServer(this);
    QLocalServer::removeServer(entry->serverName);
    if (!entry->server->listen(entry->serverName)) {
        const QString error = QString::fromUtf8(u8"无法创建子进程 IPC 服务器：%1")
            .arg(entry->server->errorString());
        if (errorMessage != nullptr)
            *errorMessage = error;
        handleFault(entry, error);
        return false;
    }
    connect(entry->server, &QLocalServer::newConnection,
            this, &ProcessSupervisor::onServerConnection);

    entry->process = new QProcess(this);
    connect(entry->process,
            &QProcess::errorOccurred,
            this,
            &ProcessSupervisor::onProcessError);
    connect(entry->process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &ProcessSupervisor::onProcessFinished);
    entry->process->setWorkingDirectory(fileInfo.absolutePath());
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString applicationDirectory = QCoreApplication::applicationDirPath();
    environment.insert(QStringLiteral("PATH"),
                       applicationDirectory + QDir::listSeparator() + environment.value(QStringLiteral("PATH")));
    environment.insert(QStringLiteral("QT_PLUGIN_PATH"), applicationDirectory);
    environment.insert(QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"),
                       QDir(applicationDirectory).filePath(QStringLiteral("platforms")));
    entry->process->setProcessEnvironment(environment);

    QStringList arguments;
    arguments << QStringLiteral("--qframework-server") << entry->serverName
              << QStringLiteral("--qframework-token") << entry->token
              << QStringLiteral("--qframework-module-id") << entry->config.id
              << QStringLiteral("--qframework-module-type") << processTypeName(entry->config.type)
              << QStringLiteral("--qframework-shared-memory-threshold")
              << QString::number(messageBusConfig_.sharedMemoryThresholdBytes)
              << QStringLiteral("--qframework-max-message-bytes")
              << QString::number(messageBusConfig_.maxMessageBytes)
              << QStringLiteral("--qframework-shutdown-drain-timeout")
              << QString::number(messageBusConfig_.shutdownDrainTimeoutMs)
              << QStringLiteral("--qframework-debugger-timeout-ms")
              << QString::number(entry->config.debuggerWaitTimeoutMs);
    if (entry->config.waitForDebugger)
        arguments << QStringLiteral("--qframework-wait-for-debugger");

    emitState(entry, QStringLiteral("Starting"), QString());
    entry->registrationDeadlineMs = QDateTime::currentMSecsSinceEpoch() +
                                    qMax(1, processConfig_.registrationTimeoutMs);
    entry->lastPongMs = QDateTime::currentMSecsSinceEpoch();
    entry->lastPingMs = 0;
    entry->process->start(entry->config.filePath, arguments);
    if (!entry->process->waitForStarted(qMin(5000, qMax(1000, processConfig_.registrationTimeoutMs)))) {
        const QString error = QString::fromUtf8(u8"子进程启动失败：%1")
            .arg(entry->process->errorString());
        if (errorMessage != nullptr)
            *errorMessage = error;
        handleFault(entry, error);
        return false;
    }
    return true;
}

bool ProcessSupervisor::waitForRunning(Entry* entry,
                                       int timeoutMs,
                                       QString* errorMessage)
{
    if (entry == nullptr)
        return false;
    QElapsedTimer timer;
    timer.start();
    while (!entry->running && !entry->faulted && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    if (entry->running)
        return true;
    const QString detail = entry->lastError.isEmpty()
        ? QString::fromUtf8(u8"等待子进程注册或启动超时")
        : entry->lastError;
    if (errorMessage != nullptr)
        *errorMessage = detail;
    return false;
}

bool ProcessSupervisor::sendFrame(Entry* entry, const QJsonObject& frame)
{
    if (entry == nullptr || entry->socket == nullptr ||
        entry->socket->state() != QLocalSocket::ConnectedState)
        return false;
    return entry->socket->write(process::encodeFrame(frame)) >= 0;
}

void ProcessSupervisor::onServerConnection()
{
    Entry* entry = findEntryByServer(sender());
    if (entry == nullptr || entry->server == nullptr)
        return;
    while (entry->server->hasPendingConnections()) {
        QLocalSocket* socket = entry->server->nextPendingConnection();
        if (entry->socket != nullptr) {
            socket->disconnectFromServer();
            socket->deleteLater();
            continue;
        }
        entry->socket = socket;
        socket->setParent(this);
        connect(socket, &QLocalSocket::readyRead,
                this, &ProcessSupervisor::onSocketReadyRead);
        connect(socket, &QLocalSocket::disconnected,
                this, &ProcessSupervisor::onSocketDisconnected);
    }
}

void ProcessSupervisor::onSocketReadyRead()
{
    Entry* entry = findEntryBySocket(sender());
    if (entry == nullptr || entry->socket == nullptr)
        return;
    entry->inputBuffer.append(entry->socket->readAll());
    for (;;) {
        QJsonObject frame;
        QString error;
        const process::FrameResult result = process::takeFrame(
            &entry->inputBuffer,
            &frame,
            maxFrameBytes(messageBusConfig_.maxMessageBytes),
            &error);
        if (result == process::FrameResult::Incomplete)
            return;
        if (result == process::FrameResult::Invalid) {
            handleFault(entry, detailWithPrefix(QString::fromUtf8(u8"IPC 帧无效"), error));
            return;
        }
        handleFrame(entry, frame);
        if (entry->faulted || entry->stopping)
            return;
    }
}

void ProcessSupervisor::onSocketDisconnected()
{
    Entry* entry = findEntryBySocket(sender());
    if (entry == nullptr || entry->stopping || entry->faulted || shuttingDown_)
        return;
    handleFault(entry, QString::fromUtf8(u8"子进程 IPC 连接断开"));
}

void ProcessSupervisor::onProcessError(QProcess::ProcessError error)
{
    Entry* entry = findEntryByProcess(sender());
    if (entry == nullptr || entry->stopping || entry->faulted || shuttingDown_)
        return;
    if (error == QProcess::FailedToStart || error == QProcess::Crashed)
        handleFault(entry, QString::fromUtf8(u8"子进程启动或运行失败"));
}

void ProcessSupervisor::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Entry* entry = findEntryByProcess(sender());
    if (entry == nullptr || entry->stopping || entry->faulted || shuttingDown_)
        return;
    const QString detail = exitStatus == QProcess::CrashExit
        ? QString::fromUtf8(u8"子进程异常退出")
        : QString::fromUtf8(u8"子进程退出，代码 %1").arg(exitCode);
    handleFault(entry, detail);
}

void ProcessSupervisor::handleFrame(Entry* entry, const QJsonObject& frame)
{
    if (entry == nullptr)
        return;
    const QString type = frame.value(QStringLiteral("type")).toString();
    if (type == kRegister) {
        if (entry->registered) {
            handleFault(entry, QString::fromUtf8(u8"子进程重复注册"));
            return;
        }
        const QString moduleId = frame.value(QStringLiteral("moduleId")).toString();
        const QString moduleType = frame.value(QStringLiteral("moduleType")).toString();
        const QString token = frame.value(QStringLiteral("token")).toString();
        QStringList publishedTopics;
        QStringList subscribedTopics;
        if (moduleId != entry->config.id ||
            moduleType != processTypeName(entry->config.type) ||
            token != entry->token ||
            !jsonStringList(frame.value(QStringLiteral("publishedTopics")), &publishedTopics) ||
            !jsonStringList(frame.value(QStringLiteral("subscribedTopics")), &subscribedTopics)) {
            QJsonObject rejected;
            rejected.insert(QStringLiteral("type"), kRegisterAck);
            rejected.insert(QStringLiteral("accepted"), false);
            sendFrame(entry, rejected);
            handleFault(entry, QString::fromUtf8(u8"子进程注册信息校验失败"));
            return;
        }

        entry->bridge = new ProcessBridge(this,
                                          entry->config.id,
                                          publishedTopics,
                                          subscribedTopics);
        QString busError;
        if (!messageBus_->registerModule(entry->config.id, entry->bridge, &busError)) {
            delete entry->bridge;
            entry->bridge = nullptr;
            QJsonObject rejected;
            rejected.insert(QStringLiteral("type"), kRegisterAck);
            rejected.insert(QStringLiteral("accepted"), false);
            sendFrame(entry, rejected);
            handleFault(entry, detailWithPrefix(QString::fromUtf8(u8"子进程注册失败"), busError));
            return;
        }
        entry->registered = true;
        entry->lastPongMs = QDateTime::currentMSecsSinceEpoch();
        // 允许 onStart() 发布，交付是否开始仍由上层统一控制。
        messageBus_->setModuleRunning(entry->config.id, true);
        QJsonObject accepted;
        accepted.insert(QStringLiteral("type"), kRegisterAck);
        accepted.insert(QStringLiteral("accepted"), true);
        if (entry->config.type == ModuleType::ProcessUi)
            accepted.insert(QStringLiteral("styleSheet"), styleSheet_);
        sendFrame(entry, accepted);
        return;
    }
    if (!entry->registered)
        return;
    if (type == kStarted) {
        entry->running = true;
        entry->faulted = false;
        entry->lastPongMs = QDateTime::currentMSecsSinceEpoch();
        entry->lastPingMs = 0;
        emitState(entry, QStringLiteral("Running"), QString());
        return;
    }
    if (type == kStartFailed) {
        messageBus_->setModuleRunning(entry->config.id, false);
        handleFault(entry,
                    detailWithPrefix(QString::fromUtf8(u8"子进程 onStart 失败"),
                                     frame.value(QStringLiteral("detail")).toString()));
        return;
    }
    if (type == kPong) {
        entry->lastPongMs = QDateTime::currentMSecsSinceEpoch();
        return;
    }
    if (type == kStopAck) {
        entry->stopAcknowledged = true;
        return;
    }
    if (type == kDebugWaitTimeout) {
        Logger::instance().log(LogLevel::Warning,
                               entry->config.id,
                               QString::fromUtf8(u8"等待调试器超时，继续启动"));
        return;
    }
    if (type == kWindowReady) {
        const QString value = frame.value(QStringLiteral("windowId")).toString();
        bool ok = false;
        const quintptr windowId = static_cast<quintptr>(value.toULongLong(&ok));
        if (ok)
            emit windowHandleReady(entry->config.id, windowId);
        return;
    }
    if (type == kSharedAck) {
        const QString messageId = frame.value(QStringLiteral("messageId")).toString();
        QSharedMemory* shared = entry->outgoingShared.take(messageId);
        if (shared != nullptr) {
            shared->detach();
            delete shared;
        }
        return;
    }
    if (type == kLog) {
        const int levelValue = frame.value(QStringLiteral("level")).toInt();
        const LogLevel level = levelValue >= static_cast<int>(LogLevel::Debug) &&
                               levelValue <= static_cast<int>(LogLevel::Error)
            ? static_cast<LogLevel>(levelValue)
            : LogLevel::Warning;
        messageBus_->logFromModule(level,
                                   entry->config.id,
                                   frame.value(QStringLiteral("text")).toString());
        return;
    }
    if (type != kMessage)
        return;

    const QString topic = frame.value(QStringLiteral("topic")).toString();
    const QString messageId = frame.value(QStringLiteral("messageId")).toString();
    const QString transport = frame.value(QStringLiteral("transport")).toString();
    QByteArray data;
    if (transport == QStringLiteral("shared")) {
        const QString key = frame.value(QStringLiteral("sharedKey")).toString();
        QSharedMemory shared(key);
        if (shared.attach(QSharedMemory::ReadOnly)) {
            if (shared.lock()) {
                const int declaredSize = frame.value(QStringLiteral("size")).toInt();
                const int copySize = qMin(shared.size(), qMax(0, declaredSize));
                if (copySize <= messageBusConfig_.maxMessageBytes)
                    data = QByteArray(static_cast<const char*>(shared.constData()), copySize);
                shared.unlock();
            }
            shared.detach();
        }
        QJsonObject ack;
        ack.insert(QStringLiteral("type"), kSharedAck);
        ack.insert(QStringLiteral("messageId"), messageId);
        sendFrame(entry, ack);
    } else {
        data = QByteArray::fromBase64(
            frame.value(QStringLiteral("data")).toString().toLatin1());
    }
    if (data.size() > messageBusConfig_.maxMessageBytes)
        return;
    messageBus_->publishFromModule(entry->config.id, topic, data);
}

void ProcessSupervisor::onSupervisionTick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (Entry* entry : entries_) {
        if (entry->restartAtMs > 0 && now >= entry->restartAtMs && !shuttingDown_) {
            entry->restartAtMs = 0;
            QString error;
            if (!startEntry(entry, &error))
                entry->lastError = error;
            continue;
        }
        if (entry->faulted || entry->stopping || entry->process == nullptr)
            continue;
        if (!entry->registered) {
            if (now > entry->registrationDeadlineMs)
                handleFault(entry, QString::fromUtf8(u8"子进程注册超时"));
            continue;
        }
        if (entry->socket == nullptr || entry->socket->state() != QLocalSocket::ConnectedState)
            continue;
        if (entry->config.waitForDebugger &&
            !entry->running &&
            now <= entry->registrationDeadlineMs +
                       qMax(1, entry->config.debuggerWaitTimeoutMs)) {
            continue;
        }
        if (now - entry->lastPingMs >= qMax(1, processConfig_.heartbeatIntervalMs)) {
            QJsonObject ping;
            ping.insert(QStringLiteral("type"), kPing);
            sendFrame(entry, ping);
            entry->lastPingMs = now;
        }
        if (now - entry->lastPongMs > qMax(1, processConfig_.heartbeatTimeoutMs))
            handleFault(entry, QString::fromUtf8(u8"子进程心跳超时"));
    }
}

void ProcessSupervisor::sendMessageToChild(const QString& moduleId,
                                           const QString& topic,
                                           const QString& senderModuleId,
                                           const QByteArray& data)
{
    Entry* entry = findEntry(moduleId);
    if (entry == nullptr || !entry->registered || entry->socket == nullptr ||
        entry->socket->state() != QLocalSocket::ConnectedState ||
        data.size() > messageBusConfig_.maxMessageBytes)
        return;

    const QString messageId = QUuid::createUuid().toString(QUuid::Id128);
    QJsonObject frame;
    frame.insert(QStringLiteral("type"), kMessage);
    frame.insert(QStringLiteral("topic"), topic);
    frame.insert(QStringLiteral("senderModuleId"), senderModuleId);
    frame.insert(QStringLiteral("messageId"), messageId);
    if (data.size() >= messageBusConfig_.sharedMemoryThresholdBytes) {
        const QString key = makeSharedKey(moduleId);
        QSharedMemory* shared = new QSharedMemory(key, this);
        if (!shared->create(data.size())) {
            delete shared;
            Logger::instance().log(LogLevel::Error,
                                   moduleId,
                                   QString::fromUtf8(u8"创建发送共享内存失败"));
            return;
        }
        if (!shared->lock()) {
            shared->detach();
            delete shared;
            return;
        }
        std::memcpy(shared->data(), data.constData(), static_cast<size_t>(data.size()));
        shared->unlock();
        entry->outgoingShared.insert(messageId, shared);
        frame.insert(QStringLiteral("transport"), QStringLiteral("shared"));
        frame.insert(QStringLiteral("sharedKey"), key);
        frame.insert(QStringLiteral("size"), data.size());
    } else {
        frame.insert(QStringLiteral("transport"), QStringLiteral("inline"));
        frame.insert(QStringLiteral("data"), QString::fromLatin1(data.toBase64()));
    }
    sendFrame(entry, frame);
}

void ProcessSupervisor::handleFault(Entry* entry, const QString& detail)
{
    if (entry == nullptr || entry->faulted)
        return;
    entry->faulted = true;
    entry->running = false;
    entry->lastError = detail;
    destroyRuntime(entry);
    emitState(entry, QStringLiteral("Failed"), detail);

    if (shuttingDown_ || !entry->config.enabled)
        return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (entry->restartWindowStartMs == 0 ||
        now - entry->restartWindowStartMs > qMax(1, processConfig_.restartWindowMs)) {
        entry->restartWindowStartMs = now;
        entry->restartCount = 0;
    }
    if (entry->restartCount < qMax(0, processConfig_.maxRestartCount)) {
        ++entry->restartCount;
        entry->restartAtMs = now + qMax(0, processConfig_.restartDelayMs);
        emitState(entry,
                  QStringLiteral("Restarting"),
                  QString::fromUtf8(u8"已排队自动重启"));
    } else {
        emit moduleFault(entry->config.id, detail);
    }
}

void ProcessSupervisor::destroyRuntime(Entry* entry)
{
    if (entry == nullptr)
        return;
    ProcessBridge* bridge = entry->bridge;
    QLocalSocket* socket = entry->socket;
    QLocalServer* server = entry->server;
    QProcess* process = entry->process;
    entry->bridge = nullptr;
    entry->socket = nullptr;
    entry->server = nullptr;
    entry->process = nullptr;

    if (bridge != nullptr) {
        messageBus_->setModuleRunning(entry->config.id, false);
        messageBus_->unregisterModule(entry->config.id, false);
        delete bridge;
    }
    if (socket != nullptr) {
        socket->disconnect(this);
        socket->disconnectFromServer();
        socket->close();
        socket->deleteLater();
    }
    if (server != nullptr) {
        const QString name = entry->serverName;
        server->disconnect(this);
        server->close();
        server->deleteLater();
        QLocalServer::removeServer(name);
    }
    if (process != nullptr) {
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
        process->deleteLater();
    }
    clearOutgoingShared(entry);
    entry->inputBuffer.clear();
    entry->registered = false;
    entry->running = false;
    entry->stopAcknowledged = false;
}

void ProcessSupervisor::clearOutgoingShared(Entry* entry)
{
    const QList<QSharedMemory*> segments = entry->outgoingShared.values();
    entry->outgoingShared.clear();
    for (QSharedMemory* shared : segments) {
        shared->detach();
        delete shared;
    }
}

void ProcessSupervisor::emitState(Entry* entry,
                                  const QString& stateValue,
                                  const QString& detail)
{
    entry->state = stateValue;
    emit moduleStateChanged(entry->config.id, stateValue, detail);
}
}
