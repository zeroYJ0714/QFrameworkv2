#include "ProcessSupervisor.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMutexLocker>
#include <QProcessEnvironment>
#include <QQueue>
#include <QSet>
#include <QSharedMemory>
#include <QThread>
#include <QUuid>
#include <QWaitCondition>

#include <cstring>

#include "Logger.h"
#include "MessageBus.h"
#include "ModuleEndpoint.h"
#include "ProcessProtocol.h"
#include "ProcessRuntime.h"

// 主进程监督器的数据流：MessageBus 回调 -> outgoingQueue -> Socket 帧，
// 子进程帧 -> handleFrame -> MessageBus；Entry 记录一份子进程的全部资源。

namespace qframework
{
namespace
{
// 所有 type 字符串都是父子 IPC 的内部协议值，不属于模块公开主题。
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
const QString kDeliveryAck = QStringLiteral("deliveryAck");
const QString kPublishAck = QStringLiteral("publishAck");
const QString kDebugWaitTimeout = QStringLiteral("debugWaitTimeout");
const QString kStyleSheet = QStringLiteral("styleSheet");

QString processTypeName(ModuleType type)
{
    // 配置枚举转换成注册帧中的稳定文本，只允许两种子进程类型。
    if (type == ModuleType::ProcessUi)
        return QStringLiteral("ProcessUi");
    if (type == ModuleType::ProcessNonUi)
        return QStringLiteral("ProcessNonUi");
    return QString();
}

bool isProcessType(ModuleType type)
{
    // startAll 用它跳过由 PluginManager 负责的两个主进程类型。
    return type == ModuleType::ProcessUi || type == ModuleType::ProcessNonUi;
}

QString makeServerName(const QString& moduleId)
{
    // 模块 ID + 父进程 PID + UUID 组合，避免重启或并行部署时名称冲突。
    return QStringLiteral("QFramework_%1_%2_%3")
        .arg(moduleId,
             QString::number(QCoreApplication::applicationPid()),
             QUuid::createUuid().toString(QUuid::Id128));
}

QString makeSharedKey(const QString& moduleId)
{
    // 每条大消息使用独立 UUID，ACK 后立即释放，不复用旧共享段。
    return QStringLiteral("QFrameworkShared_%1_%2")
        .arg(moduleId, QUuid::createUuid().toString(QUuid::Id128));
}

int maxFrameBytes(int maxMessageBytes)
{
    // inline payload 经 base64/JSON 会膨胀，因此取消息上限两倍并设置硬边界。
    const qint64 value = qMax<qint64>(64 * 1024,
                                      static_cast<qint64>(maxMessageBytes) * 2);
    return static_cast<int>(qMin<qint64>(value, 128 * 1024 * 1024));
}

bool jsonStringList(const QJsonValue& value, QStringList* result)
{
    // 注册主题必须是纯字符串数组，任一元素类型错误就拒绝整次注册。
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
    // 统一拼接用户可读错误，空 detail 时不多加冒号。
    if (detail.isEmpty())
        return prefix;
    return prefix + QStringLiteral(": ") + detail;
}

struct OutgoingMessage
{
    // 这是“父进程准备发给某个子进程”的待发送项。
    // 它先放在内存队列里，只有真正取得一个在途槽位时才会生成
    // messageId。这样被 Latest 丢弃的等待项不会留下永远等不到 ACK 的记录。
    QString topic;
    QString senderModuleId;
    QByteArray data;
};

TopicConfig effectiveTopicConfig(const MessageBusConfig& config,
                                 const QString& topic)
{
    // 主题没有单独配置时，沿用 MessageBus 的默认容量、大小上限和策略。
    // qMax(1, ...) 是最后一道保护，避免配置错误把队列容量变成 0，
    // 进而让“永远无法发送”的状态传播到跨进程队列。
    TopicConfig result = config.topics.value(topic);
    if (!config.topics.contains(topic)) {
        result.queueCapacity = config.defaultQueueCapacity;
        result.maxMessageBytes = config.maxMessageBytes;
        result.policy = config.defaultPolicy;
    }
    result.queueCapacity = qMax(1, result.queueCapacity);
    result.maxMessageBytes = qMax(1, result.maxMessageBytes);
    return result;
}

QString queuePolicyName(QueuePolicy policy)
{
    // registerAck 使用稳定英文值，子进程按大小写不敏感方式解析。
    return policy == QueuePolicy::Latest
        ? QStringLiteral("Latest")
        : QStringLiteral("Reliable");
}

int queuedTopicCount(const QQueue<OutgoingMessage>& queue,
                     const QString& topic)
{
    // 容量按主题计算，而不是把不同主题互相挤占同一个数字。
    int count = 0;
    for (const OutgoingMessage& message : queue) {
        if (message.topic == topic)
            ++count;
    }
    return count;
}

void appendTopicConfig(const MessageBusConfig& config,
                       const QString& topic,
                       QJsonArray* array,
                       QSet<QString>* seen)
{
    // seen 防止一个主题同时出现在 published/subscribed 时被下发两次。
    if (array == nullptr || seen == nullptr || topic.isEmpty() || seen->contains(topic))
        return;
    seen->insert(topic);
    const TopicConfig value = effectiveTopicConfig(config, topic);
    QJsonObject item;
    item.insert(QStringLiteral("topic"), topic);
    item.insert(QStringLiteral("queueCapacity"), value.queueCapacity);
    item.insert(QStringLiteral("maxMessageBytes"), value.maxMessageBytes);
    item.insert(QStringLiteral("policy"), queuePolicyName(value.policy));
    array->append(item);
}

void appendTopicConfig(const MessageBusConfig& config,
                       const QStringList& topics,
                       QJsonArray* array,
                       QSet<QString>* seen)
{
    // 列表重载逐项复用单主题校验和去重逻辑。
    for (const QString& topic : topics)
        appendTopicConfig(config, topic, array, seen);
}
}

// MessageBus 注册的“虚拟订阅模块”。它把子进程订阅主题接入总线，收到消息后
// 只做线程安全入队；真正写 Socket 必须回到监督器线程，避免跨线程操作 QLocalSocket。
class ProcessBridge final : public ModuleEndpoint
{
public:
    // 保存监督器借用指针和主题声明快照，不复制 Entry 资源。
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

