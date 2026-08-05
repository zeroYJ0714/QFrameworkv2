// 本文件是 QFramework 的综合回归测试实现。
//
// 初学者可以把它理解成一份“可执行的框架使用说明”：每个测试先搭建一个
// 很小的运行环境，再执行一次真实操作，最后用 QCOMPARE/QVERIFY 检查结果。
// 除普通 Qt Test 身份外，本可执行文件还会被 ProcessSupervisor 作为测试子进程
// 再次启动，用来验证注册、心跳、双向消息、ACK 和共享内存等跨进程行为。
#include "tst_Baseline.h"

#include <QApplication>
#include <QAction>
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
#include <QThread>
#include <QTimer>
#include <QWaitCondition>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "FrameworkConfig.h"
#include "FrameworkRuntime.h"
#include "InProcessUiModule.h"
#include "InProcessNonUiModule.h"
#include "ProcessNonUiModule.h"
#include "ProcessRuntime.h"
#include "Logger.h"
#include "LayoutManager.h"
#include "MainWindow.h"
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
// 查询原生 Win32 窗口客户区大小，供“子进程窗口嵌入”测试确认 resize
// 命令确实落到了子窗口。windowId 来自 ProcessSupervisor 上报的原生句柄；
// 句柄无效时返回空 QSize，让测试给出清晰失败，而不是继续访问无效窗口。
QSize nativeWindowClientSize(quintptr windowId)
{
    const HWND handle = reinterpret_cast<HWND>(windowId);
    RECT rect = {};
    if (handle == nullptr || !IsWindow(handle) || !GetClientRect(handle, &rect))
        return QSize();
    return QSize(rect.right - rect.left, rect.bottom - rect.top);
}

// 原生可见性可区分“只收到 windowReady 并完成嵌入”和“随后真正执行 showWindow”。
// IsWindowVisible 会同时考虑父窗口链，因此隐藏 Dock 中的子窗口仍按不可见处理。
bool nativeWindowVisible(quintptr windowId)
{
    const HWND handle = reinterpret_cast<HWND>(windowId);
    return handle != nullptr && IsWindow(handle) && IsWindowVisible(handle);
}
#endif

// 最小化的模块宿主替身。
//
// 正式运行时 ModuleEndpoint 会把 publish()/log() 转交给 MessageBus 和 Logger；
// 生命周期单元测试不需要启动整套框架，因此由 FakeHost 只记录最后一次调用。
// 这些公开字段就是测试的“观测口”，不承担生产逻辑。
class FakeHost : public qframework::ModuleHost
{
public:
    // 记录发布者、主题和负载，并返回 true 模拟宿主接受消息。
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

    // 记录模块日志。该测试不关心级别，所以用 Q_UNUSED 明确忽略它。
    void logFromModule(qframework::LogLevel level,
                       const QString& moduleId,
                       const QString& text) override
    {
        Q_UNUSED(level)
        lastModuleId = moduleId;
        lastText = text;
    }

    // 以下字段保存最近一次宿主调用，便于 QCOMPARE 逐项验证转发参数。
    // 最近一次 publish/log 调用的模块身份。
    QString lastModuleId;
    // 最近一次发布的主题和完整负载。
    QString lastTopic;
    QByteArray lastData;
    // 最近一次日志文本及累计发布次数。
    QString lastText;
    int publishCount = 0;
};

// 用于验证 ModuleEndpoint 默认生命周期规则的最小非 UI 模块。
// 它只声明一个可发布主题，不覆盖启动/停止行为，因此可以专门观察基类默认值。
class TestNonUiModule : public qframework::InProcessNonUiModule
{
public:
    // 继承基类构造函数，避免测试替身引入无关初始化逻辑。
    using qframework::InProcessNonUiModule::InProcessNonUiModule;

    // 框架只允许模块发布自己声明过的主题。
    QStringList publishedTopics() const override
    {
        return QStringList() << QStringLiteral("TEST_TOPIC");
    }
};

// 一条已送达测试模块的消息快照。
// QByteArray 在这里按值保存，使断言不依赖 MessageBus 回调参数的生命周期。
struct ReceivedMessage
{
    // 主题名决定消息走哪条队列规则。
    QString topic;
    // 发送者 ID 用来验证转发链路没有丢失来源。
    QString sender;
    // 复制后的业务负载；测试不保留底层 Socket/共享内存指针。
    QByteArray data;
};

// MessageBus 测试模块：既可以充当发布者，也可以充当订阅者。
//
// onMessage() 可能由消息队列工作线程调用，而测试断言运行在主线程，故接收数组
// 必须由 QMutex 保护；QWaitCondition 让测试在有限超时内等待目标数量，避免忙等。
class BusTestModule : public qframework::InProcessNonUiModule
{
public:
    // published/subscribed 分别定义允许发布和希望订阅的主题集合。
    BusTestModule(const QStringList& published, const QStringList& subscribed)
        : published_(published),
          subscribed_(subscribed)
    {
    }

    // MessageBus 注册模块时会读取这两份声明来建立权限和路由关系。
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

    // 等待至少 count 条消息，最多等待 timeoutMs 毫秒。
    // 每轮按已消耗时间重新计算 remaining，可抵抗条件变量的提前唤醒。
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

    // 返回接收数组副本；复制发生在锁内，调用方随后可以无锁检查内容。
    QVector<ReceivedMessage> received() const
    {
        QMutexLocker locker(&mutex_);
        return received_;
    }

private:
    // 主题声明只在构造时写入，后三项是跨线程共享的接收状态。
    QStringList published_;
    QStringList subscribed_;
    mutable QMutex mutex_;
    QWaitCondition changed_;
    QVector<ReceivedMessage> received_;
};

// 主进程卡死回调替身：进入 onMessage 后等待测试显式 release，模拟 DLL 业务卡死。
class BlockingBusModule : public qframework::InProcessNonUiModule
{
public:
    QStringList subscribedTopics() const override
    {
        return QStringList() << QStringLiteral("TEST_BLOCKING_CALLBACK");
    }

    void onMessage(const QString&, const QString&, const QByteArray&) override
    {
        QMutexLocker locker(&mutex_);
        entered_ = true;
        changed_.wakeAll();
        while (!released_)
            changed_.wait(&mutex_);
    }

    bool waitUntilEntered(int timeoutMs)
    {
        QMutexLocker locker(&mutex_);
        if (!entered_)
            changed_.wait(&mutex_, static_cast<unsigned long>(qMax(1, timeoutMs)));
        return entered_;
    }

    void release()
    {
        QMutexLocker locker(&mutex_);
        released_ = true;
        changed_.wakeAll();
    }

private:
    QMutex mutex_;
    QWaitCondition changed_;
    bool entered_ = false;
    bool released_ = false;
};

QMutex failFastRecordMutex;
bool failFastRequested = false;
QString failFastReason;

void recordFailFastRequest(const QString& reason)
{
    QMutexLocker locker(&failFastRecordMutex);
    failFastRequested = true;
    failFastReason = reason;
}

// 测试子进程中的生产者只调用公开 publish()；停止标志由 Qt 互斥锁保护。
class RuntimePublishWorker final : public QThread
{
public:
    explicit RuntimePublishWorker(qframework::ModuleEndpoint* module)
        : module_(module)
    {
    }

    void requestFinish()
    {
        QMutexLocker locker(&mutex_);
        finishing_ = true;
    }

protected:
    void run() override
    {
        quint64 sequence = 0;
        for (;;) {
            {
                QMutexLocker locker(&mutex_);
                if (finishing_)
                    return;
            }
            module_->publish(QStringLiteral("TEST_CHILD_STRESS"),
                             QByteArray::number(++sequence));
            QThread::msleep(1);
        }
    }

private:
    qframework::ModuleEndpoint* module_;
    QMutex mutex_;
    bool finishing_ = false;
};

// 运行在测试子进程中的模块。
//
// mode_ 由模块 ID 决定，同一个轻量实现可覆盖 Latest、Reliable、inline 和
// shared-memory 四类 IPC 场景。测试消息使用独立主题，避免不同场景互相干扰。
class RuntimeQueueTestModule final : public qframework::ProcessNonUiModule
{
public:
    // parent 继续交给 QObject 管理；mode 用于选择本次子进程要执行的场景。
    explicit RuntimeQueueTestModule(const QString& mode,
                                    QObject* parent = nullptr)
        : qframework::ProcessNonUiModule(parent),
          mode_(mode)
    {
    }

    ~RuntimeQueueTestModule() override
    {
        stopPublishWorkers();
    }

    // 只声明当前场景需要的发布主题，顺便验证框架的主题权限检查没有被绕过。
    QStringList publishedTopics() const override
    {
        if (mode_ == QStringLiteral("RuntimeHungCallback"))
            return QStringList() << QStringLiteral("TEST_CHILD_HUNG_ENTERED");
        if (mode_ == QStringLiteral("RuntimePublishStress"))
            return QStringList() << QStringLiteral("TEST_CHILD_STRESS");
        if (mode_ == QStringLiteral("RuntimeQueueLatest"))
            return QStringList() << QStringLiteral("TEST_CHILD_RESULT")
                                 << QStringLiteral("TEST_CHILD_LATEST");
        if (mode_ == QStringLiteral("RuntimeQueueReliable"))
            return QStringList() << QStringLiteral("TEST_CHILD_RESULT")
                                 << QStringLiteral("TEST_CHILD_RELIABLE");
        return QStringList() << QStringLiteral("TEST_CHILD_LATEST")
                             << QStringLiteral("TEST_CHILD_RELIABLE")
                             << QStringLiteral("TEST_CHILD_INLINE")
                             << QStringLiteral("TEST_CHILD_SHARED")
                             << QStringLiteral("TEST_CHILD_REPLY_INLINE")
                             << QStringLiteral("TEST_CHILD_REPLY_SHARED");
    }

    // 父到子测试需要订阅父进程主题；子到父启动发布场景不需要额外订阅。
    QStringList subscribedTopics() const override
    {
        if (mode_ == QStringLiteral("RuntimeHungCallback"))
            return QStringList() << QStringLiteral("TEST_PARENT_HUNG");
        if (mode_ == QStringLiteral("RuntimeQueueLatest"))
            return QStringList() << QStringLiteral("TEST_PARENT_LATEST");
        if (mode_ == QStringLiteral("RuntimeQueueReliable"))
            return QStringList() << QStringLiteral("TEST_PARENT_RELIABLE");
        return QStringList() << QStringLiteral("TEST_PARENT_INLINE")
                             << QStringLiteral("TEST_PARENT_SHARED");
    }

    // 模块进入 Running 前执行一次场景初始化；返回 false 会被框架视为启动失败。
    bool onStart() override
    {
        if (mode_ == QStringLiteral("RuntimeHungCallback"))
            return true;
        if (mode_ == QStringLiteral("RuntimePublishStress")) {
            for (int index = 0; index < 4; ++index) {
                RuntimePublishWorker* worker = new RuntimePublishWorker(this);
                publishWorkers_.append(worker);
                worker->start();
            }
            return true;
        }
        if (mode_ == QStringLiteral("RuntimeQueueLatest")) {
            // 子进程启动时连续产生 100 帧；父进程的输入队列容量为 1，
            // 配合下面 120 ms 的慢回调，足以让覆盖逻辑而不是“消费者速度”成为主因。
            for (int sequence = 1; sequence <= 100; ++sequence)
                publish(QStringLiteral("TEST_CHILD_LATEST"),
                        QByteArray::number(sequence));
            return true;
        }
        if (mode_ == QStringLiteral("RuntimeQueueReliable")) {
            // Reliable 容量为 1：第一条进入本地队列后，第二条应立即被拒绝，
            // 且第一条仍须按原值送达，不能被第二条覆盖。
            const bool firstAccepted = publish(
                QStringLiteral("TEST_CHILD_RELIABLE"), QByteArrayLiteral("first"));
            const bool secondAccepted = publish(
                QStringLiteral("TEST_CHILD_RELIABLE"), QByteArrayLiteral("second"));
            publish(QStringLiteral("TEST_CHILD_RESULT"),
                    QByteArrayLiteral("localReliable:") +
                        QByteArray::number(firstAccepted ? 1 : 0) +
                        QByteArrayLiteral(",") +
                        QByteArray::number(secondAccepted ? 1 : 0));
            return firstAccepted;
        }
        if (mode_ == QStringLiteral("RuntimeQueueInlineShared")) {
            // 1024 字节大于测试阈值 128，强制走共享内存；短字符串则走 inline。
            publish(QStringLiteral("TEST_CHILD_INLINE"),
                    QByteArrayLiteral("child-inline"));
            publish(QStringLiteral("TEST_CHILD_SHARED"),
                    QByteArray(1024, 'C'));
            return true;
        }
        return false;
    }

    void onStop() override
    {
        stopPublishWorkers();
    }

    // 在锁内复制消息并唤醒等待者，确保测试线程看见一份完整快照。
    // 收到父进程消息后按主题选择慢消费或原样回显，用回包证明数据真正到达。
    void onMessage(const QString& topic,
                   const QString& senderModuleId,
                   const QByteArray& data) override
    {
        Q_UNUSED(senderModuleId)
        if (topic == QStringLiteral("TEST_PARENT_HUNG")) {
            publish(QStringLiteral("TEST_CHILD_HUNG_ENTERED"), QByteArrayLiteral("entered"));
            QMutexLocker locker(&hungMutex_);
            while (!hungReleased_)
                hungCondition_.wait(&hungMutex_);
        } else if (topic == QStringLiteral("TEST_PARENT_LATEST")) {
            // 人为放慢消费者，让父到子等待队列在 ACK 释放之前达到容量。
            QThread::msleep(120);
            publish(QStringLiteral("TEST_CHILD_RESULT"),
                    topic.toUtf8() + QByteArrayLiteral(":") + data);
        } else if (topic == QStringLiteral("TEST_PARENT_RELIABLE")) {
            // Reliable 测试使用更慢的消费者，观察旧帧是否保持以及发送顺序。
            QThread::msleep(250);
            publish(QStringLiteral("TEST_CHILD_RESULT"),
                    topic.toUtf8() + QByteArrayLiteral(":") + data);
        } else if (topic == QStringLiteral("TEST_PARENT_INLINE")) {
            publish(QStringLiteral("TEST_CHILD_REPLY_INLINE"), data);
        } else if (topic == QStringLiteral("TEST_PARENT_SHARED")) {
            publish(QStringLiteral("TEST_CHILD_REPLY_SHARED"), data);
        }
    }

private:
    void stopPublishWorkers()
    {
        for (RuntimePublishWorker* worker : publishWorkers_)
            worker->requestFinish();
        for (RuntimePublishWorker* worker : publishWorkers_) {
            // 测试模块也遵守有界等待；失败就让子进程测试明确失败，不强杀线程。
            if (!worker->wait(2000))
                qFatal("RuntimePublishWorker did not stop within 2000 ms");
            delete worker;
        }
        publishWorkers_.clear();
    }

