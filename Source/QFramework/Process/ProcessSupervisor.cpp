// 文件职责：监督 ProcessUi/ProcessNonUi 子进程及其本地 IPC。
// 本类拥有 QProcess、QLocalServer、QLocalSocket 和共享内存对象，全部在监督器线程创建/使用/销毁；
// 注册、认证、心跳、Latest/Reliable 队列、窗口嵌入和停止状态都由同一状态机推进。
// 每个请求有独立 deadline，整体 shutdown 还有总预算；子进程不响应时报告 fault/timeout 并自然回收，
// 不在 GUI 线程同步等待，也不调用强制终止线程的方式破坏 Qt 对象生命周期。
#include "ProcessSupervisor.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
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
const QString kRegister = QStringLiteral("register"); ///< 子进程上线注册帧。
const QString kRegisterAck = QStringLiteral("registerAck"); ///< 父进程确认注册并下发主题配置的帧。
const QString kStarted = QStringLiteral("started"); ///< 子进程模块启动成功上报帧。
const QString kStartFailed = QStringLiteral("startFailed"); ///< 子进程模块启动失败上报帧。
const QString kMessage = QStringLiteral("message"); ///< 父子双向业务消息帧。
const QString kLog = QStringLiteral("log"); ///< 子进程日志上报帧。
const QString kPing = QStringLiteral("ping"); ///< 父进程心跳探测帧。
const QString kPong = QStringLiteral("pong"); ///< 子进程心跳应答帧。
const QString kStop = QStringLiteral("stop"); ///< 父进程要求子进程停止帧。
const QString kStopAck = QStringLiteral("stopAck"); ///< 子进程确认停止帧。
const QString kWindowReady = QStringLiteral("windowReady"); ///< 子进程上报原生窗口句柄帧。
const QString kShowWindow = QStringLiteral("showWindow"); ///< 父进程要求显示子窗口帧。
const QString kResizeWindow = QStringLiteral("resizeWindow"); ///< 父进程要求调整子窗口尺寸帧。
const QString kWindowWidth = QStringLiteral("windowWidth"); ///< 尺寸帧中的宽度字段名。
const QString kWindowHeight = QStringLiteral("windowHeight"); ///< 尺寸帧中的高度字段名。
const QString kSharedAck = QStringLiteral("sharedAck"); ///< 父进程确认收到共享内存消息帧。
const QString kDeliveryAck = QStringLiteral("deliveryAck"); ///< 子进程确认父到子消息已入队帧。
const QString kPublishAck = QStringLiteral("publishAck"); ///< 父进程确认子到父消息已接受帧。
const QString kDebugWaitTimeout = QStringLiteral("debugWaitTimeout"); ///< 调试等待超时提示帧。
const QString kStyleSheet = QStringLiteral("styleSheet"); ///< 父进程下发全局 QSS 帧。

/// @brief 把模块类型枚举转换为注册帧使用的稳定英文文本。
/// @param type 待转换的模块类型。
/// @return ProcessUi 或 ProcessNonUi；其他类型返回空字符串。
QString processTypeName(ModuleType type)
{
    // 配置枚举转换成注册帧中的稳定文本，只允许两种子进程类型。
    if (type == ModuleType::ProcessUi)
        return QStringLiteral("ProcessUi");
    if (type == ModuleType::ProcessNonUi)
        return QStringLiteral("ProcessNonUi");
    return QString();
}

/// @brief 判断模块类型是否由本监督器管理。
/// @param type 待判断的模块类型。
/// @return 属于两种子进程类型返回 true，否则返回 false。
bool isProcessType(ModuleType type)
{
    // startAll 用它跳过由 PluginManager 负责的两个主进程类型。
    return type == ModuleType::ProcessUi || type == ModuleType::ProcessNonUi;
}

/// @brief 生成本次启动专用的本地服务器名。
/// @param moduleId 模块标识。
/// @return 由模块 ID、父进程 PID 和随机 UUID 组成的服务器名。
QString makeServerName(const QString& moduleId)
{
    // 模块 ID + 父进程 PID + UUID 组合，避免重启或并行部署时名称冲突。
    return QStringLiteral("QFramework_%1_%2_%3")
        .arg(moduleId,
             QString::number(QCoreApplication::applicationPid()),
             QUuid::createUuid().toString(QUuid::Id128));
}

/// @brief 为一条大消息生成独立的共享内存键。
/// @param moduleId 模块标识。
/// @return 带模块前缀和随机 UUID 的共享内存键。
QString makeSharedKey(const QString& moduleId)
{
    // 每条大消息使用独立 UUID，ACK 后立即释放，不复用旧共享段。
    return QStringLiteral("QFrameworkShared_%1_%2")
        .arg(moduleId, QUuid::createUuid().toString(QUuid::Id128));
}

/// @brief 计算允许的最大帧字节数，为 base64 膨胀留出余量并设置硬上限。
/// @param maxMessageBytes 单条消息的字节上限。
/// @return 最终生效的帧字节上限。
int maxFrameBytes(int maxMessageBytes)
{
    // inline payload 经 base64/JSON 会膨胀，因此取消息上限两倍并设置硬边界。
    const qint64 value = qMax<qint64>(64 * 1024,
                                      static_cast<qint64>(maxMessageBytes) * 2); ///< 未裁剪前的帧上限。
    return static_cast<int>(qMin<qint64>(value, 128 * 1024 * 1024));
}

/// @brief 把 JSON 值解析为纯字符串数组。
/// @param value 待解析的 JSON 值。
/// @param result 输出参数，解析成功时写入字符串列表。
/// @return 解析成功返回 true；结果指针为空、不是数组或含非字符串元素返回 false。
bool jsonStringList(const QJsonValue& value, QStringList* result)
{
    // 注册主题必须是纯字符串数组，任一元素类型错误就拒绝整次注册。
    if (result == nullptr || !value.isArray())
        return false;
    result->clear();
    const QJsonArray array = value.toArray(); ///< 待遍历的 JSON 数组。
    for (const QJsonValue& item : array) { ///< 当前检查的数组元素。
        if (!item.isString())
            return false;
        result->append(item.toString());
    }
    return true;
}

/// @brief 拼接带前缀的用户可读错误文本。
/// @param prefix 错误前缀。
/// @param detail 具体原因，可为空。
/// @return 拼接结果；detail 为空时只返回前缀。
QString detailWithPrefix(const QString& prefix, const QString& detail)
{
    // 统一拼接用户可读错误，空 detail 时不多加冒号。
    if (detail.isEmpty())
        return prefix;
    return prefix + QStringLiteral(": ") + detail;
}

/// @brief 计算配置中允许的最大消息字节数，覆盖全局默认和所有主题专用值。
/// @param config 消息总线配置。
/// @return 配置允许的最大消息字节数，至少为 1。
int maxConfiguredMessageBytes(const MessageBusConfig& config)
{
    // 命令行绝对上限和 Socket 帧解析上限必须覆盖所有主题专用值。
    // 主题规则仍会在实际入队前做第二次校验，不能用较小的全局默认值提前
    // 截断视频主题，也不能因为绝对上限较大而放宽普通主题。
    int result = qMax(1, config.maxMessageBytes); ///< 当前见到的最大消息上限。
    for (const TopicConfig& topic : config.topics) ///< 当前参与比较的主题配置。
        result = qMax(result, qMax(1, topic.maxMessageBytes));
    return result;
}

struct OutgoingMessage
{
    // 这是“父进程准备发给某个子进程”的待发送项。
    // 它先放在内存队列里，只有真正取得一个在途槽位时才会生成
    // messageId。这样被 Latest 丢弃的等待项不会留下永远等不到 ACK 的记录。
    QString topic; ///< 目标主题，用于按主题限流。
    QString senderModuleId; ///< 原始发送方模块标识，随消息一起转发。
    // 父进程等待队列持有不可变共享载荷；真正写入跨进程介质后释放引用。
    MessagePayload payload; ///< 待发送的不可变消息载荷。
};

/// @brief 取主题最终生效的队列配置，未单独配置时回退全局默认值。
/// @param config 消息总线配置。
/// @param topic 主题名。
/// @return 该主题的容量、大小上限和策略，容量与大小均至少为 1。
TopicConfig effectiveTopicConfig(const MessageBusConfig& config,
                                 const QString& topic)
{
    // 主题没有单独配置时，沿用 MessageBus 的默认容量、大小上限和策略。
    // qMax(1, ...) 是最后一道保护，避免配置错误把队列容量变成 0，
    // 进而让“永远无法发送”的状态传播到跨进程队列。
    TopicConfig result = config.topics.value(topic); ///< 主题规则，未配置时用默认值填充。
    if (!config.topics.contains(topic)) {
        result.queueCapacity = config.defaultQueueCapacity;
        result.maxMessageBytes = config.maxMessageBytes;
        result.policy = config.defaultPolicy;
    }
    result.queueCapacity = qMax(1, result.queueCapacity);
    result.maxMessageBytes = qMax(1, result.maxMessageBytes);
    return result;
}

/// @brief 把队列策略枚举转换为协议中使用的稳定英文名。
/// @param policy 队列策略。
/// @return Latest 或 Reliable。
QString queuePolicyName(QueuePolicy policy)
{
    // registerAck 使用稳定英文值，子进程按大小写不敏感方式解析。
    return policy == QueuePolicy::Latest
        ? QStringLiteral("Latest")
        : QStringLiteral("Reliable");
}

/// @brief 统计发送队列中指定主题的待发送条数。
/// @param queue 父到子的待发送队列。
/// @param topic 要统计的主题名。
/// @return 该主题在队列中的条数。
int queuedTopicCount(const QQueue<OutgoingMessage>& queue,
                     const QString& topic)
{
    // 容量按主题计算，而不是把不同主题互相挤占同一个数字。
    int count = 0; ///< 命中该主题的条数。
    for (const OutgoingMessage& message : queue) { ///< 当前检查的等待项。
        if (message.topic == topic)
            ++count;
    }
    return count;
}

/// @brief 把单个主题的队列规则追加到 registerAck 的主题数组中。
/// @param config 消息总线配置，用于取该主题的最终规则。
/// @param topic 主题名。
/// @param array 输出参数，追加到的主题配置数组。
/// @param seen 已下发主题集合，用于跨发布/订阅去重。
/// @return 无。
void appendTopicConfig(const MessageBusConfig& config,
                       const QString& topic,
                       QJsonArray* array,
                       QSet<QString>* seen)
{
    // seen 防止一个主题同时出现在 published/subscribed 时被下发两次。
    if (array == nullptr || seen == nullptr || topic.isEmpty() || seen->contains(topic))
        return;
    seen->insert(topic);
    const TopicConfig value = effectiveTopicConfig(config, topic); ///< 该主题最终生效的队列规则。
    QJsonObject item; ///< 单条主题配置的 JSON 对象。
    item.insert(QStringLiteral("topic"), topic);
    item.insert(QStringLiteral("queueCapacity"), value.queueCapacity);
    item.insert(QStringLiteral("maxMessageBytes"), value.maxMessageBytes);
    item.insert(QStringLiteral("policy"), queuePolicyName(value.policy));
    array->append(item);
}