    // 返回注册时冻结的发布/订阅权限。
    QStringList publishedTopics() const override { return publishedTopics_; }
    QStringList subscribedTopics() const override { return subscribedTopics_; }

    // MessageBus 回调线程只进入有界队列，不携带完整 QByteArray 创建 queued event。
    void onMessage(const QString& topic,
                   const QString& senderModuleId,
                   const QByteArray& data) override
    {
        // MessageBus 只把消息交给桥接器；桥接器不在这里直接写 Socket。
        // 真正的 Socket 写入统一由 ProcessSupervisor 所在线程排空有界队列，
        // 因此慢子进程不会让 MessageBus 的回调线程同步等待网络或磁盘。
        supervisor_->enqueueMessageToChild(moduleId_, topic, senderModuleId, data);
    }

private:
    ProcessSupervisor* supervisor_;
    QString moduleId_;
    QStringList publishedTopics_;
    QStringList subscribedTopics_;
};

struct ProcessSupervisor::Entry
{
    // Entry 是一次模块监督生命周期的状态快照；重启时复用记录但重建运行时资源。
    ModuleConfig config;
    // 对外可查询的生命周期状态和最近故障文本。
    QString state;
    QString lastError;
    // 每次启动重新生成的本地服务器名与一次性认证令牌。
    QString serverName;
    QString token;
    // 本轮运行时对象均由监督器创建并在 destroyRuntime 中释放。
    QLocalServer* server = nullptr;
    QLocalSocket* socket = nullptr;
    QProcess* process = nullptr;
    ProcessBridge* bridge = nullptr;
    // 保存 Socket 半帧；解析完成的字节会由 takeFrame 移除。
    QByteArray inputBuffer;
    // outgoingQueue 只保存尚未写入 Socket 的消息。它按主题计数，
    // Latest 满时只会删除同主题最旧的等待项。
    QMutex outgoingMutex;
    QWaitCondition outgoingChanged;
    QQueue<OutgoingMessage> outgoingQueue;
    // 已经写入 Socket、但还没有收到子进程 deliveryAck 的数量。
    // 在途数量也受每个主题的 QueueCapacity 限制，防止 Socket 缓冲无限堆积。
    QHash<QString, int> outgoingInFlightByTopic;
    // messageId -> topic。ACK 到达时靠它找到要减少的主题计数。
    QHash<QString, QString> outgoingMessageTopics;
    // 大消息对应的共享内存句柄。只有 ACK 或故障清理后才能 detach/delete。
    QHash<QString, QSharedMemory*> outgoingShared;
    // Latest 覆盖和 Reliable 拒绝的累计计数，仅用于诊断/测试。
    quint64 outgoingDropped = 0;
    quint64 outgoingRejected = 0;
    qint64 lastOutgoingWarningMs = 0;
    int outgoingInFlightCount = 0;
    // true 表示已经投递了一个合并后的 drainChildQueue 唤醒事件。
    // 后续入队只唤醒条件变量，不再为每一帧创建 Qt queued event。
    bool outgoingWakeScheduled = false;
    bool outgoingAccepting = false;
    bool outgoingStopping = false;
    // 下列时间均为统一毫秒时基，用于注册、心跳和延迟重启判断。
    qint64 registrationDeadlineMs = 0;
    qint64 lastPongMs = 0;
    qint64 lastPingMs = 0;
    qint64 restartAtMs = 0;
    qint64 restartWindowStartMs = 0;
    int restartCount = 0;
    // 生命周期布尔值把协议阶段与对外 state 文本分开，便于快速分支判断。
    bool registered = false;
    bool running = false;
    bool stopping = false;
    bool faulted = false;
    bool stopAcknowledged = false;
};

// 构造监督器并启动轻量定时器；真正的子进程直到 startAll 才创建。
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

// 析构兜底调用幂等 shutdown，再释放每个模块的 Entry 记录。
ProcessSupervisor::~ProcessSupervisor()
{
    shutdown();
    qDeleteAll(entries_);
    entries_.clear();
}

// 筛选进程型配置、为每个模块建立 Entry，并逐个等待注册/启动结果。
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

// 正常停止先关闭消息入口，再发送 stop 并在有限时间内等待 stopAck/退出。
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

// 手动重启复用正常 stop，清空自动重启计数后重新创建全部运行时资源。
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

// 无尺寸重载沿用协议默认值，便于只请求“显示”而不调整客户区。
bool ProcessSupervisor::showWindow(const QString& moduleId, QString* errorMessage)
{
    return showWindow(moduleId, 0, 0, errorMessage);
}

// 向正在运行的 ProcessUi 发送显示命令和可选客户区尺寸。
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

// 故障注入/紧急路径直接终止 QProcess，让统一 finished/fault 逻辑接管。
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

// 关闭定时监督，并对全部 Entry 执行一次正常 stop；重复调用直接返回。
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

// 遍历 Entry 状态生成当前运行模块 ID 快照。
QStringList ProcessSupervisor::runningModuleIds() const
{
    QStringList result;
    for (const Entry* entry : entries_) {
        if (entry->running)
            result.append(entry->config.id);
    }
    return result;
}

// 未知模块返回空字符串，调用方可与 Running/Failed 等状态区分。
QString ProcessSupervisor::state(const QString& moduleId) const
{
    const Entry* entry = findEntry(moduleId);
    return entry == nullptr ? QString() : entry->state;
}

// 保存最新 QSS，并广播给所有已注册且连接正常的 UI 子进程。
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

// 以下查找函数只返回监督器拥有的借用指针，不转移 Entry 所有权。
ProcessSupervisor::Entry* ProcessSupervisor::findEntry(const QString& moduleId) const
{
    for (Entry* entry : entries_) {
        if (entry->config.id == moduleId)
            return entry;
    }
    return nullptr;
}

// 根据 Qt signal 的 sender() 反查对应本地服务器。
ProcessSupervisor::Entry* ProcessSupervisor::findEntryByServer(QObject* object) const
{
    for (Entry* entry : entries_) {
        if (entry->server == object)
            return entry;
    }
    return nullptr;
}

// 根据 readyRead/disconnected 的 sender() 反查子进程连接。
ProcessSupervisor::Entry* ProcessSupervisor::findEntryBySocket(QObject* object) const
{
    for (Entry* entry : entries_) {
        if (entry->socket == object)
            return entry;
    }
    return nullptr;
}

// 根据 QProcess 错误/退出信号反查模块配置。
ProcessSupervisor::Entry* ProcessSupervisor::findEntryByProcess(QObject* object) const
{
    for (Entry* entry : entries_) {
        if (entry->process == object)
            return entry;
    }
    return nullptr;
}

// 为一个 Entry 创建随机服务器名/令牌、QLocalServer 和 QProcess，再等待 Running。
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

// 在有限预算内泵送 Qt 事件，等待注册帧和 started；不会使用阻塞队列连接。
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

// 编码并写入已连接 Socket；这里只判断写入是否被 Qt 接受。
bool ProcessSupervisor::sendFrame(Entry* entry, const QJsonObject& frame)
{
    if (entry == nullptr || entry->socket == nullptr ||
        entry->socket->state() != QLocalSocket::ConnectedState)
        return false;
    return entry->socket->write(process::encodeFrame(frame)) >= 0;
}

// 接受第一个合法待连接 Socket；同一 Entry 的额外连接立即拒绝。
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

// 累积并循环解析完整 IPC 帧，协议错误统一转入故障处理。
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

// 非停止期间的连接断开属于模块故障，可能触发自动重启。
void ProcessSupervisor::onSocketDisconnected()
{
    Entry* entry = findEntryBySocket(sender());
    if (entry == nullptr || entry->stopping || entry->faulted || shuttingDown_)
        return;
    handleFault(entry, QString::fromUtf8(u8"子进程 IPC 连接断开"));
}

// 只把无法启动和崩溃升级为监督器故障，其他状态由 finished/Socket 路径处理。
void ProcessSupervisor::onProcessError(QProcess::ProcessError error)
{
    Entry* entry = findEntryByProcess(sender());
    if (entry == nullptr || entry->stopping || entry->faulted || shuttingDown_)
        return;
    if (error == QProcess::FailedToStart || error == QProcess::Crashed)
        handleFault(entry, QString::fromUtf8(u8"子进程启动或运行失败"));
}

// 非预期退出转换为可读错误，并进入统一清理/重启策略。
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

// 分派注册、生命周期、心跳、窗口、日志、ACK 和子到父业务消息帧。
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
        {
            QMutexLocker locker(&entry->outgoingMutex);
            entry->outgoingAccepting = true;
            entry->outgoingStopping = false;
            entry->outgoingChanged.wakeAll();
        }
        // 注册确认不仅表示“可以启动”，还把父进程最终采用的队列规则
        // 下发给子进程。父、子两侧因此使用同一份 QueueCapacity、大小上限
        // 和 Latest/Reliable 策略，而不是各自猜一个默认值。
        QJsonObject accepted;
        accepted.insert(QStringLiteral("type"), kRegisterAck);
        accepted.insert(QStringLiteral("accepted"), true);
        accepted.insert(QStringLiteral("defaultQueueCapacity"),
                        qMax(1, messageBusConfig_.defaultQueueCapacity));
        accepted.insert(QStringLiteral("defaultMaxMessageBytes"),
                        qMax(1, messageBusConfig_.maxMessageBytes));
        accepted.insert(QStringLiteral("defaultPolicy"),
                        queuePolicyName(messageBusConfig_.defaultPolicy));
        QJsonArray topicConfigs;
        QSet<QString> seenTopics;
        appendTopicConfig(messageBusConfig_, publishedTopics, &topicConfigs, &seenTopics);
        appendTopicConfig(messageBusConfig_, subscribedTopics, &topicConfigs, &seenTopics);
        accepted.insert(QStringLiteral("topicConfigs"), topicConfigs);
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
    if (type == kDeliveryAck) {
        // deliveryAck 的含义是：消息已经被子进程放进自己的有界输入队列，
        // 并不要求慢消费者的 onMessage() 已经执行完。收到它后，父进程
        // 才能安全地减少在途计数，并释放这条消息占用的共享内存槽位。
        acknowledgeChildMessage(entry,
                                frame.value(QStringLiteral("messageId")).toString(),
                                frame.value(QStringLiteral("accepted")).toBool());
        return;
    }
    if (type == kSharedAck) {
        const QString messageId = frame.value(QStringLiteral("messageId")).toString();
        // 兼容旧子进程：旧版本只为共享内存发送 sharedAck。
        acknowledgeChildMessage(entry, messageId, true);
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

    // 子进程 publish() 的反方向路径：先解析控制帧，再由父进程的
    // MessageBus 做权限、大小和订阅者队列判断。父进程不会把整帧 QByteArray
    // 再排成一个无界 Qt 事件；Socket 帧本身受 maxFrameBytes 限制，处理完后
    // 立即用 publishAck 告诉子进程是否接收。
    const QString topic = frame.value(QStringLiteral("topic")).toString();
    const QString messageId = frame.value(QStringLiteral("messageId")).toString();
    const QString transport = frame.value(QStringLiteral("transport")).toString();
    QByteArray data;
    bool valid = !topic.isEmpty() && !messageId.isEmpty();
    if (transport == QStringLiteral("shared")) {
        const QString key = frame.value(QStringLiteral("sharedKey")).toString();
        QSharedMemory shared(key);
        if (shared.attach(QSharedMemory::ReadOnly)) {
            if (shared.lock()) {
                const int declaredSize = frame.value(QStringLiteral("size")).toInt();
                const int copySize = qMin(shared.size(), qMax(0, declaredSize));
                if (declaredSize > 0 && copySize == declaredSize &&
                    copySize <= messageBusConfig_.maxMessageBytes) {
                    data = QByteArray(static_cast<const char*>(shared.constData()), copySize);
                } else {
                    valid = false;
                }
                shared.unlock();
            } else {
                valid = false;
            }
            shared.detach();
        } else {
            valid = false;
        }
    } else if (transport == QStringLiteral("inline")) {
        data = QByteArray::fromBase64(
            frame.value(QStringLiteral("data")).toString().toLatin1());
    } else {
        valid = false;
    }
    bool accepted = false;
    const TopicConfig config = effectiveTopicConfig(messageBusConfig_, topic);
    if (valid && data.size() <= config.maxMessageBytes)
        accepted = messageBus_->publishFromModule(entry->config.id, topic, data);

    // 这个 ACK 与父到子的 deliveryAck 是两件事：这里确认的是“父进程的
    // MessageBus 已接收/拒绝”，子进程据此释放自己的在途槽位和共享段。
    QJsonObject ack;
    ack.insert(QStringLiteral("type"), kPublishAck);
    ack.insert(QStringLiteral("messageId"), messageId);
    ack.insert(QStringLiteral("accepted"), accepted);
    sendFrame(entry, ack);
}

// 定时执行延迟重启、注册超时、ping 发送和 pong 超时检查。
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

// MessageBus 到子进程的非阻塞入口：验证后放入按主题有界等待队列。
bool ProcessSupervisor::enqueueMessageToChild(const QString& moduleId,
                                              const QString& topic,
                                              const QString& senderModuleId,
                                              const QByteArray& data)
{
    Entry* entry = findEntry(moduleId);
    if (entry == nullptr || topic.isEmpty())
        return false;

    const TopicConfig config = effectiveTopicConfig(messageBusConfig_, topic);
    if (data.size() > config.maxMessageBytes)
        return false;

    bool schedule = false;
    {
        QMutexLocker locker(&entry->outgoingMutex);
        // 这里是父进程到子进程的“第一道闸门”。停止、断线或重启会把
        // outgoingAccepting 置为 false，并唤醒等待者，所以生产者不会在
        // 生命周期结束后继续把消息塞进旧队列。
        if (!entry->outgoingAccepting || entry->outgoingStopping) {
            ++entry->outgoingRejected;
            return false;
        }

        int topicCount = queuedTopicCount(entry->outgoingQueue, topic);
        if (topicCount >= config.queueCapacity) {
            if (config.policy == QueuePolicy::Reliable) {
                // Reliable 的契约是“宁可拒绝，也不能覆盖”。注意这里的
                // topicCount 只统计等待队列；在途计数会在 drain 时另行限制。
                ++entry->outgoingRejected;
                return false;
            }

            // Latest 只覆盖同主题仍在等待发送的最旧帧，绝不会触碰别的主题，
            // 也不会覆盖已经写入 Socket、正在等待 ACK 的帧。
            int oldestIndex = -1;
            for (int index = 0; index < entry->outgoingQueue.size(); ++index) {
                if (entry->outgoingQueue.at(index).topic == topic) {
                    oldestIndex = index;
                    break;
                }
            }
            if (oldestIndex < 0) {
                ++entry->outgoingRejected;
                return false;
            }
            entry->outgoingQueue.removeAt(oldestIndex);
            ++entry->outgoingDropped;
            --topicCount;
        }

        OutgoingMessage message;
        message.topic = topic;
        message.senderModuleId = senderModuleId;
        message.data = data;
        entry->outgoingQueue.enqueue(message);
        entry->outgoingChanged.wakeOne();
        if (!entry->outgoingWakeScheduled) {
            // 用一个布尔标记把很多并发入队合并为一次 Qt 唤醒事件。
            // 事件只携带 moduleId；真正的 QByteArray 留在受锁保护的 QQueue 中。
            entry->outgoingWakeScheduled = true;
            schedule = true;
        }
    }

    if (schedule) {
        QMetaObject::invokeMethod(this,
                                  "drainChildQueue",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, moduleId));
    }
    return true;
}