    // 保存构造时选定的测试模式，子进程生命周期内不再修改。
    QString mode_;
    QVector<RuntimePublishWorker*> publishWorkers_;
    QMutex hungMutex_;
    QWaitCondition hungCondition_;
    bool hungReleased_ = false;
};

// 返回测试使用的只读 INI 路径。
// CI 或开发者可用 QFRAMEWORK_CONFIG_PATH 显式指定；未指定时按测试程序相对
// 目录回到仓库 config/QFramework.ini，避免依赖启动时工作目录。
QString fixedConfigPath()
{
    const QByteArray configured = qgetenv("QFRAMEWORK_CONFIG_PATH");
    if (!configured.isEmpty())
        return QString::fromUtf8(configured);
    return QDir::cleanPath(QCoreApplication::applicationDirPath()
                           + QStringLiteral("/../../../../config/QFramework.ini"));
}

// 根据模块 ID 计算构建产物中的进程内插件 DLL 路径。
// 这里只拼路径，不检查文件；具体测试会用 QFileInfo 给出缺失产物断言。
QString pluginPath(const QString& moduleId)
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("Plugins/%1/%1.dll").arg(moduleId));
}

// 生成一个最小进程内模块配置。
// enabled 可用于验证“禁用模块不会被加载”，其余字段与真实配置结构一致。
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

// 根据模块 ID 计算独立进程模块 EXE 的构建产物路径。
QString processModulePath(const QString& moduleId)
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("Plugins/%1/%1.exe").arg(moduleId));
}

// 生成独立进程模块配置；调用方负责传入 ProcessUi 或 ProcessNonUi 类型。
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

// 统计 moduleStateChanged 信号中“指定模块 + 指定状态”的出现次数。
// QSignalSpy 每一项都是一次信号的 QVariant 参数列表；少于两个参数的异常项忽略。
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

// 判断本测试程序是否由 ProcessSupervisor 以子进程身份启动。
// 只检查服务名开关是否存在，具体参数完整性由 runFaultProcessClient 再验证。
bool hasSupervisorArguments(int argc, char* argv[])
{
    for (int index = 1; index < argc; ++index) {
        if (QByteArray(argv[index]) == QByteArrayLiteral("--qframework-server"))
            return true;
    }
    return false;
}

// 读取形如“--参数 值”的命令行参数。
// 参数不存在或末尾缺值时返回空字符串，让调用方统一走可预测的错误码。
QString supervisorArgumentValue(const QStringList& arguments, const QString& name)
{
    const int index = arguments.indexOf(name);
    if (index < 0 || index + 1 >= arguments.size())
        return QString();
    return arguments.at(index + 1);
}

// 把一帧测试协议写入本地套接字，并最多等待 1000 ms。
// 返回 false 表示空套接字、短写或超时；所有等待都有上限，测试不会永久卡住。
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

// 以“受监督子进程”身份运行当前测试程序。
//
// RuntimeQueue* 模块进入真实 ProcessRuntime；其他特殊模块则手工发送注册帧，
// 有意制造错误 token、注册超时或心跳超时。不同非零返回码标明失败阶段，便于
// 测试日志定位是参数、连接、编码、注册还是 started 发送失败。
int runFaultProcessClient(int argc, char* argv[])
{
    // 子进程只创建 QCoreApplication，故障协议场景不需要 QWidget 事件循环。
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
    // 20 表示监督器注入参数不完整，属于测试客户端自身启动错误。
    if (serverName.isEmpty() || token.isEmpty() || moduleId.isEmpty() || moduleType.isEmpty())
        return 20;
    if (moduleId.startsWith(QStringLiteral("RuntimeQueue")) ||
        moduleId == QStringLiteral("RuntimeHungCallback") ||
        moduleId == QStringLiteral("RuntimePublishStress")) {
        // 队列专项场景必须走真实 ProcessRuntime，才能覆盖内部 ACK 和共享内存。
        return qframework::ProcessRuntime::run(
            &application,
            new RuntimeQueueTestModule(moduleId));
    }

    QLocalSocket socket;
    socket.connectToServer(serverName);
    // 21 表示在 2 秒连接窗口内未找到监督器服务端。
    if (!socket.waitForConnected(2000))
        return 21;

    // 注册超时场景只连接不发送注册帧，让父进程验证自己的 deadline。
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
    QJsonArray subscribedTopics;
    if (moduleId == QStringLiteral("IngressGateModule"))
        subscribedTopics.append(QStringLiteral("TEST_STOP_GATE"));
    if (moduleId == QStringLiteral("AccountingModule"))
        subscribedTopics.append(QStringLiteral("TEST_ACCOUNTING"));
    registration.insert(QStringLiteral("subscribedTopics"), subscribedTopics);
    // 22 表示注册帧没有完整写入本地 Socket。
    if (!writeFaultProcessFrame(&socket, registration))
        return 22;

    // 无效 token 场景发送完故意错误的注册帧后保持存活，等待监督器拒绝。
    if (moduleId == QStringLiteral("InvalidTokenModule"))
        return application.exec();

    QByteArray inputBuffer;
    QElapsedTimer timer;
    timer.start();
    bool accepted = false;
    while (!accepted && timer.elapsed() < 2000) {
        // 先泵送事件再读 Socket，兼容 QLocalSocket 的异步 readyRead 信号。
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
            // 23 表示监督器回传的控制帧本身损坏，24 表示 2 秒内没有合法 ACK。
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

    const bool lifecycleClient =
        moduleId == QStringLiteral("AsyncRestartModule") ||
        moduleId == QStringLiteral("DuplicateRestartModule") ||
        moduleId == QStringLiteral("IngressGateModule") ||
        moduleId == QStringLiteral("DeadlineModuleA") ||
        moduleId == QStringLiteral("DeadlineModuleB") ||
        moduleId == QStringLiteral("StopAckModule") ||
        moduleId == QStringLiteral("AccountingModule") ||
        moduleId == QStringLiteral("StartupAsyncModule") ||
        moduleId.startsWith(QStringLiteral("ShutdownHung"));
    if (lifecycleClient) {
        const bool ignoreStop = moduleId == QStringLiteral("DeadlineModuleA") ||
                                moduleId == QStringLiteral("DeadlineModuleB") ||
                                moduleId.startsWith(QStringLiteral("ShutdownHung"));
        const int stopExitDelayMs = moduleId == QStringLiteral("AsyncRestartModule")
            ? 350 : (moduleId == QStringLiteral("StopAckModule") ? 250 : 50);
        const bool ignoreMessageAcknowledgements =
            moduleId == QStringLiteral("AccountingModule");
        bool stopScheduled = false;
        QObject::connect(
            &socket,
            &QLocalSocket::readyRead,
            &application,
            [&socket, &application, &inputBuffer, &stopScheduled,
             ignoreStop, stopExitDelayMs, ignoreMessageAcknowledgements]() {
                inputBuffer.append(socket.readAll());
                for (;;) {
                    QJsonObject frame;
                    QString error;
                    const qframework::process::FrameResult result =
                        qframework::process::takeFrame(
                            &inputBuffer, &frame, 1024 * 1024, &error);
                    if (result == qframework::process::FrameResult::Incomplete)
                        return;
                    if (result == qframework::process::FrameResult::Invalid) {
                        application.exit(26);
                        return;
                    }
                    const QString type = frame.value(QStringLiteral("type")).toString();
                    if (type == QStringLiteral("ping")) {
                        QJsonObject pong;
                        pong.insert(QStringLiteral("type"), QStringLiteral("pong"));
                        if (!writeFaultProcessFrame(&socket, pong))
                            application.exit(27);
                    } else if (type == QStringLiteral("message")) {
                        if (ignoreMessageAcknowledgements)
                            continue;
                        QJsonObject ack;
                        ack.insert(QStringLiteral("type"), QStringLiteral("deliveryAck"));
                        ack.insert(QStringLiteral("messageId"),
                                   frame.value(QStringLiteral("messageId")));
                        ack.insert(QStringLiteral("accepted"), true);
                        if (!writeFaultProcessFrame(&socket, ack))
                            application.exit(28);
                    } else if (type == QStringLiteral("stop") && !ignoreStop &&
                               !stopScheduled) {
                        QJsonObject ack;
                        ack.insert(QStringLiteral("type"), QStringLiteral("stopAck"));
                        if (!writeFaultProcessFrame(&socket, ack)) {
                            application.exit(29);
                            return;
                        }
                        stopScheduled = true;
                        QTimer::singleShot(stopExitDelayMs,
                                           &application,
                                           &QCoreApplication::quit);
                    }
                }
            });
        return application.exec();
    }

    // 故意不读取后续 ping，监督器应按心跳超时结束该进程。
    return application.exec();
}

// 为故障监督测试创建小而有界的 MessageBus 配置。
// 这些测试不传输大数据，只需保证故障原因来自进程协议而不是消息大小限制。
qframework::MessageBusConfig faultBusConfig()
{
    qframework::MessageBusConfig config;
    config.defaultQueueCapacity = 8;
    config.maxMessageBytes = 1024 * 1024;
    config.sharedMemoryThresholdBytes = 256;
    config.shutdownDrainTimeoutMs = 200;
    return config;
}

// 创建单个队列主题配置。
// capacity 决定最多等待多少条，policy 决定满时覆盖旧帧还是拒绝新消息。
qframework::TopicConfig queueTopicConfig(int capacity,
                                         qframework::QueuePolicy policy)
{
    qframework::TopicConfig config;
    config.queueCapacity = capacity;
    config.maxMessageBytes = 4096;
    config.policy = policy;
    return config;
}

// 构造跨进程队列专项测试配置。
// 容量和共享内存阈值刻意压小，使覆盖、拒绝、inline/shared 分流和 ACK 回收
// 能在自动化测试的有限时间内稳定发生。
qframework::MessageBusConfig queueTestBusConfig()
{
    // 测试故意使用很小的容量和共享内存阈值，让覆盖、拒绝、ACK 回收以及
    // 两种 IPC 传输在几秒内稳定暴露；生产配置仍由 QFramework.ini 决定。
    qframework::MessageBusConfig config;
    config.defaultQueueCapacity = 16;
    config.maxMessageBytes = 4096;
    config.sharedMemoryThresholdBytes = 128;
    config.shutdownDrainTimeoutMs = 2000;
    config.defaultPolicy = qframework::QueuePolicy::Reliable;
    config.topics.insert(
        QStringLiteral("TEST_PARENT_LATEST"),
        queueTopicConfig(1, qframework::QueuePolicy::Latest));
    config.topics.insert(
        QStringLiteral("TEST_CHILD_LATEST"),
        queueTopicConfig(1, qframework::QueuePolicy::Latest));
    config.topics.insert(
        QStringLiteral("TEST_PARENT_RELIABLE"),
        queueTopicConfig(1, qframework::QueuePolicy::Reliable));
    config.topics.insert(
        QStringLiteral("TEST_CHILD_RELIABLE"),
        queueTopicConfig(1, qframework::QueuePolicy::Reliable));
    const QStringList reliableTopics = QStringList()
        << QStringLiteral("TEST_CHILD_RESULT")
        << QStringLiteral("TEST_PARENT_INLINE")
        << QStringLiteral("TEST_PARENT_SHARED")
        << QStringLiteral("TEST_CHILD_INLINE")
        << QStringLiteral("TEST_CHILD_SHARED")
        << QStringLiteral("TEST_CHILD_REPLY_INLINE")
        << QStringLiteral("TEST_CHILD_REPLY_SHARED")
        << QStringLiteral("TEST_PARENT_HUNG")
        << QStringLiteral("TEST_CHILD_HUNG_ENTERED")
        << QStringLiteral("TEST_CHILD_STRESS")
        << QStringLiteral("TEST_ACCOUNTING");
    for (const QString& topic : reliableTopics) {
        config.topics.insert(
            topic,
            queueTopicConfig(8, qframework::QueuePolicy::Reliable));
    }
    return config;
}

// 构造故障测试的短超时配置，使无注册或无心跳场景快速结束。
// maxRestartCount=0 禁止自动重启，确保一次故障只产生一组可核对信号。
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

// 异步生命周期测试使用较长心跳窗口，避免故意拖延 stop 的客户端先被心跳判故障。
qframework::ProcessConfig lifecycleProcessConfig(int stopTimeoutMs = 300)
{
    qframework::ProcessConfig config;
    config.registrationTimeoutMs = 2000;
    config.heartbeatIntervalMs = 50;
    config.heartbeatTimeoutMs = 5000;
    config.stopTimeoutMs = stopTimeoutMs;
    config.restartDelayMs = 20;
    config.restartWindowMs = 2000;
    config.maxRestartCount = 0;
    return config;
}

// 构造正常 IPC 测试的宽松超时配置。
// 与故障测试相比，这里给进程启动、ACK 和队列排空留下足够时间，降低慢机器抖动。
qframework::ProcessConfig queueTestProcessConfig()
{
    qframework::ProcessConfig config;
    config.registrationTimeoutMs = 5000;
    config.heartbeatIntervalMs = 100;
    config.heartbeatTimeoutMs = 2000;
    config.stopTimeoutMs = 3000;
    config.restartDelayMs = 100;
    config.restartWindowMs = 5000;
    config.maxRestartCount = 0;
    return config;
}

// 把当前 QFrameworkTests.exe 配置成一个 ProcessNonUi 测试模块。
// 监督器再次启动同一程序后，main() 会根据命令行切换到子进程测试入口。
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

// 生成队列测试模块配置。目前仅沿用故障模块的可执行路径和类型，单独保留
// 此函数是为了让测试意图清晰，也便于未来给队列场景增加专属参数。
qframework::ModuleConfig queueTestModuleConfig(const QString& moduleId)
{
    qframework::ModuleConfig config = faultProcessModuleConfig(moduleId);
    config.displayName = moduleId;
    return config;
}

// 在 moduleFault 信号记录中查找指定模块及原因片段。
// 使用 contains 而非完整相等，是为了允许错误信息携带额外上下文。
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

// 判断消息快照中是否存在“主题和负载均相等”的记录。
bool messagesContain(const QVector<ReceivedMessage>& messages,
                     const QString& topic,
                     const QByteArray& data)
{
    for (const ReceivedMessage& message : messages) {
        if (message.topic == topic && message.data == data)
            return true;
    }
    return false;
}

// 统计指定主题的送达次数，用于证明 Latest 确实减少了旧帧而非重复投递。
int messageCountForTopic(const QVector<ReceivedMessage>& messages,
                         const QString& topic)
{
    int count = 0;
    for (const ReceivedMessage& message : messages) {
        if (message.topic == topic)
            ++count;
    }
    return count;
}

bool stateSignalContainsDetail(const QSignalSpy& spy,
                               const QString& moduleId,
                               const QString& state,
                               const QString& detailPart)
{
    for (const QList<QVariant>& arguments : spy) {
        if (arguments.size() >= 3 &&
            arguments.at(0).toString() == moduleId &&
            arguments.at(1).toString() == state &&
            arguments.at(2).toString().contains(detailPart)) {
            return true;
        }
    }
    return false;
}

// 按文件名顺序读取目录中的全部滚动日志并拼接。
// 测试只检查文本是否已落盘，不依赖消息恰好位于哪一个滚动文件。
QByteArray readAllLogs(const QString& directory)
{
    QByteArray result;
    const QFileInfoList files = QDir(directory).entryInfoList(
        QStringList() << QStringLiteral("QFramework_*.log"),
        QDir::Files,
        QDir::Name);
    for (const QFileInfo& info : files) {
        QFile file(info.absoluteFilePath());
        if (file.open(QIODevice::ReadOnly))
            result += file.readAll();
    }
    return result;
}
}