/// @brief 把一组主题的队列规则批量追加到 registerAck 的主题数组中。
/// @param config 消息总线配置，用于取各主题的最终规则。
/// @param topics 待下发的主题列表。
/// @param array 输出参数，追加到的主题配置数组。
/// @param seen 已下发主题集合，用于去重。
/// @return 无。
void appendTopicConfig(const MessageBusConfig& config,
                       const QStringList& topics,
                       QJsonArray* array,
                       QSet<QString>* seen)
{
    // 列表重载逐项复用单主题校验和去重逻辑。
    for (const QString& topic : topics) ///< 当前待下发的主题。
        appendTopicConfig(config, topic, array, seen);
}
}

// MessageBus 注册的“虚拟订阅模块”。它把子进程订阅主题接入总线，收到消息后
// 只做线程安全入队；真正写 Socket 必须回到监督器线程，避免跨线程操作 QLocalSocket。
class ProcessBridge final : public ModuleEndpoint
{
public:
    // 保存监督器借用指针和主题声明快照，不复制 Entry 资源。
    /// @brief 构造子进程桥接器，保存监督器借用指针和主题声明快照。
    /// @param supervisor 所属监督器，生命周期必须长于本对象。
    /// @param moduleId 该子进程的模块标识。
    /// @param publishedTopics 子进程声明的发布主题。
    /// @param subscribedTopics 子进程声明的订阅主题。
    /// @return 无（构造函数不返回值）。
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
    /// @brief 返回注册时冻结的发布主题列表。
    /// @return 发布主题列表副本。
    QStringList publishedTopics() const override { return publishedTopics_; }
    /// @brief 返回注册时冻结的订阅主题列表。
    /// @return 订阅主题列表副本。
    QStringList subscribedTopics() const override { return subscribedTopics_; }

    // MessageBus 回调线程只进入有界队列，不携带完整 QByteArray 创建 queued event。
    /// @brief 接收 MessageBus 投递的消息，转入子进程的有界发送队列。
    /// @param topic 消息主题。
    /// @param senderModuleId 原始发送方模块标识。
    /// @param data 消息原始字节。
    /// @return 无。
    void onMessage(const QString& topic,
                   const QString& senderModuleId,
                   const QByteArray& data) override
    {
        // MessageBus 只把消息交给桥接器；桥接器不在这里直接写 Socket。
        // 真正的 Socket 写入统一由 ProcessSupervisor 所在线程排空有界队列，
        // 因此慢子进程不会让 MessageBus 的回调线程同步等待网络或磁盘。
        // onMessage 公共签名保持 QByteArray；这里创建的新对象继续隐式共享
        // 原始字节缓冲，直到 QSharedMemory/base64 边界才执行跨进程复制。
        supervisor_->enqueueMessageToChild(
            moduleId_, topic, senderModuleId, makeMessagePayload(data));
    }

private:
    ProcessSupervisor* supervisor_; ///< 所属监督器的借用指针，不参与所有权。
    QString moduleId_; ///< 该桥接器对应的子进程模块标识。
    QStringList publishedTopics_; ///< 注册时冻结的发布主题列表。
    QStringList subscribedTopics_; ///< 注册时冻结的订阅主题列表。
};

struct ProcessSupervisor::Entry
{
    // Entry 是一次模块监督生命周期的状态快照；重启时复用记录但重建运行时资源。
    ModuleConfig config; ///< 该模块的静态配置快照。
    // 对外可查询的生命周期状态和最近故障文本。
    QString state; ///< 当前生命周期状态文本，如 Starting/Running/Stopped。
    QString lastError; ///< 最近一次故障或停止原因的文本。
    // 每次启动重新生成的本地服务器名与一次性认证令牌。
    QString serverName; ///< 本次启动使用的本地服务器名。
    QString token; ///< 本次启动使用的一次性认证令牌。
    // 本轮运行时对象均由监督器创建并在 destroyRuntime 中释放。
    QLocalServer* server = nullptr; ///< 监听子进程注册连接的本地服务器。
    QLocalSocket* socket = nullptr; ///< 与子进程通信的本地套接字。
    QProcess* process = nullptr; ///< 子进程句柄。
    ProcessBridge* bridge = nullptr; ///< 接入 MessageBus 的虚拟订阅模块。
    // 保存 Socket 半帧；解析完成的字节会由 takeFrame 移除。
    QByteArray inputBuffer; ///< 从子进程累计接收的未解析字节。
    // outgoingQueue 只保存尚未写入 Socket 的消息。它按主题计数，
    // Latest 满时只会删除同主题最旧的等待项。
    QMutex outgoingMutex; ///< 保护发送队列、在途表和相关状态标志。
    QQueue<OutgoingMessage> outgoingQueue; ///< 尚未写入 Socket 的待发送消息。
    // 已经写入 Socket、但还没有收到子进程 deliveryAck 的数量。
    // 在途数量也受每个主题的 QueueCapacity 限制，防止 Socket 缓冲无限堆积。
    QHash<QString, int> outgoingInFlightByTopic; ///< 每主题已发送未确认的条数。
    // messageId -> topic。ACK 到达时靠它找到要减少的主题计数。
    QHash<QString, QString> outgoingMessageTopics; ///< messageId 到主题的反向索引。
    // 大消息对应的共享内存句柄。只有 ACK 或故障清理后才能 detach/delete。
    QHash<QString, QSharedMemory*> outgoingShared; ///< messageId 到共享内存段的映射。
    // Latest 覆盖和 Reliable 拒绝的累计计数，仅用于诊断/测试。
    quint64 outgoingDropped = 0; ///< 因 Latest 覆盖而丢弃的等待项累计数。
    quint64 outgoingRejected = 0; ///< 因超限或停止被本地拒绝的累计数。
    quint64 outgoingAbandoned = 0; ///< 断线时未确认就被放弃的累计数。
    qint64 lastOutgoingWarningMs = 0; ///< 上一次发送队列告警的时间戳，用于限流。
    int outgoingInFlightCount = 0; ///< 全部主题的在途消息总数，仅用于诊断。
    // true 表示已经投递了一个合并后的 drainChildQueue 唤醒事件。
    // 后续入队只唤醒条件变量，不再为每一帧创建 Qt queued event。
    bool outgoingWakeScheduled = false; ///< 是否已有待处理的排空唤醒事件。
    bool outgoingAccepting = false; ///< 是否仍接受新的待发送消息。
    bool outgoingStopping = false; ///< 是否已进入停止流程，停止后不再排空。
    // 每个 Entry 自己持有 deadline；A 的停止预算不会被 B 的状态转换重置。
    QDeadlineTimer startupDeadline; ///< 本次启动的等待截止时间。
    QDeadlineTimer stopDeadline; ///< 本次停止流程的整体截止时间。
    qint64 lastPongMs = 0; ///< 最近一次收到心跳应答的时间戳。
    qint64 lastPingMs = 0; ///< 最近一次发送心跳探测的时间戳。
    qint64 restartAtMs = 0; ///< 计划执行下一次自动重启的时间戳。
    qint64 restartWindowStartMs = 0; ///< 自动重启计数窗口的起始时间戳。
    int restartCount = 0; ///< 当前重启窗口内已自动重启的次数。
    LifecyclePhase phase = LifecyclePhase::Stopped; ///< 当前生命周期阶段。
    StopPurpose stopPurpose = StopPurpose::None; ///< 本次停止的目的，决定停止完成后走哪条分支。
    quint64 generation = 0; ///< 运行时代次，用于丢弃上一代进程的迟到信号。
    quint64 outgoingSummaryGeneration = 0; ///< 已上报过队列汇总的代次，避免重复上报。
    int stopEscalation = 0; ///< 停止升级级别，用于区分正常等待与强制结束。
    bool pendingStart = false; ///< 停止完成后是否应立即启动新进程。
    bool operationBusy = false; ///< 是否有停止或重启操作正在进行。
    bool prepareExitSupported = false;
    QString prepareId;
    bool exitReady = false;
    StopPurpose preparePurpose = StopPurpose::None;
    QDeadlineTimer prepareDeadline;
    bool stopAcknowledged = false; ///< 是否已收到子进程的 stopAck。
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
      shuttingDown_(false),
      startupBatchActive_(false),
      startupBatchSignalScheduled_(false)
{
    const int interval = qMax(50, qMin(processConfig_.heartbeatIntervalMs, 250)); ///< 状态机心跳间隔，限制在 50 到 250 毫秒之间。
    supervisionTimer_->setInterval(interval);
    connect(supervisionTimer_, &QTimer::timeout,
            this, &ProcessSupervisor::onSupervisionTick);
    supervisionTimer_->start();
}

// 析构兜底调用幂等 shutdown，再释放每个模块的 Entry 记录。
/// @brief 析构监督器：先幂等停止全部子进程，再释放 Entry 记录。
/// @return 无（析构函数不返回值）。
ProcessSupervisor::~ProcessSupervisor()
{
    shutdown();
    qDeleteAll(entries_);
    entries_.clear();
}

// 筛选进程型配置并并行提交启动；最终结果由 startupBatchFinished 异步报告。
bool ProcessSupervisor::startAll(const QVector<ModuleConfig>& modules,
                                  QStringList* errors)
{
    // 启动批次只接受配置中启用的 ProcessUi/ProcessNonUi，逐个创建 Entry 并异步启动；
    // 返回值表示请求是否全部被接受，真正 Running/Failed 结果通过批次信号给出。
    // Supervisor 属于框架线程，QProcess、QLocalSocket 和子进程窗口控制帧都在此线程
    // 串行处理。心跳定时器只推进状态机，不在 timeout 槽中同步等待进程。
    if (errors != nullptr)
        errors->clear();
    if (startupBatchActive_) {
        if (errors != nullptr)
            errors->append(QString::fromUtf8(u8"已有一批子进程正在启动"));
        return false;
    }
    shuttingDown_ = false;
    if (!supervisionTimer_->isActive())
        supervisionTimer_->start();
    startupBatchActive_ = true;
    startupPendingModules_.clear();
    startupErrors_.clear();
    bool allAccepted = true; ///< 是否所有启动请求都被接受。
    for (const ModuleConfig& config : modules) { ///< 当前提交启动的模块配置。
        if (!config.enabled || !isProcessType(config.type))
            continue;
        if (findEntry(config.id) != nullptr) {
            allAccepted = false;
            const QString detail = QString::fromUtf8(u8"子进程模块 ID 重复：%1").arg(config.id); ///< 重复 ID 的错误说明。
            startupErrors_.append(detail);
            if (errors != nullptr)
                errors->append(detail);
            continue;
        }
        Entry* entry = new Entry; ///< 该模块的监督记录。
        entry->config = config;
        entries_.append(entry);
        startupPendingModules_.insert(config.id);
        QString error; ///< 承接该模块启动失败原因。
        if (!beginStartEntry(entry, &error)) {
            allAccepted = false;
            if (errors != nullptr)
                errors->append(error);
        }
    }
    scheduleStartupBatchFinished();
    return allAccepted;
}

// 兼容入口现在只提交异步停止请求。
bool ProcessSupervisor::stop(const QString& moduleId, QString* errorMessage)
{
    return requestStop(moduleId, errorMessage);
}

// 兼容入口现在只提交异步重启请求。
bool ProcessSupervisor::restart(const QString& moduleId, QString* errorMessage)
{
    return requestRestart(moduleId, errorMessage);
}

// 停止请求关闭入口并发送 stop 后立即返回，QProcess 信号继续推进状态。
bool ProcessSupervisor::requestStop(const QString& moduleId, QString* errorMessage)
{
    // stop 是非阻塞状态转换：先关闭父子消息入口，再发送 stop 控制帧；QProcess 的
    // ACK、退出、超时和故障由后续信号/心跳推进，不让 GUI 卡在某个子进程上。
    Entry* entry = findEntry(moduleId); ///< 目标模块的监督记录。
    if (entry == nullptr) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"未找到子进程模块：%1").arg(moduleId);
        return false;
    }
    if (preparingExit_ || entry->operationBusy || isStoppingPhase(entry->phase) || entry->phase == LifecyclePhase::RestartDelay) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"模块操作正在进行：%1").arg(moduleId);
        return false;
    }
    entry->pendingStart = false;
    entry->lastError.clear();
    setOperationBusy(entry, true);
    beginPrepareEntry(entry, StopPurpose::Stop);
    return true;
}