// 在监督器 Qt 线程中有限批次写 Socket，并建立等待 deliveryAck 的在途记录。
void ProcessSupervisor::drainChildQueue(const QString& moduleId)
{
    Entry* entry = findEntry(moduleId);
    if (entry == nullptr)
        return;

    // 每次最多处理一小批，避免监督器线程长时间被一个高吞吐模块占用。
    // 批次结束后若仍有可发送项，再安排下一次合并唤醒。
    const int batchLimit = 8;
    for (int sent = 0; sent < batchLimit; ++sent) {
        if (entry->socket == nullptr ||
            entry->socket->state() != QLocalSocket::ConnectedState) {
            QMutexLocker locker(&entry->outgoingMutex);
            entry->outgoingWakeScheduled = false;
            return;
        }

        OutgoingMessage message;
        bool haveMessage = false;
        {
            QMutexLocker locker(&entry->outgoingMutex);
            if (!entry->outgoingAccepting || entry->outgoingStopping)
                break;

            // 只挑选“该主题在途数量还没有达到容量”的项。这样即使 Socket
            // 写得很快，未收到 ACK 的消息数量也不会突破 QueueCapacity。
            int selectedIndex = -1;
            for (int index = 0; index < entry->outgoingQueue.size(); ++index) {
                const OutgoingMessage& candidate = entry->outgoingQueue.at(index);
                const TopicConfig config = effectiveTopicConfig(messageBusConfig_,
                                                                 candidate.topic);
                if (entry->outgoingInFlightByTopic.value(candidate.topic, 0) <
                    config.queueCapacity) {
                    selectedIndex = index;
                    break;
                }
            }
            if (selectedIndex >= 0) {
                message = entry->outgoingQueue.takeAt(selectedIndex);
                ++entry->outgoingInFlightByTopic[message.topic];
                ++entry->outgoingInFlightCount;
                haveMessage = true;
            }
        }
        if (!haveMessage)
            break;

        // 等待队列中的项故意没有 messageId；现在它已经占用一个在途槽位，
        // 才生成唯一 ID，并建立 ID->主题映射供 deliveryAck 回收计数。
        const QString messageId = QUuid::createUuid().toString(QUuid::Id128);
        {
            QMutexLocker locker(&entry->outgoingMutex);
            entry->outgoingMessageTopics.insert(messageId, message.topic);
        }

        QJsonObject frame;
        frame.insert(QStringLiteral("type"), kMessage);
        frame.insert(QStringLiteral("topic"), message.topic);
        frame.insert(QStringLiteral("senderModuleId"), message.senderModuleId);
        frame.insert(QStringLiteral("messageId"), messageId);
        if (message.data.size() >= messageBusConfig_.sharedMemoryThresholdBytes) {
            // 大消息只在共享内存中复制一次；Socket 里只放 key 和 size 等控制信息。
            // 共享段的所有权仍在父进程，必须一直保留到子进程 ACK。
            const QString key = makeSharedKey(moduleId);
            QSharedMemory* shared = new QSharedMemory(key, this);
            if (!shared->create(message.data.size())) {
                delete shared;
                acknowledgeChildMessage(entry, messageId, false);
                continue;
            }
            if (!shared->lock()) {
                shared->detach();
                delete shared;
                acknowledgeChildMessage(entry, messageId, false);
                continue;
            }
            std::memcpy(shared->data(),
                        message.data.constData(),
                        static_cast<size_t>(message.data.size()));
            shared->unlock();
            {
                QMutexLocker locker(&entry->outgoingMutex);
                entry->outgoingShared.insert(messageId, shared);
            }
            frame.insert(QStringLiteral("transport"), QStringLiteral("shared"));
            frame.insert(QStringLiteral("sharedKey"), key);
            frame.insert(QStringLiteral("size"), message.data.size());
        } else {
            // 小消息直接放进 JSON 控制帧。它仍然受单条大小上限约束，且不会
            // 作为 QByteArray 参数附着在每个 Qt queued event 上。
            frame.insert(QStringLiteral("transport"), QStringLiteral("inline"));
            frame.insert(QStringLiteral("data"),
                         QString::fromLatin1(message.data.toBase64()));
        }

        if (!sendFrame(entry, frame)) {
            acknowledgeChildMessage(entry, messageId, false);
            handleFault(entry, QString::fromUtf8(u8"无法向子进程发送消息"));
            return;
        }
    }

    bool schedule = false;
    {
        QMutexLocker locker(&entry->outgoingMutex);
        bool eligible = false;
        if (entry->outgoingAccepting && !entry->outgoingStopping &&
            entry->socket != nullptr &&
            entry->socket->state() == QLocalSocket::ConnectedState) {
            for (const OutgoingMessage& candidate : entry->outgoingQueue) {
                const TopicConfig config = effectiveTopicConfig(messageBusConfig_,
                                                                 candidate.topic);
                if (entry->outgoingInFlightByTopic.value(candidate.topic, 0) <
                    config.queueCapacity) {
                    eligible = true;
                    break;
                }
            }
        }
        if (eligible) {
            // 保留“已安排”标记，直到下一次 drain 取到消息，避免并发入队
            // 为同一批消息创建大量 Qt 事件。
            schedule = true;
        } else {
            entry->outgoingWakeScheduled = false;
        }
    }
    if (schedule)
        QMetaObject::invokeMethod(this,
                                  "drainChildQueue",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, moduleId));
}