// 目的：锁定框架依赖和自身版本基线。
// 准备：直接读取编译期 Qt 版本及两个运行时版本函数。
// 动作与断言：确认 Qt 不低于 5.15.2，并确认框架、Protobuf 版本字符串准确。
void BaselineTest::qtAndFrameworkVersions()
{
    QVERIFY(QT_VERSION >= QT_VERSION_CHECK(5, 15, 2));
    QCOMPARE(QString::fromUtf8(qframework::frameworkVersion()),
             QStringLiteral("0.1.0-baseline"));
    QCOMPARE(QString::fromUtf8(qframework::protobufRuntimeVersion()),
             QStringLiteral("3.21.12"));
}

// 目的：验证模块只有在“已绑定宿主且处于 Running”时才能发布。
// 准备：创建最小模块和 FakeHost；动作：依次在绑定前、运行中、停止后发布。
// 断言：只有运行中的消息被接受，且模块 ID、主题和负载完整转交给宿主。
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

// 目的：证明生成的 Protobuf 类型可序列化/反序列化，公共主题常量彼此独立。
// 准备：构造含尺寸、格式和载荷的 ImageFrame；动作：编码后再解析。
// 断言：关键字段原值返回，ModuleStatus 初始化有效，图像主题不等于状态主题。
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

// 目的：验证 FrameworkConfig 只读加载 INI，并正确解析路径和新增日志配置。
// 准备：加载前保存文件原始字节；动作：读取模块、日志和主题配置。
// 断言：四个模块顺序、相对路径、100 ms 刷新及 Latest 策略正确，文件字节未变。
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
    QCOMPARE(config.logging().flushIntervalMs, 100);
    QCOMPARE(config.messageBus().topics.value(QStringLiteral("QFRAMEWORK_IMAGE_RAW")).policy,
             qframework::QueuePolicy::Latest);

    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray after = file.readAll();
    file.close();
    QCOMPARE(after, before);
}

// 目的：验证小文件滚动、框架日志格式以及 Qt 全局消息接管。
// 准备：在临时目录以 256 字节上限启动 Logger；动作：写普通日志和 qWarning。
// 断言：产生至少两个日志文件，内容含模块、警告级别和线程标识。
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

// 目的：验证 FlushIntervalMs 默认/非法回退值和 100 ms 周期批量落盘。
// 准备：分别生成缺失、值为 0 的临时 INI，并以 100 ms 启动后台日志线程。
// 动作与断言：不调用 flush() 写普通日志，最多 500 ms 内应能从文件读到该记录。
void BaselineTest::loggerFlushesAtConfiguredInterval()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    QString error;
    const QString missingConfigPath = temporary.filePath(
        QStringLiteral("missing-flush.ini"));
    QFile missingConfigFile(missingConfigPath);
    QVERIFY(missingConfigFile.open(QIODevice::WriteOnly | QIODevice::Text));
    const QByteArray missingConfig(
        "[Modules]\nNames=\n[Logging]\nDirectory=Logs\n");
    QCOMPARE(missingConfigFile.write(missingConfig), qint64(missingConfig.size()));
    missingConfigFile.close();
    // 先验证“缺失”和“非法”两种 INI 都回退到 100 ms，再验证后台线程
    // 无需显式 flush 也能在一个有限窗口内把普通日志写入文件。
    qframework::FrameworkConfig defaultConfig;
    QVERIFY2(defaultConfig.load(missingConfigPath, &error), qPrintable(error));
    QCOMPARE(defaultConfig.logging().flushIntervalMs, 100);

    const QString invalidConfigPath = temporary.filePath(
        QStringLiteral("invalid-flush.ini"));
    QFile configFile(invalidConfigPath);
    QVERIFY(configFile.open(QIODevice::WriteOnly | QIODevice::Text));
    const QByteArray invalidConfig(
        "[Modules]\nNames=\n[Logging]\nDirectory=Logs\nFlushIntervalMs=0\n");
    QCOMPARE(configFile.write(invalidConfig), qint64(invalidConfig.size()));
    configFile.close();
    qframework::FrameworkConfig config;
    error.clear();
    QVERIFY2(config.load(invalidConfigPath, &error), qPrintable(error));
    QCOMPARE(config.logging().flushIntervalMs, 100);

    const QString logDirectory = temporary.filePath(QStringLiteral("IntervalLogs"));
    qframework::Logger& logger = qframework::Logger::instance();
    logger.uninstallQtMessageHandler();
    logger.stop();
    QVERIFY2(logger.start(logDirectory, 1024 * 1024, 100, &error),
             qPrintable(error));
    QElapsedTimer elapsed;
    elapsed.start();
    logger.log(qframework::LogLevel::Info,
               QStringLiteral("IntervalTest"),
               QStringLiteral("periodic-flush"));
    QTRY_VERIFY_WITH_TIMEOUT(readAllLogs(logDirectory).contains("periodic-flush"),
                             500);
    QVERIFY(elapsed.elapsed() <= 500);
    logger.stop();
}

// 目的：证明显式 flush() 与 stop() 不受较长批量刷新周期限制。
// 准备：将周期设为 5 秒；动作：分别在 flush 前和 stop 前写入一条日志。
// 断言：flush 后第一条立即可见，stop 返回后尾部日志也没有丢失。
void BaselineTest::loggerExplicitFlushAndStopNoLoss()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString logDirectory = temporary.filePath(QStringLiteral("ExplicitLogs"));
    qframework::Logger& logger = qframework::Logger::instance();
    logger.uninstallQtMessageHandler();
    logger.stop();
    QString error;
    QVERIFY2(logger.start(logDirectory, 1024 * 1024, 5000, &error),
             qPrintable(error));

    // 把周期设为 5 秒，刻意让自动刷新不会抢先完成；下面的显式 flush
    // 必须立即看到第一条，stop 则必须保留最后一条尾日志。
    logger.log(qframework::LogLevel::Info,
               QStringLiteral("ExplicitTest"),
               QStringLiteral("explicit-flush"));
    logger.flush();
    QVERIFY(readAllLogs(logDirectory).contains("explicit-flush"));

    logger.log(qframework::LogLevel::Info,
               QStringLiteral("ExplicitTest"),
               QStringLiteral("stop-tail"));
    logger.stop();
    const QByteArray all = readAllLogs(logDirectory);
    QVERIFY(all.contains("explicit-flush"));
    QVERIFY(all.contains("stop-tail"));
}

// 目的：覆盖进程内 MessageBus 的顺序、Latest、Reliable、大小和权限规则。
// 准备：建立容量为 1 的 Latest/Reliable 主题及发布者、订阅者。
// 动作：先关闭投递制造积压，再发布多条消息并恢复投递。
// 断言：普通主题有序，Latest 只留新值，Reliable 拒绝第二条，统计量准确。
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

// 目的：验证插件元数据、配置顺序、禁用模块和单模块故障隔离。
// 准备：使用真实构建出的两个示例 DLL；动作：按不同配置组合加载并启动。
// 断言：状态信号顺序正确、UI 模块收到消息、禁用项跳过、错误项不拖垮后续模块。
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

    // ProcessNonUiExample 每次启动都会发布 2048 字符的共享内存演示消息。
    // UI 只能显示有界预览，不能让任意长业务文本反向放大 Dock 的最小尺寸。
    qframework::protocols::LogDisplayMessage oversizedMessage;
    oversizedMessage.set_level("INFO");
    oversizedMessage.set_module_id("ProcessNonUiExample");
    oversizedMessage.set_text(std::string("ProcessNonUiExample ready ") +
                              std::string(2048, 'X'));
    std::string oversizedBytes;
    QVERIFY(oversizedMessage.SerializeToString(&oversizedBytes));
    uiModule->onMessage(QString::fromLatin1(QFRAMEWORK_LOG_DISPLAY),
                        QStringLiteral("ProcessNonUiExample"),
                        QByteArray::fromStdString(oversizedBytes));
    QTRY_COMPARE(uiModule->property("receivedMessageCount").toInt(), 2);
    QVERIFY2(uiModule->minimumSizeHint().width() < 1200,
             qPrintable(QStringLiteral("long message expanded UI minimum width to %1")
                            .arg(uiModule->minimumSizeHint().width())));
    bus.beginShutdown();
    const qframework::MessageBusStopReport pluginStopReport = bus.stopQueues(2000);
    QVERIFY(pluginStopReport.allStopped());
    QVERIFY(manager.shutdown(pluginStopReport.timedOutModuleIds).isEmpty());
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
    disabledBus.beginShutdown();
    const qframework::MessageBusStopReport disabledStopReport =
        disabledBus.stopQueues(2000);
    QVERIFY(disabledManager.shutdown(disabledStopReport.timedOutModuleIds).isEmpty());

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
    isolatedBus.beginShutdown();
    const qframework::MessageBusStopReport isolatedStopReport =
        isolatedBus.stopQueues(2000);
    QVERIFY(isolatedManager.shutdown(isolatedStopReport.timedOutModuleIds).isEmpty());
}

// 目的：验证本地 IPC 的“4 字节长度 + JSON”拆包规则。
// 准备：编码一帧 ping；动作：先只喂两个字节，再补齐，并构造超长帧头。
// 断言：半包返回 Incomplete、完整帧返回 Ready 且字段一致、超限返回 Invalid。
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

// 目的：综合验证真实进程模块的注册、消息、心跳、窗口嵌入和有序停止。
// 准备：启动一个非 UI 和一个 UI 示例进程，并注册日志观察模块。
// 动作：开放总线投递、接收大消息、挂接窗口、发送尺寸/显隐控制后停止。
// 断言：模块状态、共享内存大消息、原生窗口尺寸和停止顺序均符合协议。
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
    QSignalSpy startupSpy(&supervisor, &qframework::ProcessSupervisor::startupBatchFinished);
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
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.runningModuleIds().size(), 2, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(startupSpy.count(), 1, 10000);
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
    QVERIFY2(supervisor.showWindow(QStringLiteral("ProcessUiExample"), &error),
             qPrintable(error));
    QVERIFY2(supervisor.resizeWindow(QStringLiteral("ProcessUiExample"),
                                     windowHost.width(),
                                     windowHost.height(),
                                     &error),
             qPrintable(error));