// 手动重启只启动状态转换；重复点击不会创建第二轮 generation。
bool ProcessSupervisor::requestRestart(const QString& moduleId, QString* errorMessage)
{
    // restart 复用同一个 Entry；运行中先完整 stop，退出后才按延迟创建新 QProcess，
    // 避免旧 Socket、窗口句柄和新一代运行时混在一起。
    Entry* entry = findEntry(moduleId); ///< 目标模块的监督记录。
    if (entry == nullptr) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"未找到子进程模块：%1").arg(moduleId);
        return false;
    }
    if (shuttingDown_ || preparingExit_ || entry->operationBusy ||
        (entry->phase != LifecyclePhase::Running &&
         entry->phase != LifecyclePhase::Failed &&
         entry->phase != LifecyclePhase::Stopped)) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"模块操作正在进行：%1").arg(moduleId);
        return false;
    }
    entry->restartCount = 0;
    entry->restartWindowStartMs = 0;
    entry->restartAtMs = 0;
    entry->lastError.clear();
    entry->pendingStart = true;
    setOperationBusy(entry, true);
    if (entry->process != nullptr &&
        entry->process->state() != QProcess::NotRunning) {
        beginPrepareEntry(entry, StopPurpose::ManualRestart);
    } else {
        destroyRuntime(entry);
        entry->stopPurpose = StopPurpose::ManualRestart;
        entry->phase = LifecyclePhase::RestartDelay;
        entry->restartAtMs = QDateTime::currentMSecsSinceEpoch();
        emitState(entry, QStringLiteral("Restarting"), QString::fromUtf8(u8"已排队手动重启"));
    }
    return true;
}

// 无尺寸重载只请求子进程第一次显示，不携带宿主尺寸。
bool ProcessSupervisor::showWindow(const QString& moduleId, QString* errorMessage)
{
    Entry* entry = findEntry(moduleId); ///< 目标模块的监督记录。
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
    if (entry->phase != LifecyclePhase::Running) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"子进程 UI 尚未就绪：%1").arg(moduleId);
        return false;
    }

    QJsonObject frame; ///< 窗口显示控制帧。
    frame.insert(QStringLiteral("type"), kShowWindow);
    if (sendFrame(entry, frame))
        return true;
    handleFault(entry, QString::fromUtf8(u8"子进程窗口显示控制帧发送失败"));
    if (errorMessage != nullptr)
        *errorMessage = QString::fromUtf8(u8"无法向子进程发送窗口显示请求：%1").arg(moduleId);
    return false;
}

// 带尺寸的兼容入口按两个有序控制帧发送；尺寸调整不会重复调用 QWidget::show()。
bool ProcessSupervisor::showWindow(const QString& moduleId,
                                    int width,
                                    int height,
                                    QString* errorMessage)
{
    if (!showWindow(moduleId, errorMessage))
        return false;
    if (width <= 0 || height <= 0)
        return true;
    return resizeWindow(moduleId, width, height, errorMessage);
}

// 向正在运行的 ProcessUi 发送独立尺寸控制帧；失败才进入子进程故障路径。
bool ProcessSupervisor::resizeWindow(const QString& moduleId,
                                      int width,
                                      int height,
                                      QString* errorMessage)
{
    Entry* entry = findEntry(moduleId); ///< 目标模块的监督记录。
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
    if (width <= 0 || height <= 0) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"子进程窗口尺寸必须为正：%1").arg(moduleId);
        return false;
    }
    if (entry->phase != LifecyclePhase::Running) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"子进程 UI 尚未就绪：%1").arg(moduleId);
        return false;
    }

    QJsonObject frame; ///< 窗口尺寸控制帧。
    frame.insert(QStringLiteral("type"), kResizeWindow);
    frame.insert(kWindowWidth, width);
    frame.insert(kWindowHeight, height);
    if (sendFrame(entry, frame))
        return true;
    handleFault(entry, QString::fromUtf8(u8"子进程窗口尺寸控制帧发送失败"));
    if (errorMessage != nullptr)
        *errorMessage = QString::fromUtf8(u8"无法向子进程发送窗口尺寸请求：%1").arg(moduleId);
    return false;
}

// 故障注入/紧急路径直接终止 QProcess，让统一 finished/fault 逻辑接管。
bool ProcessSupervisor::terminate(const QString& moduleId)
{
    Entry* entry = findEntry(moduleId); ///< 目标模块的监督记录。
    if (entry == nullptr || entry->process == nullptr ||
        entry->process->state() == QProcess::NotRunning)
        return false;
    entry->process->kill();
    return true;
}

// 先并行发送 stop，再用一个总 QDeadlineTimer 收尾，期间不泵送普通 GUI 事件。
void ProcessSupervisor::shutdown()
{
    // 全局关闭先并行向所有子进程发 stop，再用一份总 deadline 等 ACK/进程退出；
    // 预算耗尽后统一升级为 terminate/kill，仍不调用 QThread::terminate。
    if (shuttingDown_)
        return;
    shuttingDown_ = true;
    startupBatchActive_ = false;
    startupPendingModules_.clear();
    if (supervisionTimer_ != nullptr)
        supervisionTimer_->stop();

    // 先向全部子进程发 stop，避免逐个等待让后面的模块晚收到停止请求。
    for (Entry* entry : entries_) { ///< 当前发送停止请求的模块记录。
        entry->pendingStart = false;
        beginStopEntry(entry, StopPurpose::Shutdown);
    }

    // 所有进程共享同一个总预算。轮询只等待 QProcess 自身，不泵送 GUI 事件，
    // 因而 A 的退出等待不会执行 B 的用户重启槽。
    QDeadlineTimer totalDeadline(qMax(1, processConfig_.stopTimeoutMs) + 1250); ///< 整个关闭流程共享的总截止时间。
    const auto waitForAll = [this, &totalDeadline](int phaseBudgetMs) { ///< 在给定阶段预算内轮询等待所有进程退出。
        const qint64 available = totalDeadline.remainingTime(); ///< 总预算的剩余毫秒数。
        if (available <= 0 || phaseBudgetMs <= 0)
            return;
        QDeadlineTimer phaseDeadline(
            static_cast<int>(qMin<qint64>(available, phaseBudgetMs))); ///< 本阶段的实际截止时间。
        for (;;) {
            bool foundRunning = false; ///< 本轮是否还有仍在运行的进程。
            for (Entry* entry : entries_) { ///< 当前检查的模块记录。
                QProcess* process = entry->process; ///< 该模块的子进程句柄。
                if (process == nullptr || process->state() == QProcess::NotRunning)
                    continue;
                foundRunning = true;
                const qint64 remaining = qMin(phaseDeadline.remainingTime(),
                                              totalDeadline.remainingTime()); ///< 本次等待允许消耗的毫秒数。
                if (remaining <= 0)
                    return;
                process->waitForFinished(
                    static_cast<int>(qMin<qint64>(10, remaining)));
            }
            if (!foundRunning || phaseDeadline.hasExpired() || totalDeadline.hasExpired())
                return;
        }
    };

    waitForAll(qMax(1, processConfig_.stopTimeoutMs));
    for (Entry* entry : entries_) { ///< 当前尝试温和终止的模块记录。
        if (entry->process != nullptr &&
            entry->process->state() != QProcess::NotRunning)
            entry->process->terminate();
    }
    waitForAll(250);
    for (Entry* entry : entries_) { ///< 当前尝试强杀的模块记录。
        if (entry->process != nullptr &&
            entry->process->state() != QProcess::NotRunning)
            entry->process->kill();
    }
    waitForAll(static_cast<int>(qMax<qint64>(0, totalDeadline.remainingTime())));
    for (Entry* entry : entries_) { ///< 当前收尾并更新状态的模块记录。
        if (entry->process == nullptr ||
            entry->process->state() == QProcess::NotRunning) {
            finishEntryAfterProcessExit(entry, QString());
        } else {
            entry->pendingStart = false;
            entry->lastError = QString::fromUtf8(u8"应用退出时子进程在总 deadline 后仍未退出");
            entry->phase = LifecyclePhase::Failed;
            emitState(entry, QStringLiteral("Failed"), entry->lastError);
        }
    }
}

// 遍历 Entry 状态生成当前运行模块 ID 快照。
QStringList ProcessSupervisor::runningModuleIds() const
{
    QStringList result; ///< 运行中模块 ID 的收集结果。
    for (const Entry* entry : entries_) { ///< 当前检查的模块记录。
        if (entry->phase == LifecyclePhase::Running)
            result.append(entry->config.id);
    }
    return result;
}

// 未知模块返回空字符串，调用方可与 Running/Failed 等状态区分。
QString ProcessSupervisor::state(const QString& moduleId) const
{
    const Entry* entry = findEntry(moduleId); ///< 目标模块的监督记录。
    return entry == nullptr ? QString() : entry->state;
}

ProcessQueueStats ProcessSupervisor::queueStats(const QString& moduleId) const
{
    ProcessQueueStats result; ///< 待返回的统计快照。
    Entry* entry = findEntry(moduleId); ///< 目标模块的监督记录。
    if (entry == nullptr)
        return result;
    QMutexLocker locker(&entry->outgoingMutex); ///< 读取统计前先锁住该模块的发送队列。
    result.dropped = entry->outgoingDropped;
    result.rejected = entry->outgoingRejected;
    result.abandoned = entry->outgoingAbandoned;
    result.pending = entry->outgoingQueue.size();
    result.inFlight = entry->outgoingInFlightCount;
    return result;
}