// 根据 deliveryAck 回收父到子的在途槽位、主题计数和共享段。
void ProcessSupervisor::acknowledgeChildMessage(Entry* entry,
                                                const QString& messageId,
                                                bool accepted)
{
    if (entry == nullptr || messageId.isEmpty())
        return;

    QSharedMemory* shared = nullptr;
    QString topic;
    bool schedule = false;
    bool logRejected = false;
    quint64 rejectedCount = 0;
    {
        QMutexLocker locker(&entry->outgoingMutex);
        // take() 同时完成“验证 ACK 属于当前在途消息”和“删除待回收记录”。
        // 重复 ACK 或迟到的旧 ACK 找不到 topic 时直接忽略，避免重复减少计数。
        topic = entry->outgoingMessageTopics.take(messageId);
        if (topic.isEmpty())
            return;
        if (!accepted) {
            ++entry->outgoingRejected;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - entry->lastOutgoingWarningMs >= 1000) {
                entry->lastOutgoingWarningMs = now;
                logRejected = true;
                rejectedCount = entry->outgoingRejected;
            }
        }
        const int count = entry->outgoingInFlightByTopic.value(topic, 0);
        if (count <= 1)
            entry->outgoingInFlightByTopic.remove(topic);
        else
            entry->outgoingInFlightByTopic.insert(topic, count - 1);
        entry->outgoingInFlightCount = qMax(0, entry->outgoingInFlightCount - 1);
        // 无论 ACK 表示接收还是拒绝，都必须释放一个在途槽位；只有 accepted
        // 影响统计和限频日志，不影响槽位回收。
        shared = entry->outgoingShared.take(messageId);
        entry->outgoingChanged.wakeAll();

        if (!entry->outgoingWakeScheduled && entry->outgoingAccepting &&
            !entry->outgoingStopping && entry->socket != nullptr &&
            entry->socket->state() == QLocalSocket::ConnectedState) {
            for (const OutgoingMessage& candidate : entry->outgoingQueue) {
                const TopicConfig config = effectiveTopicConfig(messageBusConfig_,
                                                                 candidate.topic);
                if (entry->outgoingInFlightByTopic.value(candidate.topic, 0) <
                    config.queueCapacity) {
                    entry->outgoingWakeScheduled = true;
                    schedule = true;
                    break;
                }
            }
        }
    }
    if (shared != nullptr) {
        // 子进程已经完成读取（或明确拒绝）后，共享段才可以 detach。
        // 在此之前删除会让子进程读到失效地址或直接丢帧。
        shared->detach();
        delete shared;
    }
    if (logRejected) {
        Logger::instance().log(
            LogLevel::Warning,
            entry->config.id,
            QString::fromUtf8(u8"子进程拒绝消息，累计 %1 条")
                .arg(rejectedCount));
    }
    if (schedule)
        QMetaObject::invokeMethod(this,
                                  "drainChildQueue",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, entry->config.id));
}