#ifdef Q_OS_WIN
    QTRY_COMPARE_WITH_TIMEOUT(nativeWindowClientSize(initialWindowId),
                              windowHost.size(),
                              1000);
    windowHost.resize(500, 320);
    QTRY_COMPARE_WITH_TIMEOUT(nativeWindowClientSize(initialWindowId),
                              windowHost.size(),
                              2000);
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

    // 精确复现桌面问题：ProcessUi 仍嵌入时手动重启 ProcessNonUi。后者会再次
    // 发布 2048 字符消息，但不得改变现有 HWND、宿主尺寸或主窗口布局约束。
    const int windowCountBeforeNonUiRestart = windowSpy.size();
    QSignalSpy nonUiRestartSpy(&supervisor,
                               &qframework::ProcessSupervisor::restartFinished);
    QVERIFY2(supervisor.requestRestart(QStringLiteral("ProcessNonUiExample"), &error),
             qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(nonUiRestartSpy.count(), 1, 5000);
    QVERIFY2(nonUiRestartSpy.first().at(1).toBool(),
             qPrintable(nonUiRestartSpy.first().at(2).toString()));
    QCOMPARE(windowSpy.size(), windowCountBeforeNonUiRestart);
    QCOMPARE(supervisor.state(QStringLiteral("ProcessUiExample")),
             QStringLiteral("Running"));
#ifdef Q_OS_WIN
    QTest::qWait(500);
    QCOMPARE(nativeWindowClientSize(initialWindowId), windowHost.size());
#endif

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
    QVERIFY2(supervisor.showWindow(QStringLiteral("ProcessUiExample"), &error),
             qPrintable(error));
    QVERIFY2(supervisor.resizeWindow(QStringLiteral("ProcessUiExample"),
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

// 目的：验证父到子、子到父两个方向的 Latest 队列满时覆盖同主题最旧帧。
// 准备：两侧主题容量均设为 1，并用慢消费者制造尚未 ACK 的积压。
// 动作：快速发送连续序号；断言：最后序号可达、旧帧送达数减少且无故障。
void BaselineTest::processLatestQueueOverwritesOldFrames()
{
    // 父进程先快速发布 100 帧，子进程慢速消费。容量为 1 时，最终应看到
    // 最新的“100”，而不是早期帧；统计值小于 100 证明发生了有界覆盖。
    const qframework::MessageBusConfig busConfig = queueTestBusConfig();
    qframework::MessageBus bus(busConfig);
    BusTestModule publisher(
        QStringList() << QStringLiteral("TEST_PARENT_LATEST"),
        QStringList());
    BusTestModule observer(
        QStringList(),
        QStringList() << QStringLiteral("TEST_CHILD_LATEST")
                      << QStringLiteral("TEST_CHILD_RESULT"));
    QString error;
    QVERIFY2(bus.registerModule(QStringLiteral("LatestPublisher"),
                                &publisher,
                                &error),
             qPrintable(error));
    QVERIFY2(bus.registerModule(QStringLiteral("LatestObserver"),
                                &observer,
                                &error),
             qPrintable(error));
    QVERIFY(bus.setModuleRunning(QStringLiteral("LatestPublisher"), true));
    QVERIFY(bus.setModuleRunning(QStringLiteral("LatestObserver"), true));

    qframework::ProcessSupervisor supervisor(
        &bus, busConfig, queueTestProcessConfig());
    QVector<qframework::ModuleConfig> modules;
    modules.append(queueTestModuleConfig(QStringLiteral("RuntimeQueueLatest")));
    QStringList errors;
    QVERIFY2(supervisor.startAll(modules, &errors),
             qPrintable(errors.join('\n')));
    QTRY_COMPARE_WITH_TIMEOUT(
        supervisor.state(QStringLiteral("RuntimeQueueLatest")),
        QStringLiteral("Running"),
        5000);
    bus.setDeliveryEnabled(true);

    for (int sequence = 1; sequence <= 100; ++sequence) {
        QVERIFY(publisher.publish(QStringLiteral("TEST_PARENT_LATEST"),
                                  QByteArray::number(sequence)));
    }

    QTRY_VERIFY_WITH_TIMEOUT(
        messagesContain(observer.received(),
                        QStringLiteral("TEST_CHILD_LATEST"),
                        QByteArrayLiteral("100")),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        messagesContain(observer.received(),
                        QStringLiteral("TEST_CHILD_RESULT"),
                        QByteArrayLiteral("TEST_PARENT_LATEST:100")),
        5000);
    QVERIFY(messageCountForTopic(observer.received(),
                                 QStringLiteral("TEST_CHILD_LATEST")) < 100);
    QVERIFY(messageCountForTopic(observer.received(),
                                 QStringLiteral("TEST_CHILD_RESULT")) < 100);

    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(2000));
    QVERIFY(bus.unregisterModule(QStringLiteral("LatestPublisher"), false));
    QVERIFY(bus.unregisterModule(QStringLiteral("LatestObserver"), false));
}

// 目的：验证 Reliable 队列满时拒绝新消息，绝不拿新值覆盖已接受的旧值。
// 准备：两侧容量均为 1；动作：在 ACK 释放槽位前连续发布 first/second。
// 断言：第二条被拒绝、第一条原样送达，父子两个方向都遵守相同语义。
void BaselineTest::processReliableQueueDoesNotOverwrite()
{
    // Reliable 场景与 Latest 对照：第二、第三条可以被拒绝，但第一条不能
    // 被替换；结果还必须保持 first -> second -> third 的相对顺序。
    const qframework::MessageBusConfig busConfig = queueTestBusConfig();
    qframework::MessageBus bus(busConfig);
    BusTestModule publisher(
        QStringList() << QStringLiteral("TEST_PARENT_RELIABLE"),
        QStringList());
    BusTestModule observer(
        QStringList(),
        QStringList() << QStringLiteral("TEST_CHILD_RELIABLE")
                      << QStringLiteral("TEST_CHILD_RESULT"));
    QString error;
    QVERIFY2(bus.registerModule(QStringLiteral("ReliablePublisher"),
                                &publisher,
                                &error),
             qPrintable(error));
    QVERIFY2(bus.registerModule(QStringLiteral("ReliableObserver"),
                                &observer,
                                &error),
             qPrintable(error));
    QVERIFY(bus.setModuleRunning(QStringLiteral("ReliablePublisher"), true));
    QVERIFY(bus.setModuleRunning(QStringLiteral("ReliableObserver"), true));

    qframework::ProcessSupervisor supervisor(
        &bus, busConfig, queueTestProcessConfig());
    QVector<qframework::ModuleConfig> modules;
    modules.append(queueTestModuleConfig(QStringLiteral("RuntimeQueueReliable")));
    QStringList errors;
    QVERIFY2(supervisor.startAll(modules, &errors),
             qPrintable(errors.join('\n')));
    QTRY_COMPARE_WITH_TIMEOUT(
        supervisor.state(QStringLiteral("RuntimeQueueReliable")),
        QStringLiteral("Running"),
        5000);
    bus.setDeliveryEnabled(true);

    QVERIFY(publisher.publish(QStringLiteral("TEST_PARENT_RELIABLE"),
                              QByteArrayLiteral("first")));
    publisher.publish(QStringLiteral("TEST_PARENT_RELIABLE"),
                      QByteArrayLiteral("second"));
    publisher.publish(QStringLiteral("TEST_PARENT_RELIABLE"),
                      QByteArrayLiteral("third"));

    QTRY_VERIFY_WITH_TIMEOUT(
        messagesContain(observer.received(),
                        QStringLiteral("TEST_CHILD_RELIABLE"),
                        QByteArrayLiteral("first")),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        messagesContain(observer.received(),
                        QStringLiteral("TEST_CHILD_RESULT"),
                        QByteArrayLiteral("localReliable:1,0")),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        messagesContain(observer.received(),
                        QStringLiteral("TEST_CHILD_RESULT"),
                        QByteArrayLiteral("TEST_PARENT_RELIABLE:first")),
        5000);
    QTest::qWait(700);
    QVERIFY(!messagesContain(observer.received(),
                             QStringLiteral("TEST_CHILD_RELIABLE"),
                             QByteArrayLiteral("second")));
    QVector<QByteArray> parentResults;
    for (const ReceivedMessage& message : observer.received()) {
        if (message.topic == QStringLiteral("TEST_CHILD_RESULT") &&
            message.data.startsWith("TEST_PARENT_RELIABLE:")) {
            parentResults.append(message.data);
        }
    }
    QVERIFY(!parentResults.isEmpty());
    QCOMPARE(parentResults.first(),
             QByteArrayLiteral("TEST_PARENT_RELIABLE:first"));
    QVERIFY(parentResults.size() <= 3);
    const QVector<QByteArray> expectedResults = QVector<QByteArray>()
        << QByteArrayLiteral("TEST_PARENT_RELIABLE:first")
        << QByteArrayLiteral("TEST_PARENT_RELIABLE:second")
        << QByteArrayLiteral("TEST_PARENT_RELIABLE:third");
    int previousIndex = -1;
    for (const QByteArray& result : parentResults) {
        const int currentIndex = expectedResults.indexOf(result);
        QVERIFY(currentIndex > previousIndex);
        previousIndex = currentIndex;
    }

    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(2000));
    QVERIFY(bus.unregisterModule(QStringLiteral("ReliablePublisher"), false));
    QVERIFY(bus.unregisterModule(QStringLiteral("ReliableObserver"), false));
}

// 目的：同时验证双向 inline/shared-memory 分流以及内部 ACK 后资源回收。
// 准备：共享内存阈值设为 128 字节；动作：父子各发送短消息和 1024 字节消息。
// 断言：四条原始消息及两条回显数据均完整，进程可正常停止且没有 IPC 故障。
void BaselineTest::processIpcInlineSharedBidirectional()
{
    // 一次测试覆盖四条路径：父发小/大消息到子进程，子再原样回发小/大消息。
    // 这样既能验证 QLocalSocket 控制帧，也能验证共享段在 publish ACK 后释放。
    const qframework::MessageBusConfig busConfig = queueTestBusConfig();
    qframework::MessageBus bus(busConfig);
    BusTestModule publisher(
        QStringList() << QStringLiteral("TEST_PARENT_INLINE")
                      << QStringLiteral("TEST_PARENT_SHARED"),
        QStringList());
    BusTestModule observer(
        QStringList(),
        QStringList() << QStringLiteral("TEST_CHILD_INLINE")
                      << QStringLiteral("TEST_CHILD_SHARED")
                      << QStringLiteral("TEST_CHILD_REPLY_INLINE")
                      << QStringLiteral("TEST_CHILD_REPLY_SHARED"));
    QString error;
    QVERIFY2(bus.registerModule(QStringLiteral("IpcPublisher"),
                                &publisher,
                                &error),
             qPrintable(error));
    QVERIFY2(bus.registerModule(QStringLiteral("IpcObserver"),
                                &observer,
                                &error),
             qPrintable(error));
    QVERIFY(bus.setModuleRunning(QStringLiteral("IpcPublisher"), true));
    QVERIFY(bus.setModuleRunning(QStringLiteral("IpcObserver"), true));

    qframework::ProcessSupervisor supervisor(
        &bus, busConfig, queueTestProcessConfig());
    QVector<qframework::ModuleConfig> modules;
    modules.append(
        queueTestModuleConfig(QStringLiteral("RuntimeQueueInlineShared")));
    QStringList errors;
    QVERIFY2(supervisor.startAll(modules, &errors),
             qPrintable(errors.join('\n')));
    QTRY_COMPARE_WITH_TIMEOUT(
        supervisor.state(QStringLiteral("RuntimeQueueInlineShared")),
        QStringLiteral("Running"),
        5000);
    bus.setDeliveryEnabled(true);

    const QByteArray parentInline("parent-inline");
    const QByteArray parentShared(1024, 'P');
    QVERIFY(publisher.publish(QStringLiteral("TEST_PARENT_INLINE"), parentInline));
    QVERIFY(publisher.publish(QStringLiteral("TEST_PARENT_SHARED"), parentShared));

    QTRY_VERIFY_WITH_TIMEOUT(
        messagesContain(observer.received(),
                        QStringLiteral("TEST_CHILD_INLINE"),
                        QByteArrayLiteral("child-inline")),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        messagesContain(observer.received(),
                        QStringLiteral("TEST_CHILD_SHARED"),
                        QByteArray(1024, 'C')),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        messagesContain(observer.received(),
                        QStringLiteral("TEST_CHILD_REPLY_INLINE"),
                        parentInline),
        5000);
    QTRY_VERIFY_WITH_TIMEOUT(
        messagesContain(observer.received(),
                        QStringLiteral("TEST_CHILD_REPLY_SHARED"),
                        parentShared),
        5000);

    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(2000));
    QVERIFY(bus.unregisterModule(QStringLiteral("IpcPublisher"), false));
    QVERIFY(bus.unregisterModule(QStringLiteral("IpcObserver"), false));
}

// 目的：验证注册帧 token 不匹配时，监督器拒绝并报告明确故障。
// 准备：启动会发送 invalid-token 的测试子进程；动作：等待 moduleFault 信号。
// 断言：故障归属于目标模块，且原因包含 token/认证相关信息。
void BaselineTest::processRejectsInvalidToken()
{
    const qframework::MessageBusConfig busConfig = faultBusConfig();
    const qframework::ProcessConfig processConfig = faultProcessConfig();
    qframework::MessageBus bus(busConfig);
    qframework::ProcessSupervisor supervisor(&bus, busConfig, processConfig);
    QSignalSpy faultSpy(&supervisor, &qframework::ProcessSupervisor::moduleFault);
    QSignalSpy startupSpy(&supervisor, &qframework::ProcessSupervisor::startupBatchFinished);

    const QString moduleId = QStringLiteral("InvalidTokenModule");
    QVector<qframework::ModuleConfig> modules;
    modules.append(faultProcessModuleConfig(moduleId));
    QStringList errors;
    QVERIFY2(supervisor.startAll(modules, &errors), qPrintable(errors.join('\n')));
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.state(moduleId), QStringLiteral("Failed"), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(startupSpy.count(), 1, 3000);
    const QStringList startupErrors = startupSpy.first().at(0).toStringList();
    QVERIFY(startupErrors.join('\n').contains(
        QString::fromUtf8(u8"子进程注册信息校验失败")));
    QTRY_VERIFY_WITH_TIMEOUT(
        faultSignalContains(faultSpy,
                            moduleId,
                            QString::fromUtf8(u8"子进程注册信息校验失败")),
        3000);

    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(200));
}