// 保存最新 QSS，并广播给所有已注册且连接正常的 UI 子进程。
void ProcessSupervisor::applyStyleSheet(const QString& styleSheet)
{
    styleSheet_ = styleSheet;
    for (Entry* entry : entries_) { ///< 当前接收样式下发的模块记录。
        if (entry->config.type != ModuleType::ProcessUi || !isRegisteredPhase(entry->phase))
            continue;
        QJsonObject frame; ///< 样式下发控制帧。
        frame.insert(QStringLiteral("type"), kStyleSheet);
        frame.insert(QStringLiteral("styleSheet"), styleSheet_);
        if (!sendFrame(entry, frame))
            handleFault(entry, QString::fromUtf8(u8"子进程样式控制帧发送失败"));
    }
}

// 以下查找函数只返回监督器拥有的借用指针，不转移 Entry 所有权。
ProcessSupervisor::Entry* ProcessSupervisor::findEntry(const QString& moduleId) const
{
    for (Entry* entry : entries_) { ///< 当前比对的模块记录。
        if (entry->config.id == moduleId)
            return entry;
    }
    return nullptr;
}

// 根据 Qt signal 的 sender() 反查对应本地服务器。
ProcessSupervisor::Entry* ProcessSupervisor::findEntryByServer(QObject* object) const
{
    for (Entry* entry : entries_) { ///< 当前比对的模块记录。
        if (entry->server == object)
            return entry;
    }
    return nullptr;
}

// 根据 readyRead/disconnected 的 sender() 反查子进程连接。
ProcessSupervisor::Entry* ProcessSupervisor::findEntryBySocket(QObject* object) const
{
    for (Entry* entry : entries_) { ///< 当前比对的模块记录。
        if (entry->socket == object)
            return entry;
    }
    return nullptr;
}

// 根据 QProcess 错误/退出信号反查模块配置。
ProcessSupervisor::Entry* ProcessSupervisor::findEntryByProcess(QObject* object) const
{
    for (Entry* entry : entries_) { ///< 当前比对的模块记录。
        if (entry->process == object)
            return entry;
    }
    return nullptr;
}

// 为一个 Entry 创建随机服务器名/令牌、QLocalServer 和 QProcess 后立即返回。
bool ProcessSupervisor::beginStartEntry(Entry* entry, QString* errorMessage)
{
    if (entry == nullptr)
        return false;
    if (entry->process != nullptr &&
        entry->process->state() != QProcess::NotRunning) {
        const QString detail = QString::fromUtf8(
            u8"旧子进程尚未退出，拒绝启动下一轮：%1").arg(entry->config.id); ///< 旧进程仍存活时的错误说明。
        if (errorMessage != nullptr)
            *errorMessage = detail;
        handleFault(entry, detail);
        return false;
    }
    destroyRuntime(entry);
    entry->prepareExitSupported = false;
    ++entry->generation;
    {
        QMutexLocker locker(&entry->outgoingMutex); ///< 重置本代发送队列统计。
        // 每个 generation 独立记账；上一轮汇总已在 destroyRuntime 输出。
        entry->outgoingDropped = 0;
        entry->outgoingRejected = 0;
        entry->outgoingAbandoned = 0;
        entry->lastOutgoingWarningMs = 0;
    }
    entry->phase = LifecyclePhase::StartingProcess;
    entry->lastError.clear();
    entry->stopAcknowledged = false;
    entry->stopEscalation = 0;
    entry->pendingStart = false;
    entry->restartAtMs = 0;
    entry->serverName = makeServerName(entry->config.id);
    entry->token = QUuid::createUuid().toString(QUuid::Id128);

    const QFileInfo fileInfo(entry->config.filePath); ///< 子进程可执行文件信息。
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
            .arg(entry->server->errorString()); ///< 监听失败的具体原因。
        if (errorMessage != nullptr)
            *errorMessage = error;
        handleFault(entry, error);
        return false;
    }
    connect(entry->server, &QLocalServer::newConnection,
            this, &ProcessSupervisor::onServerConnection);

    entry->process = new QProcess(this);
    connect(entry->process,
            &QProcess::started,
            this,
            &ProcessSupervisor::onProcessStarted);
    connect(entry->process,
            &QProcess::errorOccurred,
            this,
            &ProcessSupervisor::onProcessError);
    connect(entry->process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &ProcessSupervisor::onProcessFinished);
    entry->process->setWorkingDirectory(fileInfo.absolutePath());
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment(); ///< 子进程环境变量，继承系统环境后追加框架路径。
    const QString applicationDirectory = QCoreApplication::applicationDirPath(); ///< 主程序所在目录，用作插件与平台插件搜索基准。
    // 子进程和其内部 worker 使用同一份发布目录配置；不依赖 QProcess 的工作目录。
    environment.insert(QStringLiteral("QFRAMEWORK_APP_DIR"), applicationDirectory);
    environment.insert(QStringLiteral("PATH"),
                       applicationDirectory + QDir::listSeparator() + environment.value(QStringLiteral("PATH")));
    environment.insert(QStringLiteral("QT_PLUGIN_PATH"), applicationDirectory);
    environment.insert(QStringLiteral("QT_QPA_PLATFORM_PLUGIN_PATH"),
                       QDir(applicationDirectory).filePath(QStringLiteral("platforms")));
    entry->process->setProcessEnvironment(environment);

    QStringList arguments; ///< 传给子进程的内部命令行参数。
    arguments << QStringLiteral("--qframework-server") << entry->serverName
              << QStringLiteral("--qframework-token") << entry->token
              << QStringLiteral("--qframework-module-id") << entry->config.id
              << QStringLiteral("--qframework-module-type") << processTypeName(entry->config.type)
              << QStringLiteral("--qframework-shared-memory-threshold")
              << QString::number(messageBusConfig_.sharedMemoryThresholdBytes)
              << QStringLiteral("--qframework-max-message-bytes")
              << QString::number(maxConfiguredMessageBytes(messageBusConfig_))
              << QStringLiteral("--qframework-shutdown-drain-timeout")
              << QString::number(messageBusConfig_.shutdownDrainTimeoutMs)
              << QStringLiteral("--qframework-debugger-timeout-ms")
              << QString::number(entry->config.debuggerWaitTimeoutMs);
    if (entry->config.waitForDebugger)
        arguments << QStringLiteral("--qframework-wait-for-debugger");

    emitState(entry,
              entry->stopPurpose == StopPurpose::ManualRestart ||
                      entry->stopPurpose == StopPurpose::AutoRestart
                  ? QStringLiteral("Restarting") : QStringLiteral("Starting"),
              QString());
    entry->startupDeadline = QDeadlineTimer(qMax(1, processConfig_.registrationTimeoutMs));
    entry->lastPongMs = QDateTime::currentMSecsSinceEpoch();
    entry->lastPingMs = 0;
    entry->process->start(entry->config.filePath, arguments);
    return true;
}

// 编码并写入已连接 Socket；这里只判断写入是否被 Qt 接受。
bool ProcessSupervisor::sendFrame(Entry* entry, const QJsonObject& frame)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (entry == nullptr || entry->socket == nullptr ||
        entry->socket->state() != QLocalSocket::ConnectedState)
        return false;
    Q_ASSERT(entry->socket->thread() == QThread::currentThread());
    const QByteArray encoded = process::encodeFrame(frame); ///< 编码后的完整帧字节。
    return entry->socket->write(encoded) == encoded.size();
}

// 接受第一个合法待连接 Socket；同一 Entry 的额外连接立即拒绝。
void ProcessSupervisor::onServerConnection()
{
    Entry* entry = findEntryByServer(sender()); ///< 发出新连接信号的模块记录。
    if (entry == nullptr || entry->server == nullptr)
        return;
    while (entry->server->hasPendingConnections()) {
        QLocalSocket* socket = entry->server->nextPendingConnection(); ///< 本次取出的待处理连接。
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
    Entry* entry = findEntryBySocket(sender()); ///< 发出可读信号的模块记录。
    if (entry == nullptr || entry->socket == nullptr)
        return;
    entry->inputBuffer.append(entry->socket->readAll());
    for (;;) {
        QJsonObject frame; ///< 本次解析出的协议帧。
        QString error; ///< 承接取帧失败原因。
        const process::FrameResult result = process::takeFrame(
            &entry->inputBuffer,
            &frame,
            maxFrameBytes(maxConfiguredMessageBytes(messageBusConfig_)),
            &error); ///< 本次取帧的状态。
        if (result == process::FrameResult::Incomplete)
            return;
        if (result == process::FrameResult::Invalid) {
            handleFault(entry, detailWithPrefix(QString::fromUtf8(u8"IPC 帧无效"), error));
            return;
        }
        handleFrame(entry, frame);
        if (isStoppingPhase(entry->phase) ||
            entry->phase == LifecyclePhase::Failed ||
            entry->phase == LifecyclePhase::RestartDelay)
            return;
    }
}

// 非停止期间的连接断开属于模块故障，可能触发自动重启。
void ProcessSupervisor::onSocketDisconnected()
{
    Entry* entry = findEntryBySocket(sender()); ///< 发出断开信号的模块记录。
    if (entry == nullptr || isStoppingPhase(entry->phase) ||
        entry->phase == LifecyclePhase::Failed ||
        entry->phase == LifecyclePhase::RestartDelay || shuttingDown_)
        return;
    handleFault(entry, QString::fromUtf8(u8"子进程 IPC 连接断开"));
}

// QProcess 已启动后进入注册阶段；这里只推进当前 generation。
void ProcessSupervisor::onProcessStarted()
{
    Entry* entry = findEntryByProcess(sender()); ///< 发出启动信号的模块记录。
    if (entry != nullptr && entry->phase == LifecyclePhase::StartingProcess)
        entry->phase = LifecyclePhase::WaitingRegistration;
}

// 只把无法启动和崩溃升级为监督器故障，其他状态由 finished/Socket 路径处理。
void ProcessSupervisor::onProcessError(QProcess::ProcessError error)
{
    Entry* entry = findEntryByProcess(sender()); ///< 发出错误信号的模块记录。
    if (entry == nullptr || isStoppingPhase(entry->phase) || shuttingDown_)
        return;
    if (error == QProcess::FailedToStart || error == QProcess::Crashed)
        handleFault(entry, QString::fromUtf8(u8"子进程启动或运行失败"));
}

// 非预期退出转换为可读错误，并进入统一清理/重启策略。
void ProcessSupervisor::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Entry* entry = findEntryByProcess(sender()); ///< 发出退出信号的模块记录。
    if (entry == nullptr)
        return;
    if (isStoppingPhase(entry->phase) ||
        entry->stopPurpose != StopPurpose::None || shuttingDown_) {
        finishEntryAfterProcessExit(entry, entry->lastError);
        return;
    }
    const QString detail = exitStatus == QProcess::CrashExit
        ? QString::fromUtf8(u8"子进程异常退出")
        : QString::fromUtf8(u8"子进程退出，代码 %1").arg(exitCode); ///< 面向用户的退出原因文本。
    handleFault(entry, detail);
}