// 防重复地标记失败、销毁旧运行时，并按时间窗口决定是否自动重启。
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

// 统一释放桥接器、Socket、Server、QProcess、队列状态和共享内存。
void ProcessSupervisor::destroyRuntime(Entry* entry)
{
    if (entry == nullptr)
        return;
    {
        QMutexLocker locker(&entry->outgoingMutex);
        // 故障、断开、重启和正常停止共用这一条清理路径。先禁止生产者，
        // 再清空等待/在途索引并 wakeAll，保证任何将来的等待都有机会醒来。
        entry->outgoingAccepting = false;
        entry->outgoingStopping = true;
        entry->outgoingQueue.clear();
        entry->outgoingInFlightByTopic.clear();
        entry->outgoingMessageTopics.clear();
        entry->outgoingInFlightCount = 0;
        entry->outgoingWakeScheduled = false;
        entry->outgoingChanged.wakeAll();
    }
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

// 收集锁内共享段指针，锁外 detach/delete，缩短队列互斥锁持有时间。
void ProcessSupervisor::clearOutgoingShared(Entry* entry)
{
    QList<QSharedMemory*> segments;
    {
        QMutexLocker locker(&entry->outgoingMutex);
        segments = entry->outgoingShared.values();
        entry->outgoingShared.clear();
        entry->outgoingChanged.wakeAll();
    }
    for (QSharedMemory* shared : segments) {
        // 这里处理的是 ACK 尚未到达就发生故障/停止的剩余段；它们已经不再
        // 有合法消费者，因此由父进程统一 detach，防止共享内存泄漏。
        shared->detach();
        delete shared;
    }
}

// 更新 Entry 快照后发信号，确保 UI 查询 state() 时能看到同一状态。
void ProcessSupervisor::emitState(Entry* entry,
                                  const QString& stateValue,
                                  const QString& detail)
{
    entry->state = stateValue;
    emit moduleStateChanged(entry->config.id, stateValue, detail);
}
}