// 目的：验证子进程连上服务端但迟迟不注册时会触发有界注册超时。
// 准备：使用 faultProcessConfig 的 250 ms 注册窗口；动作：启动后不发送注册帧。
// 断言：监督器在有限时间内报告 registration timeout，而不是永久等待。
void BaselineTest::processRegistrationTimeout()
{
    const qframework::MessageBusConfig busConfig = faultBusConfig();
    const qframework::ProcessConfig processConfig = faultProcessConfig();
    qframework::MessageBus bus(busConfig);
    qframework::ProcessSupervisor supervisor(&bus, busConfig, processConfig);
    QSignalSpy faultSpy(&supervisor, &qframework::ProcessSupervisor::moduleFault);
    QSignalSpy startupSpy(&supervisor, &qframework::ProcessSupervisor::startupBatchFinished);

    const QString moduleId = QStringLiteral("RegistrationTimeoutModule");
    QVector<qframework::ModuleConfig> modules;
    modules.append(faultProcessModuleConfig(moduleId));
    QStringList errors;
    QVERIFY2(supervisor.startAll(modules, &errors), qPrintable(errors.join('\n')));
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.state(moduleId), QStringLiteral("Failed"), 3000);
    QTRY_COMPARE_WITH_TIMEOUT(startupSpy.count(), 1, 3000);
    const QStringList startupErrors = startupSpy.first().at(0).toStringList();
    QVERIFY(startupErrors.join('\n').contains(QString::fromUtf8(u8"子进程注册超时")));
    QTRY_VERIFY_WITH_TIMEOUT(
        faultSignalContains(faultSpy,
                            moduleId,
                            QString::fromUtf8(u8"子进程注册超时")),
        3000);

    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(200));
}

// 目的：验证已注册并 started 的子进程停止回应 ping 后会被心跳超时清理。
// 准备：测试客户端故意进入事件循环但不读取后续 ping；动作：等待故障信号。
// 断言：原因包含 heartbeat/timeout，监督器状态最终回到可停止状态。
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
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.state(moduleId), QStringLiteral("Running"), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(supervisor.state(moduleId) == QStringLiteral("Failed"), 3000);
    QVERIFY(faultSignalContains(faultSpy,
                                moduleId,
                                QString::fromUtf8(u8"子进程心跳超时")));

    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(200));
}

// 目的：验证调试等待期间心跳计时不会误杀尚未完成 onStart 的子进程。
// 准备：客户端在 started 前等待 600 ms，超过普通心跳周期；动作：启动并观察。
// 断言：调试等待阶段不产生 premature heartbeat fault，随后可显式停止。
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
    QVERIFY2(elapsed.elapsed() < 200,
             qPrintable(QStringLiteral("startAll blocked for %1 ms").arg(elapsed.elapsed())));
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.state(moduleId), QStringLiteral("Running"), 3000);
    QVERIFY(faultSpy.isEmpty());

    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(200));
}

// requestRestart 只提交状态转换；用主线程 QTimer 证明等待 stop/启动期间事件循环仍运行。
void BaselineTest::processRestartRequestDoesNotBlockGui()
{
    const qframework::MessageBusConfig busConfig = faultBusConfig();
    qframework::MessageBus bus(busConfig);
    qframework::ProcessSupervisor supervisor(
        &bus, busConfig, lifecycleProcessConfig());
    QSignalSpy restartSpy(&supervisor, &qframework::ProcessSupervisor::restartFinished);

    const QString moduleId = QStringLiteral("AsyncRestartModule");
    QVector<qframework::ModuleConfig> modules;
    modules.append(faultProcessModuleConfig(moduleId));
    QStringList errors;
    QVERIFY2(supervisor.startAll(modules, &errors), qPrintable(errors.join('\n')));
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.state(moduleId), QStringLiteral("Running"), 3000);

    int timerTicks = 0;
    QTimer guiTimer;
    guiTimer.setInterval(20);
    connect(&guiTimer, &QTimer::timeout, this, [&timerTicks]() { ++timerTicks; });
    guiTimer.start();
    QElapsedTimer elapsed;
    elapsed.start();
    QString error;
    QVERIFY2(supervisor.requestRestart(moduleId, &error), qPrintable(error));
    QVERIFY2(elapsed.elapsed() < 100,
             qPrintable(QStringLiteral("requestRestart blocked for %1 ms")
                            .arg(elapsed.elapsed())));
    QTRY_COMPARE_WITH_TIMEOUT(restartSpy.count(), 1, 5000);
    guiTimer.stop();
    QVERIFY2(timerTicks >= 5,
             qPrintable(QStringLiteral("GUI timer only fired %1 times").arg(timerTicks)));
    QVERIFY(restartSpy.first().at(1).toBool());
    QCOMPARE(supervisor.state(moduleId), QStringLiteral("Running"));

    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(500));
}

// 同一 Entry 的 operationBusy 在第一轮重启完成前拒绝第二次请求。
void BaselineTest::processRejectsDuplicateRestartRequest()
{
    const qframework::MessageBusConfig busConfig = faultBusConfig();
    qframework::MessageBus bus(busConfig);
    qframework::ProcessSupervisor supervisor(
        &bus, busConfig, lifecycleProcessConfig());
    QSignalSpy restartSpy(&supervisor, &qframework::ProcessSupervisor::restartFinished);

    const QString moduleId = QStringLiteral("DuplicateRestartModule");
    QVector<qframework::ModuleConfig> modules;
    modules.append(faultProcessModuleConfig(moduleId));
    QStringList errors;
    QVERIFY2(supervisor.startAll(modules, &errors), qPrintable(errors.join('\n')));
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.state(moduleId), QStringLiteral("Running"), 3000);

    QString error;
    QVERIFY2(supervisor.requestRestart(moduleId, &error), qPrintable(error));
    error.clear();
    QVERIFY(!supervisor.requestRestart(moduleId, &error));
    QVERIFY(error.contains(QString::fromUtf8(u8"操作正在进行")));
    QTRY_COMPARE_WITH_TIMEOUT(restartSpy.count(), 1, 4000);
    QVERIFY(restartSpy.first().at(1).toBool());
    QCOMPARE(supervisor.state(moduleId), QStringLiteral("Running"));

    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(500));
}

// requestStop 返回前必须同时关闭父到子队列和 MessageBus 单模块入口。
void BaselineTest::processStopClosesIngressImmediately()
{
    qframework::MessageBusConfig busConfig = faultBusConfig();
    busConfig.defaultQueueCapacity = 8;
    qframework::MessageBus bus(busConfig);
    BusTestModule publisher(QStringList() << QStringLiteral("TEST_STOP_GATE"),
                            QStringList());
    QString error;
    QVERIFY2(bus.registerModule(QStringLiteral("StopGatePublisher"), &publisher, &error),
             qPrintable(error));
    QVERIFY(bus.setModuleRunning(QStringLiteral("StopGatePublisher"), true));

    qframework::ProcessSupervisor supervisor(
        &bus, busConfig, lifecycleProcessConfig());
    const QString moduleId = QStringLiteral("IngressGateModule");
    QVector<qframework::ModuleConfig> modules;
    modules.append(faultProcessModuleConfig(moduleId));
    QStringList errors;
    QVERIFY2(supervisor.startAll(modules, &errors), qPrintable(errors.join('\n')));
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.state(moduleId), QStringLiteral("Running"), 3000);
    bus.setDeliveryEnabled(true);
    QVERIFY(publisher.publish(QStringLiteral("TEST_STOP_GATE"), QByteArrayLiteral("before")));
    QTRY_COMPARE_WITH_TIMEOUT(bus.queueStats(moduleId).delivered, quint64(1), 1000);

    QVERIFY2(supervisor.requestStop(moduleId, &error), qPrintable(error));
    QVERIFY(!publisher.publish(QStringLiteral("TEST_STOP_GATE"), QByteArrayLiteral("after")));
    QVERIFY(bus.queueStats(moduleId).rejected >= 1);
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.state(moduleId), QStringLiteral("Stopped"), 2000);

    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(500));
    QVERIFY(bus.unregisterModule(QStringLiteral("StopGatePublisher"), false));
}

// A、B 各自持有 stop deadline；稍后启动 B 的重启不能重置 A 的超时预算。
void BaselineTest::processDeadlinesAreIndependentPerEntry()
{
    const qframework::MessageBusConfig busConfig = faultBusConfig();
    qframework::MessageBus bus(busConfig);
    qframework::ProcessSupervisor supervisor(
        &bus, busConfig, lifecycleProcessConfig(300));
    QSignalSpy restartSpy(&supervisor, &qframework::ProcessSupervisor::restartFinished);

    const QString moduleA = QStringLiteral("DeadlineModuleA");
    const QString moduleB = QStringLiteral("DeadlineModuleB");
    QVector<qframework::ModuleConfig> modules;
    modules.append(faultProcessModuleConfig(moduleA));
    modules.append(faultProcessModuleConfig(moduleB));
    QStringList errors;
    QVERIFY2(supervisor.startAll(modules, &errors), qPrintable(errors.join('\n')));
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.runningModuleIds().size(), 2, 4000);

    QElapsedTimer timeline;
    timeline.start();
    qint64 moduleAStoppedAt = -1;
    connect(&supervisor,
            &qframework::ProcessSupervisor::moduleStateChanged,
            this,
            [&timeline, &moduleAStoppedAt, moduleA](const QString& id,
                                                    const QString& state,
                                                    const QString&) {
                if (id == moduleA && state == QStringLiteral("Stopped"))
                    moduleAStoppedAt = timeline.elapsed();
            });
    QString error;
    QVERIFY2(supervisor.requestStop(moduleA, &error), qPrintable(error));
    QTest::qWait(80);
    QVERIFY2(supervisor.requestRestart(moduleB, &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(moduleAStoppedAt >= 0, 1800);
    QVERIFY2(moduleAStoppedAt < 1000,
             qPrintable(QStringLiteral("A stop deadline drifted to %1 ms")
                            .arg(moduleAStoppedAt)));
    QTRY_COMPARE_WITH_TIMEOUT(restartSpy.count(), 1, 2500);
    QVERIFY(restartSpy.first().at(1).toBool());
    QCOMPARE(supervisor.state(moduleB), QStringLiteral("Running"));

    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(500));
}

// stopAck 到达后状态机必须从 WaitingStopAck 推进，并给出可观察的等待退出详情。
void BaselineTest::processStopAckAdvancesState()
{
    const qframework::MessageBusConfig busConfig = faultBusConfig();
    qframework::MessageBus bus(busConfig);
    qframework::ProcessSupervisor supervisor(
        &bus, busConfig, lifecycleProcessConfig(1000));
    QSignalSpy stateSpy(&supervisor, &qframework::ProcessSupervisor::moduleStateChanged);

    const QString moduleId = QStringLiteral("StopAckModule");
    QVector<qframework::ModuleConfig> modules;
    modules.append(faultProcessModuleConfig(moduleId));
    QStringList errors;
    QVERIFY2(supervisor.startAll(modules, &errors), qPrintable(errors.join('\n')));
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.state(moduleId), QStringLiteral("Running"), 3000);
    QString error;
    QVERIFY2(supervisor.requestStop(moduleId, &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(
        stateSignalContainsDetail(stateSpy,
                                  moduleId,
                                  QStringLiteral("Stopping"),
                                  QStringLiteral("stopAck")),
        1000);
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.state(moduleId), QStringLiteral("Stopped"), 2000);

    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(500));
}

// 三个无响应子进程共用一次退出总预算，耗时不能线性乘以进程数量。
void BaselineTest::processShutdownUsesOneTotalDeadline()
{
    const qframework::MessageBusConfig busConfig = faultBusConfig();
    qframework::MessageBus bus(busConfig);
    qframework::ProcessSupervisor supervisor(
        &bus, busConfig, lifecycleProcessConfig(250));
    QVector<qframework::ModuleConfig> modules;
    modules.append(faultProcessModuleConfig(QStringLiteral("ShutdownHungA")));
    modules.append(faultProcessModuleConfig(QStringLiteral("ShutdownHungB")));
    modules.append(faultProcessModuleConfig(QStringLiteral("ShutdownHungC")));
    QStringList errors;
    QVERIFY2(supervisor.startAll(modules, &errors), qPrintable(errors.join('\n')));
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.runningModuleIds().size(), 3, 4000);

    QElapsedTimer elapsed;
    elapsed.start();
    supervisor.shutdown();
    QVERIFY2(elapsed.elapsed() < 2300,
             qPrintable(QStringLiteral("shutdown used %1 ms for one total deadline")
                            .arg(elapsed.elapsed())));
    QVERIFY(supervisor.runningModuleIds().isEmpty());

    bus.beginShutdown();
    QVERIFY(bus.stopQueues(500));
}

// startAll 的返回值只代表请求被接受；批次完成信号必须在后续事件循环发出。
void BaselineTest::processStartupBatchCompletesAsynchronously()
{
    const qframework::MessageBusConfig busConfig = faultBusConfig();
    qframework::MessageBus bus(busConfig);
    qframework::ProcessSupervisor supervisor(
        &bus, busConfig, lifecycleProcessConfig());
    QSignalSpy startupSpy(&supervisor, &qframework::ProcessSupervisor::startupBatchFinished);

    const QString moduleId = QStringLiteral("StartupAsyncModule");
    QVector<qframework::ModuleConfig> modules;
    modules.append(faultProcessModuleConfig(moduleId));
    QStringList errors;
    QVERIFY2(supervisor.startAll(modules, &errors), qPrintable(errors.join('\n')));
    QCOMPARE(startupSpy.count(), 0);
    QCOMPARE(supervisor.state(moduleId), QStringLiteral("Starting"));
    QTRY_COMPARE_WITH_TIMEOUT(startupSpy.count(), 1, 3000);
    QCOMPARE(startupSpy.first().at(0).toStringList(), QStringList());
    QCOMPARE(supervisor.state(moduleId), QStringLiteral("Running"));

    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(500));
}