// 分派注册、生命周期、心跳、窗口、日志、ACK 和子到父业务消息帧。
void ProcessSupervisor::handleFrame(Entry* entry, const QJsonObject& frame)
{
    if (entry == nullptr)
        return;
    const QString type = frame.value(QStringLiteral("type")).toString(); ///< 帧类型，决定分派分支。
    if (type == kRegister) {
        if (entry->bridge != nullptr ||
            (entry->phase != LifecyclePhase::WaitingRegistration &&
             entry->phase != LifecyclePhase::StartingProcess)) {
            handleFault(entry, QString::fromUtf8(u8"子进程重复注册"));
            return;
        }
        const QString moduleId = frame.value(QStringLiteral("moduleId")).toString(); ///< 子进程声明的模块 ID。
        const QString moduleType = frame.value(QStringLiteral("moduleType")).toString(); ///< 子进程声明的模块类型文本。
        const QString token = frame.value(QStringLiteral("token")).toString(); ///< 子进程回传的一次性认证令牌。
        QStringList publishedTopics; ///< 子进程声明的发布主题。
        QStringList subscribedTopics; ///< 子进程声明的订阅主题。
        if (moduleId != entry->config.id ||
            moduleType != processTypeName(entry->config.type) ||
            token != entry->token ||
            !jsonStringList(frame.value(QStringLiteral("publishedTopics")), &publishedTopics) ||
            !jsonStringList(frame.value(QStringLiteral("subscribedTopics")), &subscribedTopics)) {
            QJsonObject rejected; ///< 校验失败时回复的拒绝帧。
            rejected.insert(QStringLiteral("type"), kRegisterAck);
            rejected.insert(QStringLiteral("accepted"), false);
            handleFault(
                entry,
                sendFrame(entry, rejected)
                    ? QString::fromUtf8(u8"子进程注册信息校验失败")
                    : QString::fromUtf8(u8"子进程注册拒绝帧发送失败"));
            return;
        }

        entry->prepareExitSupported = frame.value(QStringLiteral("prepareExitSupported")).toBool();
        entry->bridge = new ProcessBridge(this,
                                          entry->config.id,
                                          publishedTopics,
                                          subscribedTopics);
        QString busError; ///< 承接注册到消息总线失败的原因。
        if (!messageBus_->registerModule(entry->config.id, entry->bridge, &busError)) {
            delete entry->bridge;
            entry->bridge = nullptr;
            QJsonObject rejected; ///< 总线注册失败时回复的拒绝帧。
            rejected.insert(QStringLiteral("type"), kRegisterAck);
            rejected.insert(QStringLiteral("accepted"), false);
            handleFault(
                entry,
                sendFrame(entry, rejected)
                    ? detailWithPrefix(QString::fromUtf8(u8"子进程注册失败"), busError)
                    : QString::fromUtf8(u8"子进程注册失败且拒绝帧发送失败"));
            return;
        }
        entry->phase = LifecyclePhase::WaitingStarted;
        entry->startupDeadline = QDeadlineTimer(
            qMax(1, processConfig_.registrationTimeoutMs) +
            (entry->config.waitForDebugger
                 ? qMax(1, entry->config.debuggerWaitTimeoutMs) : 0));
        entry->lastPongMs = QDateTime::currentMSecsSinceEpoch();
        // 允许 onStart() 发布，交付是否开始仍由上层统一控制。
        messageBus_->setModuleRunning(entry->config.id, true);
        {
            QMutexLocker locker(&entry->outgoingMutex); ///< 打开该模块的发送入口。
            entry->outgoingAccepting = true;
            entry->outgoingStopping = false;
        }
        // 注册确认不仅表示“可以启动”，还把父进程最终采用的队列规则
        // 下发给子进程。父、子两侧因此使用同一份 QueueCapacity、大小上限
        // 和 Latest/Reliable 策略，而不是各自猜一个默认值。
        QJsonObject accepted; ///< 注册确认帧。
        accepted.insert(QStringLiteral("type"), kRegisterAck);
        accepted.insert(QStringLiteral("accepted"), true);
        accepted.insert(QStringLiteral("defaultQueueCapacity"),
                        qMax(1, messageBusConfig_.defaultQueueCapacity));
        accepted.insert(QStringLiteral("defaultMaxMessageBytes"),
                        qMax(1, messageBusConfig_.maxMessageBytes));
        accepted.insert(QStringLiteral("defaultPolicy"),
                        queuePolicyName(messageBusConfig_.defaultPolicy));
        QJsonArray topicConfigs; ///< 下发给子进程的主题级队列规则数组。
        QSet<QString> seenTopics; ///< 已下发主题集合，用于跨发布/订阅去重。
        appendTopicConfig(messageBusConfig_, publishedTopics, &topicConfigs, &seenTopics);
        appendTopicConfig(messageBusConfig_, subscribedTopics, &topicConfigs, &seenTopics);
        accepted.insert(QStringLiteral("topicConfigs"), topicConfigs);
        if (entry->config.type == ModuleType::ProcessUi)
            accepted.insert(QStringLiteral("styleSheet"), styleSheet_);
        if (!sendFrame(entry, accepted))
            handleFault(entry, QString::fromUtf8(u8"子进程注册确认帧发送失败"));
        return;
    }
    if (!isRegisteredPhase(entry->phase))
        return;
    if (type == kStarted) {
        if (entry->phase != LifecyclePhase::WaitingStarted)
            return;
        const StopPurpose completedPurpose = entry->stopPurpose; ///< 启动前记录的停止目的，用于判断是否属于重启。
        entry->phase = LifecyclePhase::Running;
        entry->stopPurpose = StopPurpose::None;
        entry->lastPongMs = QDateTime::currentMSecsSinceEpoch();
        entry->lastPingMs = 0;
        emitState(entry, QStringLiteral("Running"), QString());
        settleStartupEntry(entry, QString());
        if (completedPurpose == StopPurpose::ManualRestart)
            emit restartFinished(entry->config.id, true, QString());
        setOperationBusy(entry, false);
        return;
    }
    if (type == kStartFailed) {
        messageBus_->setModuleRunning(entry->config.id, false);
        handleFault(entry,
                    detailWithPrefix(QString::fromUtf8(u8"子进程 onStart 失败"),
                                     frame.value(QStringLiteral("detail")).toString()));
        return;
    }
    if (type == QStringLiteral("exitReadiness")) {
        if (!entry->prepareId.isEmpty() && entry->phase == LifecyclePhase::Running &&
            frame.value(QStringLiteral("requestId")).toString() == entry->prepareId)
            entry->exitReady = frame.value(QStringLiteral("ready")).toBool();
        return;
    }
    if (type == kPong) {
        entry->lastPongMs = QDateTime::currentMSecsSinceEpoch();
        return;
    }
    if (type == kStopAck) {
        entry->stopAcknowledged = true;
        if (frame.contains(QStringLiteral("clean")) &&
            !frame.value(QStringLiteral("clean")).toBool()) {
            entry->lastError = frame.value(QStringLiteral("detail")).toString(
                QString::fromUtf8(u8"子进程未干净停止"));
        }
        advanceStopEntry(entry);
        return;
    }
    if (type == kDebugWaitTimeout) {
        Logger::instance().log(LogLevel::Warning,
                               entry->config.id,
                               QString::fromUtf8(u8"等待调试器超时，继续启动"));
        return;
    }
    if (type == kWindowReady) {
        const QString value = frame.value(QStringLiteral("windowId")).toString(); ///< 子进程上报的窗口句柄文本。
        bool ok = false; ///< 句柄文本是否解析成功。
        const quintptr windowId = static_cast<quintptr>(value.toULongLong(&ok)); ///< 解析出的原生窗口句柄。
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
        const QString messageId = frame.value(QStringLiteral("messageId")).toString(); ///< 被确认的消息 ID。
        // 兼容旧子进程：旧版本只为共享内存发送 sharedAck。
        acknowledgeChildMessage(entry, messageId, true);
        return;
    }
    if (type == kLog) {
        const int levelValue = frame.value(QStringLiteral("level")).toInt(); ///< 日志级别的整数取值。
        const LogLevel level = levelValue >= static_cast<int>(LogLevel::Debug) &&
                               levelValue <= static_cast<int>(LogLevel::Error)
            ? static_cast<LogLevel>(levelValue)
            : LogLevel::Warning; ///< 校验后的日志级别，越界时回退为 Warning。
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
    const QString topic = frame.value(QStringLiteral("topic")).toString(); ///< 消息主题。
    const QString messageId = frame.value(QStringLiteral("messageId")).toString(); ///< 用于回 publishAck 的消息 ID。
    const QString transport = frame.value(QStringLiteral("transport")).toString(); ///< 传输方式：shared 或 inline。
    QByteArray data; ///< 还原出的消息原始字节。
    bool valid = !topic.isEmpty() && !messageId.isEmpty(); ///< 当前消息是否仍然合法可接收。
    if (transport == QStringLiteral("shared")) {
        const QString key = frame.value(QStringLiteral("sharedKey")).toString(); ///< 共享内存段名称。
        QSharedMemory shared(key); ///< 只读附着的共享内存段。
        if (shared.attach(QSharedMemory::ReadOnly)) {
            if (shared.lock()) {
                const int declaredSize = frame.value(QStringLiteral("size")).toInt(); ///< 帧中声明的 payload 字节数。
                const int copySize = qMin(shared.size(), qMax(0, declaredSize)); ///< 实际可复制的字节数。
                if (declaredSize > 0 && copySize == declaredSize &&
                    copySize <= maxConfiguredMessageBytes(messageBusConfig_)) {
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
    bool accepted = false; ///< 消息总线是否接受该消息。
    const TopicConfig config = effectiveTopicConfig(messageBusConfig_, topic); ///< 该主题最终生效的队列规则。
    if (valid && data.size() <= config.maxMessageBytes) {
        // 跨进程接收已经得到一份独立 QByteArray；移动到 MessagePayload 后，
        // 主进程所有订阅者共用该对象，ACK 结果仍表示 MessageBus 是否接收。
        accepted = messageBus_->publishSharedFromModule(
            entry->config.id, topic, makeMessagePayload(qMove(data)));
    }

    // 这个 ACK 与父到子的 deliveryAck 是两件事：这里确认的是“父进程的
    // MessageBus 已接收/拒绝”，子进程据此释放自己的在途槽位和共享段。
    QJsonObject ack; ///< 子到父消息的接收确认帧。
    ack.insert(QStringLiteral("type"), kPublishAck);
    ack.insert(QStringLiteral("messageId"), messageId);
    ack.insert(QStringLiteral("accepted"), accepted);
    if (!sendFrame(entry, ack) && !isStoppingPhase(entry->phase))
        handleFault(entry, QString::fromUtf8(u8"子进程 publishAck 发送失败"));
}

// 定时推进每个 Entry 自己的 deadline；不进入嵌套事件循环，也不借用别人的预算。
void ProcessSupervisor::onSupervisionTick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch(); ///< 本次心跳的当前毫秒时间戳。
    for (Entry* entry : entries_) { ///< 当前推进状态的模块记录。
        if (entry->phase == LifecyclePhase::RestartDelay &&
            entry->restartAtMs > 0 && now >= entry->restartAtMs && !shuttingDown_ && !preparingExit_) {
            entry->restartAtMs = 0;
            QString error; ///< 承接重启启动失败原因。
            if (!beginStartEntry(entry, &error))
                entry->lastError = error;
            continue;
        }
        if ((entry->phase == LifecyclePhase::StartingProcess ||
             entry->phase == LifecyclePhase::WaitingRegistration ||
             entry->phase == LifecyclePhase::WaitingStarted) &&
            entry->startupDeadline.hasExpired()) {
            handleFault(
                entry,
                entry->phase == LifecyclePhase::WaitingStarted
                    ? QString::fromUtf8(u8"等待子进程 onStart 完成超时")
                    : QString::fromUtf8(u8"子进程注册超时"));
            continue;
        }
        if (isStoppingPhase(entry->phase)) {
            advanceStopEntry(entry);
            if (!entry->stopDeadline.hasExpired())
                continue;
            if (entry->process == nullptr ||
                entry->process->state() == QProcess::NotRunning) {
                finishEntryAfterProcessExit(entry, entry->lastError);
                continue;
            }
            if (entry->stopEscalation == 0) {
                entry->process->terminate();
                entry->stopEscalation = 1;
                entry->phase = LifecyclePhase::WaitingProcessExit;
                entry->stopDeadline = QDeadlineTimer(250);
            } else if (entry->stopEscalation == 1) {
                // QProcess::kill() 结束完整子进程隔离边界，不会强杀其中一条线程。
                entry->process->kill();
                entry->stopEscalation = 2;
                entry->stopDeadline = QDeadlineTimer(1000);
            } else {
                entry->pendingStart = false;
                entry->lastError = QString::fromUtf8(u8"子进程强制结束后仍未退出");
                entry->phase = LifecyclePhase::Failed;
                emitState(entry, QStringLiteral("Failed"), entry->lastError);
                if (entry->stopPurpose == StopPurpose::ManualRestart)
                    emit restartFinished(entry->config.id, false, entry->lastError);
                setOperationBusy(entry, false);
            }
            continue;
        }
        if (entry->phase != LifecyclePhase::Running || entry->process == nullptr)
            continue;
        if (preparingExit_ && entry->prepareId.isEmpty())
            beginPrepareEntry(entry, StopPurpose::None);
        if (!entry->prepareId.isEmpty() && !pollPrepareEntry(entry))
            continue;
        if (entry->socket == nullptr || entry->socket->state() != QLocalSocket::ConnectedState)
            continue;
        if (now - entry->lastPingMs >= qMax(1, processConfig_.heartbeatIntervalMs)) {
            QJsonObject ping; ///< 心跳探测帧。
            ping.insert(QStringLiteral("type"), kPing);
            if (!sendFrame(entry, ping)) {
                handleFault(entry, QString::fromUtf8(u8"子进程心跳帧发送失败"));
                continue;
            }
            // 只有 Qt Socket 接受了完整帧才开始等待 pong，不掩盖立即写失败。
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
                                              const MessagePayload& payload)
{
    Entry* entry = findEntry(moduleId); ///< 目标模块的监督记录。
    if (entry == nullptr || topic.isEmpty())
        return false;

    const TopicConfig config = effectiveTopicConfig(messageBusConfig_, topic); ///< 该主题最终生效的队列规则。
    bool schedule = false; ///< 是否需要安排一次排空唤醒。
    bool accepted = true; ///< 消息是否被本地等待队列接受。
    bool logWarning = false; ///< 本次是否需要记录一条限流告警。
    quint64 dropped = 0; ///< 操作后的丢弃累计数。
    quint64 rejected = 0; ///< 操作后的拒绝累计数。
    {
        QMutexLocker locker(&entry->outgoingMutex); ///< 保护该模块发送队列的互斥锁。
        // 这里是父进程到子进程的“第一道闸门”。停止、断线或重启会把
        // outgoingAccepting 置为 false，所以生产者不会进入旧 generation。
        if (payload.isNull() || payload->size() > config.maxMessageBytes ||
            !entry->outgoingAccepting || entry->outgoingStopping) {
            ++entry->outgoingRejected;
            accepted = false;
        } else if (queuedTopicCount(entry->outgoingQueue, topic) >=
                   config.queueCapacity) {
            if (config.policy == QueuePolicy::Reliable) {
                // Reliable 的契约是“宁可拒绝，也不能覆盖”。注意这里的
                // 等待队列与在途计数分开限制，旧消息绝不被新消息覆盖。
                ++entry->outgoingRejected;
                accepted = false;
            } else {
                // Latest 只覆盖同主题仍在等待发送的最旧帧，绝不会触碰别的主题，
                // 也不会覆盖已经写入 Socket、正在等待 ACK 的帧。
                int oldestIndex = -1; ///< 该主题最旧等待项的下标，用于 Latest 覆盖。
                for (int index = 0; index < entry->outgoingQueue.size(); ++index) { ///< 查找最旧同主题项的队列下标。
                    if (entry->outgoingQueue.at(index).topic == topic) {
                        oldestIndex = index;
                        break;
                    }
                }
                if (oldestIndex < 0) {
                    ++entry->outgoingRejected;
                    accepted = false;
                } else {
                    entry->outgoingQueue.removeAt(oldestIndex);
                    ++entry->outgoingDropped;
                }
            }
        }

        if (accepted) {
            OutgoingMessage message; ///< 本次入队的等待项。
            message.topic = topic;
            message.senderModuleId = senderModuleId;
            message.payload = payload;
            entry->outgoingQueue.enqueue(message);
            if (!entry->outgoingWakeScheduled) {
                // 用一个布尔标记把很多并发入队合并为一次 Qt 唤醒事件。
                entry->outgoingWakeScheduled = true;
                schedule = true;
            }
        }
        dropped = entry->outgoingDropped;
        rejected = entry->outgoingRejected;
        const qint64 now = QDateTime::currentMSecsSinceEpoch(); ///< 当前毫秒时间戳，用于限流告警。
        if ((dropped > 0 || rejected > 0) &&
            now - entry->lastOutgoingWarningMs >= 1000) {
            entry->lastOutgoingWarningMs = now;
            logWarning = true;
        }
    }

    if (logWarning) {
        Logger::instance().log(
            LogLevel::Warning,
            entry->config.id,
            QString::fromUtf8(u8"父到子发送队列发生丢弃/拒绝：dropped=%1，rejected=%2")
                .arg(dropped)
                .arg(rejected));
    }
    if (schedule) {
        QMetaObject::invokeMethod(this,
                                  "drainChildQueue",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, moduleId));
    }
    return accepted;
}

// 在监督器 Qt 线程中有限批次写 Socket，并建立等待 deliveryAck 的在途记录。
void ProcessSupervisor::drainChildQueue(const QString& moduleId)
{
    Entry* entry = findEntry(moduleId); ///< 目标模块的监督记录。
    if (entry == nullptr)
        return;

    // 每次最多处理一小批，避免监督器线程长时间被一个高吞吐模块占用。
    // 批次结束后若仍有可发送项，再安排下一次合并唤醒。
    const int batchLimit = 8; ///< 单次排空最多发送的条数。
    for (int sent = 0; sent < batchLimit; ++sent) { ///< 本轮已发送的条数。
        if (entry->socket == nullptr ||
            entry->socket->state() != QLocalSocket::ConnectedState) {
            QMutexLocker locker(&entry->outgoingMutex); ///< 连接不可用时清除唤醒标记。
            entry->outgoingWakeScheduled = false;
            return;
        }

        OutgoingMessage message; ///< 本次取出待发送的消息。
        bool haveMessage = false; ///< 是否成功取到一条有可用在途槽位的消息。
        {
            QMutexLocker locker(&entry->outgoingMutex); ///< 保护等待队列与在途表的互斥锁。
            if (!entry->outgoingAccepting || entry->outgoingStopping)
                break;

            // 只挑选“该主题在途数量还没有达到容量”的项。这样即使 Socket
            // 写得很快，未收到 ACK 的消息数量也不会突破 QueueCapacity。
            int selectedIndex = -1; ///< 选中的等待项下标。
            for (int index = 0; index < entry->outgoingQueue.size(); ++index) { ///< 扫描等待队列的下标。
                const OutgoingMessage& candidate = entry->outgoingQueue.at(index); ///< 当前候选等待项。
                const TopicConfig config = effectiveTopicConfig(messageBusConfig_,
                                                                 candidate.topic); ///< 该候选主题的容量配置。
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
        const QByteArray& data = *message.payload; ///< 本次要发送的不可变载荷引用。

        // 等待队列中的项故意没有 messageId；现在它已经占用一个在途槽位，
        // 才生成唯一 ID，并建立 ID->主题映射供 deliveryAck 回收计数。
        const QString messageId = QUuid::createUuid().toString(QUuid::Id128); ///< 本条消息的关联 ID。
        {
            QMutexLocker locker(&entry->outgoingMutex); ///< 登记反向索引时保护映射表。
            entry->outgoingMessageTopics.insert(messageId, message.topic);
        }

        QJsonObject frame; ///< 承载本条消息的控制帧。
        frame.insert(QStringLiteral("type"), kMessage);
        frame.insert(QStringLiteral("topic"), message.topic);
        frame.insert(QStringLiteral("senderModuleId"), message.senderModuleId);
        frame.insert(QStringLiteral("messageId"), messageId);
        if (data.size() >= messageBusConfig_.sharedMemoryThresholdBytes) {
            // 大消息只在共享内存中复制一次；Socket 里只放 key 和 size 等控制信息。
            // 共享段的所有权仍在父进程，必须一直保留到子进程 ACK。
            const QString key = makeSharedKey(moduleId); ///< 本次共享内存段名称。
            QSharedMemory* shared = new QSharedMemory(key, this); ///< 承载大 payload 的共享内存段。
            if (!shared->create(data.size())) {
                delete shared;
                discardChildMessageBeforeSend(entry, messageId);
                continue;
            }
            if (!shared->lock()) {
                shared->detach();
                delete shared;
                discardChildMessageBeforeSend(entry, messageId);
                continue;
            }
            std::memcpy(shared->data(),
                        data.constData(),
                        static_cast<size_t>(data.size()));
            shared->unlock();
            {
                QMutexLocker locker(&entry->outgoingMutex); ///< 登记共享段时保护映射表。
                entry->outgoingShared.insert(messageId, shared);
            }
            frame.insert(QStringLiteral("transport"), QStringLiteral("shared"));
            frame.insert(QStringLiteral("sharedKey"), key);
            frame.insert(QStringLiteral("size"), data.size());
        } else {
            // 小消息直接放进 JSON 控制帧。它仍然受单条大小上限约束，且不会
            // 作为 QByteArray 参数附着在每个 Qt queued event 上。
            frame.insert(QStringLiteral("transport"), QStringLiteral("inline"));
            frame.insert(QStringLiteral("data"),
                         QString::fromLatin1(data.toBase64()));
        }

        if (!sendFrame(entry, frame)) {
            discardChildMessageBeforeSend(entry, messageId);
            handleFault(entry, QString::fromUtf8(u8"无法向子进程发送消息"));
            return;
        }
    }

    bool schedule = false; ///< 批次结束后是否还需要安排下一次排空。
    {
        QMutexLocker locker(&entry->outgoingMutex); ///< 判断是否仍有可发送项时保护队列。
        bool eligible = false; ///< 是否存在仍在容量之内的等待项。
        if (entry->outgoingAccepting && !entry->outgoingStopping &&
            entry->socket != nullptr &&
            entry->socket->state() == QLocalSocket::ConnectedState) {
            for (const OutgoingMessage& candidate : entry->outgoingQueue) { ///< 当前检查的等待项。
                const TopicConfig config = effectiveTopicConfig(messageBusConfig_,
                                                                 candidate.topic); ///< 该候选主题的容量配置。
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

void ProcessSupervisor::discardChildMessageBeforeSend(Entry* entry,
                                                       const QString& messageId)
{
    if (entry == nullptr || messageId.isEmpty())
        return;
    QSharedMemory* shared = nullptr; ///< 待释放的共享段，可能不存在。
    {
        QMutexLocker locker(&entry->outgoingMutex); ///< 回收在途记录时保护映射表。
        const QString topic = entry->outgoingMessageTopics.take(messageId); ///< 该消息的主题，取走后不再重复计数。
        if (topic.isEmpty())
            return;
        const int count = entry->outgoingInFlightByTopic.value(topic, 0); ///< 该主题当前的在途条数。
        if (count <= 1)
            entry->outgoingInFlightByTopic.remove(topic);
        else
            entry->outgoingInFlightByTopic.insert(topic, count - 1);
        entry->outgoingInFlightCount = qMax(0, entry->outgoingInFlightCount - 1);
        ++entry->outgoingDropped;
        shared = entry->outgoingShared.take(messageId);
    }
    if (shared != nullptr) {
        shared->detach();
        delete shared;
    }
}

// 根据 deliveryAck 回收父到子的在途槽位、主题计数和共享段。
void ProcessSupervisor::acknowledgeChildMessage(Entry* entry,
                                                const QString& messageId,
                                                bool accepted)
{
    if (entry == nullptr || messageId.isEmpty())
        return;

    QSharedMemory* shared = nullptr; ///< 待释放的共享段，可能不存在。
    QString topic; ///< 该消息的主题，取走后不再重复计数。
    bool schedule = false; ///< 释放槽位后是否还需要安排下一次排空。
    bool logRejected = false; ///< 本次是否需要记录一条拒绝告警。
    quint64 rejectedCount = 0; ///< 累计被拒绝的消息条数。
    {
        QMutexLocker locker(&entry->outgoingMutex); ///< 回收在途记录时保护映射表。
        // take() 同时完成“验证 ACK 属于当前在途消息”和“删除待回收记录”。
        // 重复 ACK 或迟到的旧 ACK 找不到 topic 时直接忽略，避免重复减少计数。
        topic = entry->outgoingMessageTopics.take(messageId);
        if (topic.isEmpty())
            return;
        if (!accepted) {
            ++entry->outgoingRejected;
            const qint64 now = QDateTime::currentMSecsSinceEpoch(); ///< 当前毫秒时间戳，用于限流告警。
            if (now - entry->lastOutgoingWarningMs >= 1000) {
                entry->lastOutgoingWarningMs = now;
                logRejected = true;
                rejectedCount = entry->outgoingRejected;
            }
        }
        const int count = entry->outgoingInFlightByTopic.value(topic, 0); ///< 该主题当前的在途条数。
        if (count <= 1)
            entry->outgoingInFlightByTopic.remove(topic);
        else
            entry->outgoingInFlightByTopic.insert(topic, count - 1);
        entry->outgoingInFlightCount = qMax(0, entry->outgoingInFlightCount - 1);
        // 无论 ACK 表示接收还是拒绝，都必须释放一个在途槽位；只有 accepted
        // 影响统计和限频日志，不影响槽位回收。
        shared = entry->outgoingShared.take(messageId);

        if (!entry->outgoingWakeScheduled && entry->outgoingAccepting &&
            !entry->outgoingStopping && entry->socket != nullptr &&
            entry->socket->state() == QLocalSocket::ConnectedState) {
            for (const OutgoingMessage& candidate : entry->outgoingQueue) { ///< 当前检查的等待项。
                const TopicConfig config = effectiveTopicConfig(messageBusConfig_,
                                                                 candidate.topic); ///< 该候选主题的容量配置。
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

// 停止入口的顺序固定为：父到子闸门 -> MessageBus 单模块入口 -> 清等待队列。
void ProcessSupervisor::closeEntryIngress(Entry* entry)
{
    if (entry == nullptr)
        return;
    {
        QMutexLocker locker(&entry->outgoingMutex); ///< 关闭父到子发送闸门。
        entry->outgoingAccepting = false;
        entry->outgoingStopping = true;
    }
    if (entry->bridge != nullptr)
        messageBus_->beginModuleStop(entry->config.id, true);
    {
        QMutexLocker locker(&entry->outgoingMutex); ///< 清空等待队列并记账。
        // 闸门关闭后，这些项已经没有机会写入 Socket，只在第一次清理时计数。
        entry->outgoingDropped += static_cast<quint64>(entry->outgoingQueue.size());
        entry->outgoingQueue.clear();
        entry->outgoingWakeScheduled = false;
    }
}

// 建立单个 Entry 的停止 deadline；函数不等待 QProcess 或 Socket。
void ProcessSupervisor::beginPrepareForExit()
{
    if (preparingExit_ || shuttingDown_) return;
    preparingExit_ = true;
    for (Entry* entry : entries_) {
        // Application exit takes over an unfinished manual prepare operation.
        // If exit is cancelled it must not unexpectedly restart that module later.
        if (!entry->prepareId.isEmpty() && entry->preparePurpose != StopPurpose::None) {
            const bool restart = entry->preparePurpose == StopPurpose::ManualRestart;
            entry->preparePurpose = StopPurpose::None;
            entry->pendingStart = false;
            setOperationBusy(entry, false);
            if (restart) emit restartFinished(entry->config.id, false, QStringLiteral("Cancelled by application exit"));
        }
        if (entry->phase == LifecyclePhase::Running && entry->prepareId.isEmpty())
            beginPrepareEntry(entry, StopPurpose::None);
    }
}

QStringList ProcessSupervisor::pendingExitModules() const
{
    QStringList pending;
    for (const Entry* entry : entries_) {
        if (!entry->process || entry->process->state() == QProcess::NotRunning) continue;
        if (entry->phase != LifecyclePhase::Running || !entry->exitReady)
            pending.append(entry->config.id);
    }
    return pending;
}

bool ProcessSupervisor::isReadyForExit() const
{
    return preparingExit_ && pendingExitModules().isEmpty();
}

void ProcessSupervisor::cancelPrepareEntry(Entry* entry)
{
    const QString id = entry->prepareId;
    entry->prepareId.clear();
    entry->exitReady = false;
    entry->preparePurpose = StopPurpose::None;
    if (!id.isEmpty() && entry->phase == LifecyclePhase::Running) {
        QJsonObject frame;
        frame.insert(QStringLiteral("type"), QStringLiteral("cancelPrepareExit"));
        frame.insert(QStringLiteral("requestId"), id);
        if (!sendFrame(entry, frame)) handleFault(entry, QStringLiteral("Cancel exit preparation failed"));
    }
}

void ProcessSupervisor::cancelPrepareForExit()
{
    preparingExit_ = false;
    for (Entry* entry : entries_) {
        if (entry->preparePurpose == StopPurpose::None) cancelPrepareEntry(entry);
    }
}

void ProcessSupervisor::beginPrepareEntry(Entry* entry, StopPurpose purpose)
{
    if (entry->phase != LifecyclePhase::Running || !entry->prepareExitSupported) {
        if (purpose != StopPurpose::None) beginStopEntry(entry, purpose);
        else entry->exitReady = true; // Legacy process runtime has no prepare gate.
        return;
    }
    entry->preparePurpose = purpose;
    entry->prepareId = QUuid::createUuid().toString(QUuid::Id128);
    entry->exitReady = false;
    entry->prepareDeadline = QDeadlineTimer(30000);
    pollPrepareEntry(entry);
}

bool ProcessSupervisor::pollPrepareEntry(Entry* entry)
{
    const StopPurpose purpose = entry->preparePurpose;
    if (purpose != StopPurpose::None && !preparingExit_) {
        if (entry->prepareDeadline.hasExpired()) {
            cancelPrepareEntry(entry);
            entry->pendingStart = false;
            setOperationBusy(entry, false);
            const QString detail = QString::fromUtf8(u8"退出准备超过 30 秒，已取消操作；模块仍在运行。");
            emitState(entry, QStringLiteral("Running"), detail);
            if (purpose == StopPurpose::ManualRestart) emit restartFinished(entry->config.id, false, detail);
            else emit stopFinished(entry->config.id, false, detail);
            return true;
        }
        if (entry->exitReady) {
            beginStopEntry(entry, purpose);
            return false;
        }
    }
    if (!entry->exitReady) {
        QJsonObject frame;
        frame.insert(QStringLiteral("type"), QStringLiteral("prepareExit"));
        frame.insert(QStringLiteral("requestId"), entry->prepareId);
        if (!sendFrame(entry, frame)) {
            handleFault(entry, QStringLiteral("Exit preparation request failed"));
            return false;
        }
    }
    return true;
}

void ProcessSupervisor::beginStopEntry(Entry* entry, StopPurpose purpose)
{
    // 单个 Entry 的固定顺序是：关闭父/子消息入口 -> 建立 deadline -> 发送 stop ->
    // 等 ACK -> 等进程退出 -> 按目的决定 Stopped、Failed 或 Restarting。
    if (entry == nullptr)
        return;
    entry->prepareId.clear();
    entry->exitReady = false;
    entry->preparePurpose = StopPurpose::None;
    entry->stopPurpose = purpose;
    entry->stopAcknowledged = false;
    entry->stopEscalation = 0;
    // 先关闭两层消息入口并记账，再建立停止阶段和 deadline；stop 帧永远最后发送。
    closeEntryIngress(entry);
    entry->phase = LifecyclePhase::StopRequested;
    entry->stopDeadline = QDeadlineTimer(qMax(1, processConfig_.stopTimeoutMs));

    emitState(entry,
              purpose == StopPurpose::ManualRestart
                  ? QStringLiteral("Restarting") : QStringLiteral("Stopping"),
              QString());
    if (entry->process == nullptr ||
        entry->process->state() == QProcess::NotRunning) {
        finishEntryAfterProcessExit(entry, QString());
        return;
    }

    entry->phase = LifecyclePhase::WaitingStopAck;
    QJsonObject frame; ///< 停止请求控制帧。
    frame.insert(QStringLiteral("type"), kStop);
    if (!sendFrame(entry, frame)) {
        // 没有可用控制通道时不能等不存在的 ACK，直接推进整个进程的终止阶段。
        entry->phase = LifecyclePhase::WaitingProcessExit;
        entry->process->terminate();
        entry->stopEscalation = 1;
        entry->stopDeadline = QDeadlineTimer(250);
    }
}

// stopAcknowledged 是真实状态机输入：ACK 到达后不再停留在 WaitingStopAck。
void ProcessSupervisor::advanceStopEntry(Entry* entry)
{
    if (entry != nullptr &&
        entry->phase == LifecyclePhase::WaitingStopAck &&
        entry->stopAcknowledged) {
        entry->phase = LifecyclePhase::WaitingProcessExit;
        emitState(entry,
                  entry->stopPurpose == StopPurpose::ManualRestart
                      ? QStringLiteral("Restarting") : QStringLiteral("Stopping"),
                  QString::fromUtf8(u8"已收到 stopAck，等待子进程退出"));
    }
}

// QProcess 已退出后清理本轮资源，并按 StopPurpose 决定停止、失败或下一次启动。
void ProcessSupervisor::finishEntryAfterProcessExit(Entry* entry, const QString& detail)
{
    // QProcess 真正退出后才能销毁 Runtime。若 pendingStart 仍为 true，转入 RestartDelay；
    // 否则发布最终 Stopped/Failed 并清除 operationBusy，等待方由状态信号唤醒。
    if (entry == nullptr)
        return;
    entry->prepareId.clear();
    entry->exitReady = false;
    entry->preparePurpose = StopPurpose::None;
    const StopPurpose purpose = entry->stopPurpose; ///< 本次停止的目的。
    const bool shouldRestart = entry->pendingStart && !shuttingDown_ &&
        (purpose == StopPurpose::ManualRestart || purpose == StopPurpose::AutoRestart); ///< 是否应排队启动下一代进程。
    const QString finalDetail = detail.isEmpty() ? entry->lastError : detail; ///< 最终对外发布的详细原因。
    destroyRuntime(entry);

    if (shouldRestart) {
        entry->phase = LifecyclePhase::RestartDelay;
        entry->restartAtMs = QDateTime::currentMSecsSinceEpoch() +
            (purpose == StopPurpose::AutoRestart
                 ? qMax(0, processConfig_.restartDelayMs) : 0);
        emitState(entry,
                  QStringLiteral("Restarting"),
                  purpose == StopPurpose::AutoRestart
                      ? QString::fromUtf8(u8"已排队自动重启")
                      : QString::fromUtf8(u8"已排队手动重启"));
        return;
    }

    entry->pendingStart = false;
    entry->restartAtMs = 0;
    entry->stopPurpose = StopPurpose::None;
    if (!finalDetail.isEmpty() && purpose != StopPurpose::Stop &&
        purpose != StopPurpose::Shutdown) {
        entry->phase = LifecyclePhase::Failed;
        emitState(entry, QStringLiteral("Failed"), finalDetail);
    } else {
        entry->phase = LifecyclePhase::Stopped;
        emitState(entry, QStringLiteral("Stopped"), finalDetail);
    }
    setOperationBusy(entry, false);
    if (purpose == StopPurpose::Stop)
        emit stopFinished(entry->config.id, finalDetail.isEmpty(), finalDetail);
}

void ProcessSupervisor::setOperationBusy(Entry* entry, bool busy)
{
    if (entry == nullptr || entry->operationBusy == busy)
        return;
    entry->operationBusy = busy;
    emit operationBusyChanged(entry->config.id, busy);
}

// 初始启动批次只在每个模块第一次达到 Running 或 Failed 后结算一次。
void ProcessSupervisor::settleStartupEntry(Entry* entry, const QString& error)
{
    if (entry == nullptr || !startupPendingModules_.remove(entry->config.id))
        return;
    if (!error.isEmpty())
        startupErrors_.append(QStringLiteral("%1: %2").arg(entry->config.id, error));
    scheduleStartupBatchFinished();
}

void ProcessSupervisor::scheduleStartupBatchFinished()
{
    if (!startupBatchActive_ || !startupPendingModules_.isEmpty() ||
        startupBatchSignalScheduled_)
        return;
    startupBatchSignalScheduled_ = true;
    QTimer::singleShot(0, this, [this]() {
        startupBatchSignalScheduled_ = false;
        if (!startupBatchActive_ || !startupPendingModules_.isEmpty())
            return;
        startupBatchActive_ = false;
        emit startupBatchFinished(startupErrors_);
    });
}

bool ProcessSupervisor::isStoppingPhase(LifecyclePhase phase) const
{
    return phase == LifecyclePhase::StopRequested ||
           phase == LifecyclePhase::WaitingStopAck ||
           phase == LifecyclePhase::WaitingProcessExit;
}

bool ProcessSupervisor::isRegisteredPhase(LifecyclePhase phase) const
{
    return phase == LifecyclePhase::WaitingStarted ||
           phase == LifecyclePhase::Running ||
           isStoppingPhase(phase);
}

// 防重复地标记失败、销毁旧运行时，并按时间窗口决定是否自动重启。
void ProcessSupervisor::handleFault(Entry* entry, const QString& detail)
{
    if (entry == nullptr || shuttingDown_ ||
        (entry->phase == LifecyclePhase::Failed && entry->process == nullptr))
        return;
    if (entry->stopPurpose == StopPurpose::Stop ||
        entry->stopPurpose == StopPurpose::Shutdown)
        return;

    // A crash during manual preparation still completes that operation as failed.
    if (entry->preparePurpose == StopPurpose::ManualRestart)
        entry->stopPurpose = StopPurpose::ManualRestart;
    const bool stopPreparationFailed = entry->preparePurpose == StopPurpose::Stop;
    if (stopPreparationFailed)
        entry->stopPurpose = StopPurpose::Stop;

    entry->lastError = detail;
    closeEntryIngress(entry);
    settleStartupEntry(entry, detail);
    emitState(entry, QStringLiteral("Failed"), detail);

    const bool manualRestartFailed = entry->stopPurpose == StopPurpose::ManualRestart; ///< 本次故障是否发生在手动重启过程中。
    bool autoRestart = false; ///< 是否应安排一次自动重启。
    if (!manualRestartFailed && !stopPreparationFailed && entry->config.enabled) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch(); ///< 当前毫秒时间戳，用于判断重启窗口。
        if (entry->restartWindowStartMs == 0 ||
            now - entry->restartWindowStartMs >= qMax(1, processConfig_.restartWindowMs)) {
            entry->restartWindowStartMs = now;
            entry->restartCount = 0;
        }
        if (entry->restartCount < qMax(0, processConfig_.maxRestartCount)) {
            ++entry->restartCount;
            autoRestart = true;
        }
    }
    entry->pendingStart = autoRestart;
    if (autoRestart)
        entry->stopPurpose = StopPurpose::AutoRestart;

    if (entry->process != nullptr &&
        entry->process->state() != QProcess::NotRunning) {
        entry->phase = LifecyclePhase::WaitingProcessExit;
        entry->stopEscalation = 2;
        entry->stopDeadline = QDeadlineTimer(1000);
        // 故障时结束完整 QProcess；不会对其中某条 QThread 调用 terminate()。
        entry->process->kill();
    } else {
        finishEntryAfterProcessExit(entry, detail);
    }
    if (manualRestartFailed) {
        entry->pendingStart = false;
        emit restartFinished(entry->config.id, false, detail);
        setOperationBusy(entry, false);
    } else if (!autoRestart) {
        emit moduleFault(entry->config.id, detail);
    }
}

// 统一释放桥接器、Socket、Server、QProcess、队列状态和共享内存。
void ProcessSupervisor::destroyRuntime(Entry* entry)
{
    if (entry == nullptr)
        return;
    {
        QMutexLocker locker(&entry->outgoingMutex); ///< 清理发送队列并记账。
        // 故障、断开、重启和正常停止共用这一条清理路径。先禁止生产者，
        // 再对等待/在途项各记一次账；容器清空后重复进入不会重复累计。
        entry->outgoingAccepting = false;
        entry->outgoingStopping = true;
        entry->outgoingDropped += static_cast<quint64>(entry->outgoingQueue.size());
        entry->outgoingAbandoned +=
            static_cast<quint64>(entry->outgoingMessageTopics.size());
        entry->outgoingQueue.clear();
        entry->outgoingInFlightByTopic.clear();
        entry->outgoingMessageTopics.clear();
        entry->outgoingInFlightCount = 0;
        entry->outgoingWakeScheduled = false;
    }
    ProcessBridge* bridge = entry->bridge; ///< 待释放的桥接器。
    QLocalSocket* socket = entry->socket; ///< 待释放的本地套接字。
    QLocalServer* server = entry->server; ///< 待释放的本地服务器。
    QProcess* process = entry->process; ///< 待释放的子进程句柄。
    entry->bridge = nullptr;
    entry->socket = nullptr;
    entry->server = nullptr;
    entry->process = nullptr;

    if (bridge != nullptr) {
        messageBus_->setModuleRunning(entry->config.id, false);
        if (messageBus_->unregisterModule(entry->config.id, false)) {
            delete bridge;
        } else {
            // 极端情况下桥接回调仍在执行；保留 endpoint，绝不删除其借用对象。
            Logger::instance().log(
                LogLevel::Error,
                entry->config.id,
                QString::fromUtf8(u8"进程桥接消息线程未停止，已隔离 endpoint"));
        }
    }
    if (socket != nullptr) {
        socket->disconnect(this);
        socket->disconnectFromServer();
        socket->close();
        socket->deleteLater();
    }
    if (server != nullptr) {
        const QString name = entry->serverName; ///< 待从系统中移除的服务器名。
        server->disconnect(this);
        server->close();
        server->deleteLater();
        QLocalServer::removeServer(name);
    }
    if (process != nullptr) {
        process->disconnect(this);
        // 正常路径只会清理已经退出的 QProcess。兜底 kill 作用于完整子进程，
        // 但这里不再同步等待，避免普通重启重新引入 GUI 阻塞。
        if (process->state() != QProcess::NotRunning)
            process->kill();
        process->deleteLater();
    }
    clearOutgoingShared(entry);
    entry->inputBuffer.clear();
    entry->stopAcknowledged = false;
    if (entry->generation > 0 &&
        entry->outgoingSummaryGeneration != entry->generation) {
        ProcessQueueStats stats; ///< 本代结束时的发送队列统计快照。
        {
            QMutexLocker locker(&entry->outgoingMutex); ///< 读取并标记已汇总的代次。
            entry->outgoingSummaryGeneration = entry->generation;
            stats.dropped = entry->outgoingDropped;
            stats.rejected = entry->outgoingRejected;
            stats.abandoned = entry->outgoingAbandoned;
        }
        Logger::instance().log(
            stats.dropped == 0 && stats.rejected == 0 && stats.abandoned == 0
                ? LogLevel::Info : LogLevel::Warning,
            entry->config.id,
            QString::fromUtf8(
                u8"父到子发送队列汇总：dropped=%1，rejected=%2，abandoned=%3")
                .arg(stats.dropped)
                .arg(stats.rejected)
                .arg(stats.abandoned));
    }
}

// 收集锁内共享段指针，锁外 detach/delete，缩短队列互斥锁持有时间。
void ProcessSupervisor::clearOutgoingShared(Entry* entry)
{
    QList<QSharedMemory*> segments; ///< 待释放的共享段列表。
    {
        QMutexLocker locker(&entry->outgoingMutex); ///< 取出并清空共享段映射。
        segments = entry->outgoingShared.values();
        entry->outgoingShared.clear();
    }
    for (QSharedMemory* shared : segments) { ///< 当前待释放的共享段。
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