// MessageBus 超时只能返回报告，不能在 finishStop 后再做无界 wait。
void BaselineTest::messageBusStopTimeoutIsBounded()
{
    qframework::MessageBusConfig config = faultBusConfig();
    config.shutdownDrainTimeoutMs = 100;
    qframework::MessageBus bus(config);
    BusTestModule publisher(QStringList() << QStringLiteral("TEST_BLOCKING_CALLBACK"),
                            QStringList());
    BlockingBusModule blocker;
    QString error;
    QVERIFY2(bus.registerModule(QStringLiteral("BlockingPublisher"), &publisher, &error),
             qPrintable(error));
    QVERIFY2(bus.registerModule(QStringLiteral("BlockingModule"), &blocker, &error),
             qPrintable(error));
    QVERIFY(bus.setModuleRunning(QStringLiteral("BlockingPublisher"), true));
    QVERIFY(bus.setModuleRunning(QStringLiteral("BlockingModule"), true));
    bus.setDeliveryEnabled(true);
    QVERIFY(publisher.publish(QStringLiteral("TEST_BLOCKING_CALLBACK"),
                              QByteArrayLiteral("block")));
    QVERIFY(blocker.waitUntilEntered(1000));

    bus.beginShutdown();
    QElapsedTimer elapsed;
    elapsed.start();
    const qframework::MessageBusStopReport report = bus.stopQueues(100);
    QVERIFY2(elapsed.elapsed() < 500,
             qPrintable(QStringLiteral("stopQueues blocked for %1 ms")
                            .arg(elapsed.elapsed())));
    QVERIFY(report.timedOutModuleIds.contains(QStringLiteral("BlockingModule")));
    QVERIFY(blocker.isStopRequested());

    blocker.release();
    QTRY_VERIFY_WITH_TIMEOUT(bus.isModuleQueueStopped(QStringLiteral("BlockingModule")), 1000);
    QVERIFY(bus.unregisterModule(QStringLiteral("BlockingModule"), false));
    QVERIFY(bus.unregisterModule(QStringLiteral("BlockingPublisher"), false));
}

// 超时队列保持注册，PluginManager 收到同一超时 ID 时保留 loader，安全后才可重试回收。
void BaselineTest::timedOutQueueIsNotDeletedOrUnloaded()
{
    qframework::MessageBusConfig config = faultBusConfig();
    config.shutdownDrainTimeoutMs = 80;
    {
        qframework::MessageBus bus(config);
        BusTestModule publisher(
            QStringList() << QStringLiteral("TEST_BLOCKING_CALLBACK"), QStringList());
        BlockingBusModule blocker;
        QString error;
        QVERIFY(bus.registerModule(QStringLiteral("QuarantinePublisher"), &publisher, &error));
        QVERIFY(bus.registerModule(QStringLiteral("QuarantineModule"), &blocker, &error));
        QVERIFY(bus.setModuleRunning(QStringLiteral("QuarantinePublisher"), true));
        QVERIFY(bus.setModuleRunning(QStringLiteral("QuarantineModule"), true));
        bus.setDeliveryEnabled(true);
        QVERIFY(publisher.publish(QStringLiteral("TEST_BLOCKING_CALLBACK"),
                                  QByteArrayLiteral("block")));
        QVERIFY(blocker.waitUntilEntered(1000));
        bus.beginShutdown();
        const qframework::MessageBusStopReport report = bus.stopQueues(80);
        QVERIFY(report.timedOutModuleIds.contains(QStringLiteral("QuarantineModule")));
        QVERIFY(!bus.unregisterModule(QStringLiteral("QuarantineModule"), false));
        QVERIFY(bus.moduleIds().contains(QStringLiteral("QuarantineModule")));
        QCOMPARE(blocker.moduleId(), QStringLiteral("QuarantineModule"));
        blocker.release();
        QTRY_VERIFY_WITH_TIMEOUT(
            bus.isModuleQueueStopped(QStringLiteral("QuarantineModule")), 1000);
        QVERIFY(bus.unregisterModule(QStringLiteral("QuarantineModule"), false));
        QVERIFY(bus.unregisterModule(QStringLiteral("QuarantinePublisher"), false));
    }

    const QString moduleId = QStringLiteral("InProcessNonUiExample");
    qframework::MessageBus pluginBus(config);
    qframework::PluginManager manager(&pluginBus);
    QVector<qframework::ModuleConfig> modules;
    modules.append(pluginConfig(moduleId, qframework::ModuleType::InProcessNonUi));
    QStringList errors;
    QVERIFY2(manager.loadAndStart(modules, &errors, false), qPrintable(errors.join('\n')));
    pluginBus.beginShutdown();
    const qframework::MessageBusStopReport stopped = pluginBus.stopQueues(500);
    QVERIFY(stopped.allStopped());
    QCOMPARE(manager.shutdown(QStringList() << moduleId), QStringList() << moduleId);
    QCOMPARE(manager.quarantinedModuleIds(), QStringList() << moduleId);
    QVERIFY(pluginBus.moduleIds().contains(moduleId));
    QVERIFY(manager.retryQuarantinedShutdown().isEmpty());
    QVERIFY(!pluginBus.moduleIds().contains(moduleId));
}

// 子进程 onMessage 卡死时，运行时不杀单条 QThread，而是让整个测试子进程退出。
void BaselineTest::processHungCallbackEndsWholeChild()
{
    qframework::MessageBusConfig busConfig = queueTestBusConfig();
    busConfig.shutdownDrainTimeoutMs = 150;
    qframework::MessageBus bus(busConfig);
    BusTestModule publisher(QStringList() << QStringLiteral("TEST_PARENT_HUNG"),
                            QStringList());
    BusTestModule observer(QStringList(),
                           QStringList() << QStringLiteral("TEST_CHILD_HUNG_ENTERED"));
    QString error;
    QVERIFY(bus.registerModule(QStringLiteral("HungPublisher"), &publisher, &error));
    QVERIFY(bus.registerModule(QStringLiteral("HungObserver"), &observer, &error));
    QVERIFY(bus.setModuleRunning(QStringLiteral("HungPublisher"), true));
    QVERIFY(bus.setModuleRunning(QStringLiteral("HungObserver"), true));

    qframework::ProcessSupervisor supervisor(
        &bus, busConfig, queueTestProcessConfig());
    QSignalSpy stateSpy(&supervisor, &qframework::ProcessSupervisor::moduleStateChanged);
    const QString moduleId = QStringLiteral("RuntimeHungCallback");
    QVector<qframework::ModuleConfig> modules;
    modules.append(queueTestModuleConfig(moduleId));
    QStringList errors;
    QVERIFY2(supervisor.startAll(modules, &errors), qPrintable(errors.join('\n')));
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.state(moduleId), QStringLiteral("Running"), 3000);
    bus.setDeliveryEnabled(true);
    QVERIFY(publisher.publish(QStringLiteral("TEST_PARENT_HUNG"), QByteArrayLiteral("hang")));
    QTRY_VERIFY_WITH_TIMEOUT(
        messagesContain(observer.received(),
                        QStringLiteral("TEST_CHILD_HUNG_ENTERED"),
                        QByteArrayLiteral("entered")),
        3000);

    QElapsedTimer elapsed;
    elapsed.start();
    QVERIFY2(supervisor.requestStop(moduleId, &error), qPrintable(error));
    QVERIFY(elapsed.elapsed() < 100);
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.state(moduleId), QStringLiteral("Stopped"), 3000);
    QVERIFY(stateSignalContainsDetail(stateSpy,
                                      moduleId,
                                      QStringLiteral("Stopping"),
                                      QString::fromUtf8(u8"停止超时")) ||
            stateSignalContainsDetail(stateSpy,
                                      moduleId,
                                      QStringLiteral("Stopped"),
                                      QString::fromUtf8(u8"停止超时")));
    QVERIFY(!supervisor.runningModuleIds().contains(moduleId));

    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(1000));
    QVERIFY(bus.unregisterModule(QStringLiteral("HungPublisher"), false));
    QVERIFY(bus.unregisterModule(QStringLiteral("HungObserver"), false));
}

// fail-fast 使用可替换入口；测试记录请求并释放卡死条件，不结束 Qt Test 主进程。
void BaselineTest::shutdownRequestsFailFastForHungInProcessModule()
{
    qframework::MessageBusConfig config = faultBusConfig();
    config.shutdownDrainTimeoutMs = 80;
    qframework::MessageBus bus(config);
    BusTestModule publisher(QStringList() << QStringLiteral("TEST_BLOCKING_CALLBACK"),
                            QStringList());
    BlockingBusModule blocker;
    QString error;
    QVERIFY(bus.registerModule(QStringLiteral("FailFastPublisher"), &publisher, &error));
    QVERIFY(bus.registerModule(QStringLiteral("FailFastModule"), &blocker, &error));
    QVERIFY(bus.setModuleRunning(QStringLiteral("FailFastPublisher"), true));
    QVERIFY(bus.setModuleRunning(QStringLiteral("FailFastModule"), true));
    bus.setDeliveryEnabled(true);
    QVERIFY(publisher.publish(QStringLiteral("TEST_BLOCKING_CALLBACK"),
                              QByteArrayLiteral("block")));
    QVERIFY(blocker.waitUntilEntered(1000));
    bus.beginShutdown();
    const qframework::MessageBusStopReport report = bus.stopQueues(80);
    QVERIFY(report.timedOutModuleIds.contains(QStringLiteral("FailFastModule")));

    {
        QMutexLocker locker(&failFastRecordMutex);
        failFastRequested = false;
        failFastReason.clear();
    }
    qframework::ProcessFailFast::setHandlerForTests(&recordFailFastRequest);
    QVERIFY(qframework::ProcessFailFast::requestForHungModules(
        report.timedOutModuleIds));
    qframework::ProcessFailFast::setHandlerForTests(nullptr);
    {
        QMutexLocker locker(&failFastRecordMutex);
        QVERIFY(failFastRequested);
        QVERIFY(failFastReason.contains(QStringLiteral("FailFastModule")));
    }

    blocker.release();
    QTRY_VERIFY_WITH_TIMEOUT(bus.isModuleQueueStopped(QStringLiteral("FailFastModule")), 1000);
    QVERIFY(bus.unregisterModule(QStringLiteral("FailFastModule"), false));
    QVERIFY(bus.unregisterModule(QStringLiteral("FailFastPublisher"), false));
}

// 源码级守卫配合上一项进程行为测试，防止未来重新引入 QThread::terminate()。
void BaselineTest::messageQueueNeverUsesThreadTerminate()
{
    const QString sourcePath = QDir::cleanPath(
        QCoreApplication::applicationDirPath() +
        QStringLiteral("/../../../../Source/QFramework/Process/ProcessRuntime.cpp"));
    QFile sourceFile(sourcePath);
    QVERIFY2(sourceFile.open(QIODevice::ReadOnly), qPrintable(sourcePath));
    const QByteArray source = sourceFile.readAll();
    QVERIFY(!source.contains("QThread::terminate"));
    QVERIFY(!source.contains("terminate();"));
}

// 四个业务 QThread 持续 publish，连续重启三轮，验证 stop/delete 与 enqueue 同步。
void BaselineTest::processPublishGateIsThreadSafeDuringStop()
{
    const qframework::MessageBusConfig busConfig = queueTestBusConfig();
    qframework::MessageBus bus(busConfig);
    qframework::ProcessSupervisor supervisor(
        &bus, busConfig, queueTestProcessConfig());
    QSignalSpy restartSpy(&supervisor, &qframework::ProcessSupervisor::restartFinished);
    QSignalSpy faultSpy(&supervisor, &qframework::ProcessSupervisor::moduleFault);

    const QString moduleId = QStringLiteral("RuntimePublishStress");
    QVector<qframework::ModuleConfig> modules;
    modules.append(queueTestModuleConfig(moduleId));
    QStringList errors;
    QVERIFY2(supervisor.startAll(modules, &errors), qPrintable(errors.join('\n')));
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.state(moduleId), QStringLiteral("Running"), 4000);

    for (int round = 0; round < 3; ++round) {
        QTest::qWait(100);
        QString error;
        const int expectedSignals = restartSpy.count() + 1;
        QVERIFY2(supervisor.requestRestart(moduleId, &error), qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(restartSpy.count(), expectedSignals, 5000);
        QVERIFY2(restartSpy.last().at(1).toBool(),
                 qPrintable(restartSpy.last().at(2).toString()));
        QCOMPARE(supervisor.state(moduleId), QStringLiteral("Running"));
    }

    QString error;
    QVERIFY2(supervisor.requestStop(moduleId, &error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.state(moduleId), QStringLiteral("Stopped"), 4000);
    QVERIFY(faultSpy.isEmpty());
    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(1000));
}

// 控制帧失败分支不易靠操作系统稳定制造短写，使用源码守卫锁定立即 fault 顺序。
void BaselineTest::processControlWriteFailureTriggersFaultImmediately()
{
    const QString root = QDir::cleanPath(
        QCoreApplication::applicationDirPath() + QStringLiteral("/../../../../"));
    QFile supervisorFile(QDir(root).filePath(
        QStringLiteral("Source/QFramework/Process/ProcessSupervisor.cpp")));
    QFile runtimeFile(QDir(root).filePath(
        QStringLiteral("Source/QFramework/Process/ProcessRuntime.cpp")));
    QFile runtimeHeader(QDir(root).filePath(
        QStringLiteral("Source/QFrameworkSdk/ProcessRuntime.h")));
    QVERIFY(supervisorFile.open(QIODevice::ReadOnly));
    QVERIFY(runtimeFile.open(QIODevice::ReadOnly));
    QVERIFY(runtimeHeader.open(QIODevice::ReadOnly));
    QByteArray supervisor = supervisorFile.readAll();
    QByteArray runtime = runtimeFile.readAll();
    const QByteArray header = runtimeHeader.readAll();
    for (QByteArray* source : {&supervisor, &runtime}) {
        source->replace(" ", "");
        source->replace("\t", "");
        source->replace("\r", "");
        source->replace("\n", "");
    }

    const int pingCheck = supervisor.indexOf("if(!sendFrame(entry,ping))");
    const int pingFault = supervisor.indexOf("handleFault(entry", pingCheck);
    const int pingTimestamp = supervisor.indexOf("entry->lastPingMs=now", pingCheck);
    QVERIFY(pingCheck >= 0);
    QVERIFY(pingFault > pingCheck);
    QVERIFY(pingTimestamp > pingFault);
    QVERIFY(header.contains("bool sendFrame(const QJsonObject& frame)"));
    QVERIFY(runtime.contains("if(!sendFrame(startedFrame))"));
    QVERIFY(runtime.contains("if(!sendFrame(pong))"));
    QVERIFY(runtime.contains("if(!sendFrame(ack))"));
    QVERIFY(runtime.contains("if(!sendFrame(frame))"));
    QVERIFY(supervisor.contains(
        "if(!sendFrame(entry,ack)&&!isStoppingPhase(entry->phase))"));
}

// 故障客户端故意不回 deliveryAck，稳定形成等待、在途和容量拒绝三类项。
void BaselineTest::processStopAccountingIncludesDiscardedMessages()
{
    qframework::MessageBusConfig busConfig = faultBusConfig();
    busConfig.topics.insert(
        QStringLiteral("TEST_ACCOUNTING"),
        queueTopicConfig(1, qframework::QueuePolicy::Reliable));
    qframework::MessageBus bus(busConfig);
    BusTestModule publisher(QStringList() << QStringLiteral("TEST_ACCOUNTING"),
                            QStringList());
    QString error;
    QVERIFY2(bus.registerModule(QStringLiteral("AccountingPublisher"), &publisher, &error),
             qPrintable(error));
    QVERIFY(bus.setModuleRunning(QStringLiteral("AccountingPublisher"), true));

    qframework::ProcessSupervisor supervisor(
        &bus, busConfig, lifecycleProcessConfig(1000));
    const QString moduleId = QStringLiteral("AccountingModule");
    QVector<qframework::ModuleConfig> modules;
    modules.append(faultProcessModuleConfig(moduleId));
    QStringList errors;
    QVERIFY2(supervisor.startAll(modules, &errors), qPrintable(errors.join('\n')));
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.state(moduleId), QStringLiteral("Running"), 3000);
    bus.setDeliveryEnabled(true);

    QVERIFY(publisher.publish(QStringLiteral("TEST_ACCOUNTING"), QByteArrayLiteral("one")));
    QTRY_VERIFY_WITH_TIMEOUT(supervisor.queueStats(moduleId).inFlight >= 1, 1500);
    QVERIFY(publisher.publish(QStringLiteral("TEST_ACCOUNTING"), QByteArrayLiteral("two")));
    QTRY_VERIFY_WITH_TIMEOUT(supervisor.queueStats(moduleId).pending >= 1, 1500);
    for (int index = 0; index < 20 &&
         supervisor.queueStats(moduleId).rejected == 0; ++index) {
        publisher.publish(QStringLiteral("TEST_ACCOUNTING"), QByteArray::number(index));
        QTest::qWait(10);
    }
    QTRY_VERIFY_WITH_TIMEOUT(supervisor.queueStats(moduleId).rejected >= 1, 1500);

    QVERIFY2(supervisor.requestStop(moduleId, &error), qPrintable(error));
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.state(moduleId), QStringLiteral("Stopped"), 3000);
    const qframework::ProcessQueueStats stats = supervisor.queueStats(moduleId);
    QVERIFY(stats.dropped >= 1);
    QVERIFY(stats.rejected >= 1);
    QVERIFY(stats.abandoned >= 1);
    QCOMPARE(stats.pending, 0);
    QCOMPARE(stats.inFlight, 0);

    supervisor.shutdown();
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(1000));
    QVERIFY(bus.unregisterModule(QStringLiteral("AccountingPublisher"), false));
}

// 精确边界由源码守卫固定为 >=，避免依赖毫秒调度去“碰巧”命中等号。
void BaselineTest::processRestartWindowBoundaryIsInclusive()
{
    const QString path = QDir::cleanPath(
        QCoreApplication::applicationDirPath() +
        QStringLiteral("/../../../../Source/QFramework/Process/ProcessSupervisor.cpp"));
    QFile file(path);
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
    QByteArray source = file.readAll();
    source.replace(" ", "");
    source.replace("\t", "");
    source.replace("\r", "");
    source.replace("\n", "");
    QVERIFY(source.contains(
        "now-entry->restartWindowStartMs>=qMax(1,processConfig_.restartWindowMs)"));
    QVERIFY(!source.contains(
        "now-entry->restartWindowStartMs>qMax(1,processConfig_.restartWindowMs)"));
}

// queuePublish 只能访问 PublishQueue；所有 QLocalSocket 读写都留在 Runtime 线程。
void BaselineTest::processSocketAccessStaysOnRuntimeThread()
{
    const QString root = QDir::cleanPath(
        QCoreApplication::applicationDirPath() + QStringLiteral("/../../../../"));
    const QString runtimePath = QDir(root).filePath(
        QStringLiteral("Source/QFramework/Process/ProcessRuntime.cpp"));
    const QString supervisorPath = QDir(root).filePath(
        QStringLiteral("Source/QFramework/Process/ProcessSupervisor.cpp"));
    QFile runtimeFile(runtimePath);
    QFile supervisorFile(supervisorPath);
    QVERIFY2(runtimeFile.open(QIODevice::ReadOnly), qPrintable(runtimePath));
    QVERIFY2(supervisorFile.open(QIODevice::ReadOnly), qPrintable(supervisorPath));
    const QByteArray runtime = runtimeFile.readAll();
    const QByteArray supervisor = supervisorFile.readAll();

    const int publishStart = runtime.indexOf("bool ProcessRuntime::queuePublish");
    const int publishEnd = runtime.indexOf("// 日志体较小", publishStart);
    QVERIFY(publishStart >= 0 && publishEnd > publishStart);
    const QByteArray publishBody = runtime.mid(publishStart, publishEnd - publishStart);
    QVERIFY(!publishBody.contains("running_"));
    QVERIFY(!publishBody.contains("socket_"));
    QCOMPARE(runtime.count("socket_->write("), 1);
    QVERIFY(runtime.contains("Q_ASSERT(QThread::currentThread() == thread())"));
    QVERIFY(runtime.contains(
        "Q_ASSERT(socket_ == nullptr || socket_->thread() == QThread::currentThread())"));

    const int publishQueueStart = runtime.indexOf("class ProcessRuntime::PublishQueue");
    const int messageQueueStart = runtime.indexOf("class ProcessRuntime::MessageQueue");
    QVERIFY(publishQueueStart >= 0 && messageQueueStart > publishQueueStart);
    QVERIFY(!runtime.mid(publishQueueStart, messageQueueStart - publishQueueStart)
                 .contains("available_"));
    QVERIFY(!supervisor.contains("outgoingChanged"));
}

// 目的：验证 SingleInstanceGuard 以目录为作用域阻止同目录第二个实例。
// 准备：创建两个临时目录；动作：第一个守护同目录，再尝试第二个及另一目录。
// 断言：同目录第二次失败，另一目录成功，释放后原目录可再次获取。
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

// 目的：验证布局管理器能保存/恢复窗口状态，并注册 Dock 的白名单规则。
// 准备：在临时 QSettings 文件中创建主窗口和两个 Dock；动作：保存、修改、恢复。
// 断言：几何/状态恢复，允许 Dock 可见，未注册 Dock 被隐藏或拒绝。
void BaselineTest::layoutPersistenceAndDockingRules()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString layoutPath = directory.filePath(QStringLiteral("workspace.qflayout"));
    const QString damagedPath = directory.filePath(QStringLiteral("damaged.qflayout"));

    QMainWindow window;
    window.setObjectName(QStringLiteral("LayoutTestWindow"));
    window.resize(900, 600);
    window.setDockNestingEnabled(true);
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
    QHash<QString, bool> requestedVisibility;
    requestedVisibility.insert(QStringLiteral("First"), true);
    requestedVisibility.insert(QStringLiteral("Second"), false);
    QString error;
    QVERIFY2(manager.saveLayout(layoutPath, requestedVisibility, &error), qPrintable(error));

    QFile layoutFile(layoutPath);
    QVERIFY(layoutFile.open(QIODevice::ReadOnly));
    QJsonDocument layoutDocument = QJsonDocument::fromJson(layoutFile.readAll());
    layoutFile.close();
    QVERIFY(layoutDocument.isObject());
    QJsonObject root = layoutDocument.object();
    QJsonObject modules = root.value(QStringLiteral("modules")).toObject();
    QJsonObject missingModule;
    missingModule.insert(QStringLiteral("visible"), true);
    missingModule.insert(QStringLiteral("requestedVisible"), true);
    modules.insert(QStringLiteral("MissingModule"), missingModule);
    root.insert(QStringLiteral("modules"), modules);
    QVERIFY(layoutFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(layoutFile.write(QJsonDocument(root).toJson()),
             QJsonDocument(root).toJson().size());
    layoutFile.close();

    firstDock.hide();
    secondDock.show();
    QHash<QString, bool> loadedVisibility;
    QStringList unavailableModules;
    bool legacyVisibility = true;
    QVERIFY2(manager.loadLayout(layoutPath,
                                &loadedVisibility,
                                &error,
                                &unavailableModules,
                                &legacyVisibility),
              qPrintable(error));
    QCOMPARE(unavailableModules, QStringList() << QStringLiteral("MissingModule"));
    QVERIFY(!legacyVisibility);
    QVERIFY(loadedVisibility.value(QStringLiteral("First")));
    QVERIFY(!loadedVisibility.value(QStringLiteral("Second")));
    QVERIFY(firstDock.isVisible());
    QVERIFY(!secondDock.isVisible());
    QCOMPARE(manager.activeFilePath(), QFileInfo(layoutPath).absoluteFilePath());

    QVERIFY(firstDock.features().testFlag(QDockWidget::DockWidgetMovable));
    QVERIFY(!firstDock.features().testFlag(QDockWidget::DockWidgetFloatable));

    window.addDockWidget(Qt::LeftDockWidgetArea, &secondDock);
    secondDock.show();
    QCoreApplication::processEvents();

    window.splitDockWidget(&firstDock, &secondDock, Qt::Horizontal);
    QCoreApplication::processEvents();

    QVERIFY(window.tabifiedDockWidgets(&firstDock).isEmpty());
    QCOMPARE(window.dockWidgetArea(&firstDock), Qt::LeftDockWidgetArea);
    QCOMPARE(window.dockWidgetArea(&secondDock), Qt::LeftDockWidgetArea);

    window.tabifyDockWidget(&firstDock, &secondDock);
    QVERIFY(window.tabifiedDockWidgets(&firstDock).contains(&secondDock));

    const QByteArray stateBeforeFailure = window.saveState(1);
    const QString activeBeforeFailure = manager.activeFilePath();
    QFile damagedFile(damagedPath);
    QVERIFY(damagedFile.open(QIODevice::WriteOnly));
    QCOMPARE(damagedFile.write("{ damaged"), qint64(9));
    damagedFile.close();
    QVERIFY(!manager.loadLayout(damagedPath, &loadedVisibility, &error));
    QCOMPARE(window.saveState(1), stateBeforeFailure);
    QCOMPARE(manager.activeFilePath(), activeBeforeFailure);
}

// 菜单勾选表示用户意图，取消勾选必须立即隐藏已经 ready 的 Dock。
void BaselineTest::moduleMenuTogglesRequestedVisibility()
{
    QVector<qframework::ModuleConfig> modules;
    modules.append(pluginConfig(QStringLiteral("MenuUi"), qframework::ModuleType::InProcessUi));
    qframework::MainWindow window(modules, nullptr, nullptr, nullptr);
    window.show();
    QVERIFY(QMetaObject::invokeMethod(&window,
                                      "setUiAvailable",
                                      Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("MenuUi")),
                                      Q_ARG(bool, true)));
    QAction* action = window.findChild<QAction*>(QStringLiteral("ModuleAction.MenuUi"));
    QDockWidget* dock = window.findChild<QDockWidget*>(QStringLiteral("ModuleDock.MenuUi"));
    QVERIFY(action != nullptr);
    QVERIFY(dock != nullptr);

    action->trigger();
    QTRY_VERIFY(action->isChecked());
    QTRY_VERIFY(dock->isVisible());
    action->trigger();
    QTRY_VERIFY(!action->isChecked());
    QTRY_VERIFY(!dock->isVisible());
}

// Dock 关闭按钮与标签切换分离：只有 close 才撤销菜单勾选。
void BaselineTest::dockCloseClearsRequestedVisibility()
{
    QVector<qframework::ModuleConfig> modules;
    modules.append(pluginConfig(QStringLiteral("CloseUi"), qframework::ModuleType::InProcessUi));
    qframework::MainWindow window(modules, nullptr, nullptr, nullptr);
    window.show();
    QVERIFY(QMetaObject::invokeMethod(&window, "setUiAvailable", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("CloseUi")), Q_ARG(bool, true)));
    QAction* action = window.findChild<QAction*>(QStringLiteral("ModuleAction.CloseUi"));
    QDockWidget* dock = window.findChild<QDockWidget*>(QStringLiteral("ModuleDock.CloseUi"));
    QVERIFY(action != nullptr);
    QVERIFY(dock != nullptr);
    action->trigger();
    QTRY_VERIFY(dock->isVisible());
    QVERIFY(dock->close());
    QTRY_VERIFY(!action->isChecked());
}

// 两个标签来回 raise 只改变 Qt 当前绘制页，不改变两个 QAction 的用户意图。
void BaselineTest::tabSwitchKeepsRequestedVisibility()
{
    QVector<qframework::ModuleConfig> modules;
    modules.append(pluginConfig(QStringLiteral("TabOne"), qframework::ModuleType::InProcessUi));
    modules.append(pluginConfig(QStringLiteral("TabTwo"), qframework::ModuleType::InProcessUi));
    qframework::MainWindow window(modules, nullptr, nullptr, nullptr);
    window.show();
    const QStringList moduleIds = QStringList()
        << QStringLiteral("TabOne") << QStringLiteral("TabTwo");
    for (const QString& id : moduleIds) {
        QVERIFY(QMetaObject::invokeMethod(&window, "setUiAvailable", Qt::DirectConnection,
                                          Q_ARG(QString, id), Q_ARG(bool, true)));
        QAction* action = window.findChild<QAction*>(QStringLiteral("ModuleAction.%1").arg(id));
        QVERIFY(action != nullptr);
        action->trigger();
    }
    QDockWidget* first = window.findChild<QDockWidget*>(QStringLiteral("ModuleDock.TabOne"));
    QDockWidget* second = window.findChild<QDockWidget*>(QStringLiteral("ModuleDock.TabTwo"));
    QVERIFY(first != nullptr);
    QVERIFY(second != nullptr);
    QVERIFY(window.tabifiedDockWidgets(first).contains(second));
    first->raise();
    second->raise();
    QCoreApplication::processEvents();
    QVERIFY(window.findChild<QAction*>(QStringLiteral("ModuleAction.TabOne"))->isChecked());
    QVERIFY(window.findChild<QAction*>(QStringLiteral("ModuleAction.TabTwo"))->isChecked());
}

// 保存的是两个 Dock 的显示意图，restoreState 仍负责恢复标签关系和当前页。
void BaselineTest::layoutPreservesTabbedVisibilityIntent()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("tabs.qflayout"));
    QMainWindow window;
    qframework::ManagedDockWidget first(QStringLiteral("First"), &window);
    qframework::ManagedDockWidget second(QStringLiteral("Second"), &window);
    first.setObjectName(QStringLiteral("ModuleDock.First"));
    second.setObjectName(QStringLiteral("ModuleDock.Second"));
    window.addDockWidget(Qt::LeftDockWidgetArea, &first);
    window.addDockWidget(Qt::LeftDockWidgetArea, &second);
    window.tabifyDockWidget(&first, &second);
    window.show();
    first.show();
    second.show();
    first.raise();
    qframework::LayoutManager manager(&window);
    manager.registerModuleDock(QStringLiteral("First"), &first);
    manager.registerModuleDock(QStringLiteral("Second"), &second);
    QHash<QString, bool> requested;
    requested.insert(QStringLiteral("First"), true);
    requested.insert(QStringLiteral("Second"), true);
    QString error;
    QVERIFY2(manager.saveLayout(path, requested, &error), qPrintable(error));

    window.removeDockWidget(&second);
    window.addDockWidget(Qt::RightDockWidgetArea, &second);
    QHash<QString, bool> loaded;
    QVERIFY2(manager.loadLayout(path, &loaded, &error), qPrintable(error));
    QVERIFY(loaded.value(QStringLiteral("First")));
    QVERIFY(loaded.value(QStringLiteral("Second")));
    QVERIFY(window.tabifiedDockWidgets(&first).contains(&second));
}

// 未 ready 时保留 checked/pending；ready 到达后自动应用显示意图。
void BaselineTest::layoutDefersUnavailableDockUntilReady()
{
    QVector<qframework::ModuleConfig> modules;
    modules.append(pluginConfig(QStringLiteral("PendingUi"), qframework::ModuleType::InProcessUi));
    qframework::MainWindow window(modules, nullptr, nullptr, nullptr);
    window.show();
    QAction* action = window.findChild<QAction*>(QStringLiteral("ModuleAction.PendingUi"));
    QDockWidget* dock = window.findChild<QDockWidget*>(QStringLiteral("ModuleDock.PendingUi"));
    QVERIFY(action != nullptr);
    QVERIFY(dock != nullptr);
    action->trigger();
    QVERIFY(action->isChecked());
    QVERIFY(!dock->isVisible());
    QVERIFY(QMetaObject::invokeMethod(&window, "setUiAvailable", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("PendingUi")), Q_ARG(bool, true)));
    QTRY_VERIFY(dock->isVisible());
}

// 明确取消 pending 后，即使窗口随后 ready 也不能被补显示。
void BaselineTest::layoutHiddenDockStaysHiddenAfterReady()
{
    QVector<qframework::ModuleConfig> modules;
    modules.append(pluginConfig(QStringLiteral("HiddenUi"), qframework::ModuleType::InProcessUi));
    qframework::MainWindow window(modules, nullptr, nullptr, nullptr);
    window.show();
    QAction* action = window.findChild<QAction*>(QStringLiteral("ModuleAction.HiddenUi"));
    QDockWidget* dock = window.findChild<QDockWidget*>(QStringLiteral("ModuleDock.HiddenUi"));
    QVERIFY(action != nullptr);
    QVERIFY(dock != nullptr);
    action->trigger();
    action->trigger();
    QVERIFY(!action->isChecked());
    QVERIFY(QMetaObject::invokeMethod(&window, "setUiAvailable", Qt::DirectConnection,
                                      Q_ARG(QString, QStringLiteral("HiddenUi")), Q_ARG(bool, true)));
    QVERIFY(!dock->isVisible());
}

// 真实 ProcessUi 窗口晚到时只应用布局保存的显示意图：显示布局会补发 showWindow，
// 明确隐藏布局在重启后的新 windowReady 到达时保持隐藏，之后菜单勾选仍能显示。
void BaselineTest::processUiLateWindowHonorsLayoutVisibility()
{
    const QString moduleId = QStringLiteral("ProcessUiExample");
    const QString executablePath = processModulePath(moduleId);
    QVERIFY2(QFileInfo::exists(executablePath), qPrintable(executablePath));

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
    qframework::ProcessSupervisor supervisor(&bus, busConfig, processConfig);
    QVector<qframework::ModuleConfig> modules;
    modules.append(processModuleConfig(moduleId, qframework::ModuleType::ProcessUi));
    qframework::MainWindow window(modules, nullptr, &supervisor, nullptr);
    window.show();

    QAction* action = window.findChild<QAction*>(
        QStringLiteral("ModuleAction.%1").arg(moduleId));
    QDockWidget* dock = window.findChild<QDockWidget*>(
        QStringLiteral("ModuleDock.%1").arg(moduleId));
    qframework::ProcessWindowHost* host =
        qobject_cast<qframework::ProcessWindowHost*>(dock == nullptr ? nullptr : dock->widget());
    QVERIFY(action != nullptr);
    QVERIFY(dock != nullptr);
    QVERIFY(host != nullptr);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString visiblePath = directory.filePath(QStringLiteral("visible.qflayout"));
    const QString hiddenPath = directory.filePath(QStringLiteral("hidden.qflayout"));
    QString error;
    QHash<QString, bool> visibility;
    visibility.insert(moduleId, true);
    QVERIFY2(window.layoutManager()->saveLayout(visiblePath, visibility, &error),
             qPrintable(error));
    QVERIFY2(window.loadLayoutFile(visiblePath, &error), qPrintable(error));
    QVERIFY(action->isChecked());
    QVERIFY(!dock->isVisible());

    qRegisterMetaType<quintptr>("quintptr");
    QSignalSpy windowSpy(&supervisor, &qframework::ProcessSupervisor::windowHandleReady);
    QStringList errors;
    QVERIFY2(supervisor.startAll(modules, &errors), qPrintable(errors.join('\n')));
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.state(moduleId), QStringLiteral("Running"), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(windowSpy.count() >= 1, 5000);
    const quintptr initialWindowId = static_cast<quintptr>(
        windowSpy.first().at(1).toULongLong());
    QVERIFY(initialWindowId != 0);
    QTRY_VERIFY_WITH_TIMEOUT(dock->isVisible(), 2000);
#ifdef Q_OS_WIN
    QTRY_VERIFY_WITH_TIMEOUT(nativeWindowVisible(initialWindowId), 2000);
    // Dock 尺寸变化只发送合并后的 resizeWindow，原生客户区最终应与宿主一致。
    QTRY_COMPARE_WITH_TIMEOUT(nativeWindowClientSize(initialWindowId), host->size(), 2000);
    window.resize(960, 620);
    QTRY_COMPARE_WITH_TIMEOUT(nativeWindowClientSize(initialWindowId), host->size(), 2000);
    window.resize(700, 480);
    QTRY_COMPARE_WITH_TIMEOUT(nativeWindowClientSize(initialWindowId), host->size(), 2000);
#endif
    QVERIFY(window.minimumSize().width() < 2000);
    QVERIFY(window.minimumSize().height() < 1600);

    // 隐藏意图在重启前写入；新 HWND 晚到时只能嵌入，不能自动补发显示命令。
    visibility.insert(moduleId, false);
    QVERIFY2(window.layoutManager()->saveLayout(hiddenPath, visibility, &error),
             qPrintable(error));
    QVERIFY2(window.loadLayoutFile(hiddenPath, &error), qPrintable(error));
    QVERIFY(!action->isChecked());
    QVERIFY(!dock->isVisible());

    const int priorWindowCount = windowSpy.count();
    QVERIFY2(supervisor.requestRestart(moduleId, &error), qPrintable(error));
    QTRY_VERIFY_WITH_TIMEOUT(windowSpy.count() > priorWindowCount, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(supervisor.state(moduleId), QStringLiteral("Running"), 10000);
    const quintptr restartedWindowId = static_cast<quintptr>(
        windowSpy.last().at(1).toULongLong());
    QVERIFY(restartedWindowId != 0);
    QTest::qWait(250);
    QVERIFY(!action->isChecked());
    QVERIFY(!dock->isVisible());
#ifdef Q_OS_WIN
    QVERIFY(!nativeWindowVisible(restartedWindowId));
    // 隐藏期间即使主窗口继续 resize，也不能靠尺寸帧把子窗口补显示。
    window.resize(850, 560);
    QTest::qWait(120);
    QVERIFY(!nativeWindowVisible(restartedWindowId));
#endif

    // 窗口已经 ready 后，用户再次勾选仍会发送 showWindow 并显示同一原生窗口。
    action->trigger();
    QVERIFY(action->isChecked());
    QTRY_VERIFY_WITH_TIMEOUT(dock->isVisible(), 2000);
#ifdef Q_OS_WIN
    QTRY_VERIFY_WITH_TIMEOUT(nativeWindowVisible(restartedWindowId), 2000);
    QTRY_COMPARE_WITH_TIMEOUT(nativeWindowClientSize(restartedWindowId), host->size(), 2000);
#endif

    supervisor.shutdown();
    QVERIFY(supervisor.runningModuleIds().isEmpty());
    bus.beginShutdown();
    QVERIFY(bus.stopQueues(2000));
}

// 旧布局按 visible 兼容读取；损坏文件仍完整保留当前 state 和活动路径。
void BaselineTest::legacyLayoutLoadsWithoutCorruptingCurrentState()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString legacyPath = directory.filePath(QStringLiteral("legacy.qflayout"));
    const QString damagedPath = directory.filePath(QStringLiteral("damaged.qflayout"));
    QMainWindow window;
    qframework::ManagedDockWidget dock(QStringLiteral("Legacy"), &window);
    dock.setObjectName(QStringLiteral("ModuleDock.Legacy"));
    window.addDockWidget(Qt::LeftDockWidgetArea, &dock);
    window.show();
    qframework::LayoutManager manager(&window);
    manager.registerModuleDock(QStringLiteral("Legacy"), &dock);
    QHash<QString, bool> requested;
    requested.insert(QStringLiteral("Legacy"), true);
    QString error;
    QVERIFY2(manager.saveLayout(legacyPath, requested, &error), qPrintable(error));

    QFile file(legacyPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    root.remove(QStringLiteral("visibilitySemantics"));
    QJsonObject modules = root.value(QStringLiteral("modules")).toObject();
    QJsonObject state = modules.value(QStringLiteral("Legacy")).toObject();
    state.remove(QStringLiteral("requestedVisible"));
    modules.insert(QStringLiteral("Legacy"), state);
    root.insert(QStringLiteral("modules"), modules);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray legacyData = QJsonDocument(root).toJson();
    QCOMPARE(file.write(legacyData), qint64(legacyData.size()));
    file.close();

    dock.hide();
    QHash<QString, bool> loaded;
    bool legacy = false;
    QVERIFY2(manager.loadLayout(legacyPath, &loaded, &error, nullptr, &legacy), qPrintable(error));
    QVERIFY(legacy);
    QVERIFY(loaded.value(QStringLiteral("Legacy")));
    const QByteArray stateBeforeFailure = window.saveState(1);
    const QString activeBeforeFailure = manager.activeFilePath();
    QFile damaged(damagedPath);
    QVERIFY(damaged.open(QIODevice::WriteOnly));
    QCOMPARE(damaged.write("{ damaged"), qint64(9));
    damaged.close();
    QVERIFY(!manager.loadLayout(damagedPath, &loaded, &error));
    QCOMPARE(window.saveState(1), stateBeforeFailure);
    QCOMPARE(manager.activeFilePath(), activeBeforeFailure);
}

// 目的：验证样式表首次加载、运行时重载和失败回退。
// 准备：写入有效及无效 QSS 文件；动作：依次加载、重载和删除文件。
// 断言：有效内容作用于应用，错误不会清空旧样式，并返回可诊断错误文本。
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

// 测试程序的双身份入口：普通启动运行 Qt Test；带监督器参数时运行故障/IPC
// 子进程客户端。必须在 QApplication 与 QCoreApplication 之间按参数选择，
// 因为 UI 进程需要 GUI 事件循环，而纯协议故障客户端不应额外创建窗口系统。
int main(int argc, char* argv[])
{
    if (hasSupervisorArguments(argc, argv))
        return runFaultProcessClient(argc, argv);

    QApplication application(argc, argv);
    BaselineTest test;
    return QTest::qExec(&test, argc, argv);
}
