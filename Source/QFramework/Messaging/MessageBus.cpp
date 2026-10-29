// 【阅读导引】按主题把消息投递给订阅模块。
// 阅读顺序：registerModule 登记端点和主题；publishFromModule/publishSharedFromModule 检查发布资格；ModuleQueue 串行回调订阅者。
// 需要分清：Reliable 满时拒绝新消息；Latest 可以淘汰同主题旧等待项。publish 的 true 是准入结果，不是业务执行成功；停止超时的端点不能立即释放。
// 这是执行实现：按入口调用和信号回调追踪，不要按文件行号把所有函数理解成连续执行。

// 文件职责：实现进程内消息总线的注册、路由、容量控制和有界停止。
// publish() 在发布线程只做轻量校验并把不可变 QByteArray/共享载荷放入订阅者队列；
// 每个订阅模块有自己的投递线程和停止状态。Latest 只替换同主题最旧项，Reliable 满时拒绝新项，
// 两者都记录统计和限频日志。stopQueues() 会唤醒等待中的队列、等待 ACK/回调或超时，不强删运行对象。
#include "MessageBus.h"

#include <QDeadlineTimer>
#include <QDateTime>
#include <QMutexLocker>
#include <QQueue>
#include <QSet>
#include <QThread>
#include <QWaitCondition>

#include <exception>

#include "Logger.h"

// 本文件的两个层次：ModuleQueue 负责一个模块的线程安全输入队列，
// MessageBus 负责注册、权限检查、广播和全局关闭顺序。
//
// 发布链路是：publishFromModule()/publishSharedFromModule() 在 mutex_ 内校验
// accepting、发送权限、主题大小 -> 逐个调用订阅者 ModuleQueue::enqueue() ->
// enqueue 只短暂持锁并 wakeOne -> run() 锁外调用 endpoint->onMessage()。
// 这样业务回调可以再次 publish，不会形成总线自锁；停止时条件变量也会被唤醒。

namespace qframework
{
namespace
{
struct QueuedMessage
{
    // 发送者 ID 随主题一起传给订阅模块；payload 由所有订阅队列共享，
    // 队列覆盖、拒绝、停止清空和回调结束都会通过引用计数自动释放所有权。
    QString topic; ///< 消息主题，用于订阅匹配与容量统计。
    QString senderModuleId; ///< 发布者模块 ID，供订阅方区分来源。
    MessagePayload payload; ///< 共享不可变载荷，多个订阅队列共用同一份数据。
};

// 一个模块对应一个消费者线程和一条有界输入队列。
// 该隔离保证慢订阅者只耗尽自己的槽位，不会阻塞其他模块的 onMessage()。
class ModuleQueue : public QThread
{
public:
    // paused=true 用于启动阶段：允许先排队，但要等所有模块注册后再消费。
    /// @brief 为一个模块创建专属消费者队列。
    /// @param moduleId 队列所属模块 ID，用于日志标识。
    /// @param endpoint 模块端点借用指针，回调时调用其 onMessage。
    /// @param paused 是否先暂停消费，启动阶段传 true。
    /// @return 无（构造函数不返回值）。
    ModuleQueue(const QString& moduleId, ModuleEndpoint* endpoint, bool paused)
        : moduleId_(moduleId),
          endpoint_(endpoint),
          accepting_(true),
          paused_(paused),
          stopping_(false)
    {
    }

    // 按主题容量入队：Reliable 满时拒绝，Latest 满时删除同主题最旧等待项。
    // 注意容量按 topic 分别计算，不是整个模块所有主题共用一个数字；因此视频 Latest
    // 不会占掉 SQL Reliable 的槽位。payload 是共享不可变值，覆盖/清空后自动释放。
    /// @brief 按主题容量策略入队一条消息，必要时拒绝或淘汰旧项。
    /// @param message 待投递的消息值对象。
    /// @param config 该主题生效的容量与策略配置。
    /// @return 消息被接受返回 true；Reliable 队列已满或入口已关闭返回 false。
    bool enqueue(const QueuedMessage& message, const TopicConfig& config)
    {
        // 生产者可能来自任意线程，但这里始终只复制不可变 Qt 值并在短锁内修改队列；
        // 真正的 ModuleEndpoint::onMessage 在 run() 的锁外执行。
        QMutexLocker locker(&mutex_);
        // 生产者只在锁内修改队列和统计，消费者取出副本后才调用业务回调。
        if (!accepting_) { ///< 入口已关闭：拒绝并计入 rejected。
            ++stats_.rejected;
            return false;
        }

        int topicCount = 0; ///< 队列中与当前消息同主题的待处理条数。
        int oldestTopicIndex = -1; ///< 同主题中最旧一条的下标，-1 表示不存在。
        for (int i = 0; i < queue_.size(); ++i) { ///< i：遍历队列以统计同主题条数。
            if (queue_.at(i).topic == message.topic) {
                if (oldestTopicIndex < 0)
                    oldestTopicIndex = i;
                ++topicCount;
            }
        }
        if (topicCount >= config.queueCapacity) { ///< 同主题容量已满：按策略拒绝或淘汰。
            if (config.policy == QueuePolicy::Reliable) {
                // Reliable 满时保留旧消息并拒绝当前消息。
                ++stats_.rejected;
                return false;
            }
            if (oldestTopicIndex >= 0) {
                // Latest 只删除相同主题最旧的等待项，其他主题顺序不变。
                queue_.removeAt(oldestTopicIndex);
                ++stats_.dropped;
            }
        }

        queue_.enqueue(message);
        available_.wakeOne();
        return true;
    }

    // 暂停/恢复消费者，不改变已经排队的消息。
    /// @brief 暂停或恢复消费，已排队消息保持不动。
    /// @param paused true 表示暂停消费，false 表示恢复并唤醒线程。
    /// @return 无。
    void setPaused(bool paused)
    {
        QMutexLocker locker(&mutex_);
        // 启动阶段暂停只影响消费，不影响已注册的生产者入队。
        paused_ = paused;
        if (!paused_)
            available_.wakeOne();
    }

    // 切断生产入口并唤醒线程；调用方可选择立即丢弃或尽量排空等待项。
    /// @brief 关闭入队入口并唤醒消费者，可选择立即丢弃待处理消息。
    /// @param discardPending 为 true 时清空尚未消费的等待项。
    /// @return 无。
    void beginStop(bool discardPending)
    {
        QMutexLocker locker(&mutex_);
        // 先禁止新消息，再唤醒 run()；discardPending 只决定是否清掉尚未消费的项。
        accepting_ = false;
        paused_ = false;
        stopping_ = true;
        if (discardPending) { ///< 需要立即丢弃：清空队列并把条数计入 dropped。
            stats_.dropped += static_cast<quint64>(queue_.size());
            queue_.clear();
        }
        available_.wakeOne();
    }

    // 只做一次有界等待；超时表示回调仍在执行，调用方不得删除本 QThread，也不能
    // 为了“快点退出”调用 terminate。调用方应把该 Registration 留在 quarantine。
    /// @brief 在有界时间内等待消费者线程结束。
    /// @param timeoutMs 最长等待毫秒数，非正数表示不等待直接判定。
    /// @return 线程已停止返回 Stopped，超时返回 TimedOut。
    ModuleQueueStopResult finishStop(int timeoutMs)
    {
        if (isFinished() ||
            (timeoutMs > 0 && wait(static_cast<unsigned long>(timeoutMs)))) {
            return ModuleQueueStopResult::Stopped;
        }

        {
            QMutexLocker locker(&mutex_);
            // 只能丢弃尚未开始执行的等待项；锁外正在运行的 onMessage 仍被隔离。
            stats_.dropped += static_cast<quint64>(queue_.size());
            queue_.clear();
            available_.wakeOne();
        }
        return ModuleQueueStopResult::TimedOut;
    }

    // 返回统计快照，避免调用方持有队列内部锁。
    /// @brief 返回当前队列统计快照。
    /// @return delivered/rejected/dropped 计数的值副本。
    ModuleQueueStats stats() const
    {
        QMutexLocker locker(&mutex_);
        return stats_;
    }

protected:
    // 串行取消息并在锁外调用模块回调，停止且队列清空后退出。
    /// @brief 消费者主循环：串行取消息并在锁外调用模块 onMessage。
    /// @return 无；停止且队列清空后退出。
    void run() override
    {
        // 每个模块独占一个 QThread，消息按入队顺序逐条消费；业务回调异常会被捕获，
        // 不会让总线线程直接崩溃，也不会阻止后续模块消息。
        // 一个 ModuleQueue 一个线程，保证同一模块的 onMessage 串行且顺序稳定。
        for (;;) {
            QueuedMessage message; ///< 本轮取出的消息副本，锁外交给业务回调。
            {
                QMutexLocker locker(&mutex_);
                // 条件变量同时等待“有消息、解除暂停或停止”；100 ms 是漏唤醒时的
                // 有界兜底，正常路径仍由 publish/resume/beginStop 的 wakeOne() 即时唤醒。
                while ((queue_.isEmpty() || paused_) && !stopping_)
                    available_.wait(&mutex_, 100);
                if (queue_.isEmpty() && stopping_)
                    break;
                if (paused_)
                    continue;
                message = queue_.dequeue();
            }

            try {
                // 锁已释放，业务回调可以再次 publish，不会形成队列自锁。
                endpoint_->onMessage(message.topic,
                                     message.senderModuleId,
                                     *message.payload);
            } catch (const std::exception& exception) {
                Logger::instance().log(
                    LogLevel::Error,
                    moduleId_,
                    QString::fromUtf8(u8"onMessage 异常：%1")
                        .arg(QString::fromUtf8(exception.what())));
            } catch (...) {
                Logger::instance().log(
                    LogLevel::Error,
                    moduleId_,
                    QString::fromUtf8(u8"onMessage 未知异常"));
            }
            QMutexLocker locker(&mutex_);
            ++stats_.delivered;
        }
    }

private:
    // endpoint_ 是借用指针，生命周期由 Registration 和注销顺序保证。
    QString moduleId_; ///< 队列所属模块 ID，仅用于日志与诊断。
    ModuleEndpoint* endpoint_; ///< 模块端点借用指针，不拥有其生命周期。
    mutable QMutex mutex_; ///< 保护队列、统计与状态标志。
    QWaitCondition available_; ///< 有新消息或暂停/停止状态变化时唤醒消费者。
    QQueue<QueuedMessage> queue_; ///< 待消费消息队列，受 mutex_ 保护。
    bool accepting_; ///< 是否仍接受新消息，停止后置为 false。
    bool paused_; ///< 是否暂停消费，启动阶段为 true。
    bool stopping_; ///< 是否已请求停止，队列清空后退出 run()。
    ModuleQueueStats stats_; ///< 投递/拒绝/丢弃计数，锁内更新。
};
}

struct MessageBus::Registration
{
    // Registration 是总线对一个模块的所有权记录；endpoint 由外部创建，
    // queue 和本结构由 MessageBus 创建并在 unregister 时释放。
    ModuleEndpoint* endpoint = nullptr; ///< 模块端点借用指针，由插件/运行时拥有。
    ModuleQueue* queue = nullptr; ///< 本模块专属消费者线程，由 Registration 拥有。
    QSet<QString> publishedTopics; ///< 注册时冻结的可发布主题集合。
    QSet<QString> subscribedTopics; ///< 注册时冻结的订阅主题集合。
    bool publishingEnabled = false; ///< 当前是否允许该模块发布消息。
    bool quarantined = false; ///< 停止超时需保留，禁止立即删除其线程与端点。
};

// 保存配置快照；初始允许注册/发布，但消息投递保持暂停直到框架统一放行。
/// @brief 构造消息总线，保存配置并处于“可注册但暂停投递”的初始状态。
/// @param config 消息总线配置快照，含默认容量、载荷上限与停止预算。
/// @return 无（构造函数不返回值）。
MessageBus::MessageBus(const MessageBusConfig& config)
    : config_(config),
      accepting_(true),
      deliveryEnabled_(false),
      stopQueuesRequested_(false)
{
}

/// @brief 析构兜底：停止全部队列并回收未超时的模块注册。
/// @return 无（析构函数不返回值）。
MessageBus::~MessageBus()
{
    // 正常 FrameworkRuntime 会在析构前完成唯一一次停止；这里仅做兜底。
    beginShutdown();
    const MessageBusStopReport report = stopQueues(config_.shutdownDrainTimeoutMs); ///< 兜底停止结果，用于排除超时模块。
    const QStringList ids = moduleIds(); ///< 当前仍注册的模块 ID 快照。
    for (const QString& id : ids) { ///< id：逐个回收未超时的模块注册。
        if (!report.timedOutModuleIds.contains(id))
            unregisterModule(id, false);
    }
}

// 校验模块 ID 与主题声明，创建专属 ModuleQueue，并把 ModuleEndpoint 绑定到总线。
/// @brief 注册一个模块：冻结主题声明、创建专属队列并绑定端点。
/// @param moduleId 全局唯一的模块 ID。
/// @param endpoint 模块端点，不能为空。
/// @param errorMessage 可选的失败原因输出参数，可为空。
/// @return 注册成功返回 true；参数无效、主题声明非法或 ID 重复返回 false。
bool MessageBus::registerModule(const QString& moduleId,
                                ModuleEndpoint* endpoint,
                                QString* errorMessage)
{
    // 注册一次性冻结模块的发布/订阅主题，并创建专属队列线程；deliveryEnabled_ 为 false
    // 时线程先暂停消费，等框架把所有模块注册完再统一放行。
    // 主题声明在注册时冻结，后续 publish 不再接受动态新增主题。
    if (moduleId.trimmed().isEmpty() || endpoint == nullptr) { ///< 模块 ID 为空或端点为空：拒绝注册。
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"模块 ID 或端点为空");
        return false;
    }

    const QStringList publishedList = endpoint->publishedTopics(); ///< 端点声明的发布主题原始列表。
    const QStringList subscribedList = endpoint->subscribedTopics(); ///< 端点声明的订阅主题原始列表。
    const QSet<QString> published(publishedList.cbegin(), publishedList.cend()); ///< 去重后的发布主题集合。
    const QSet<QString> subscribed(subscribedList.cbegin(), subscribedList.cend()); ///< 去重后的订阅主题集合。
    if (published.contains(QString()) || subscribed.contains(QString()) ||
        published.size() != publishedList.size() || subscribed.size() != subscribedList.size()) { ///< 存在空主题或重复声明：拒绝注册。
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"模块 %1 的主题声明为空或重复")
                .arg(moduleId);
        return false;
    }

    QMutexLocker locker(&mutex_);
    if (!accepting_) { ///< 总线正在停止：拒绝新模块注册。
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"消息总线正在停止，拒绝新模块注册");
        return false;
    }
    if (modules_.contains(moduleId)) { ///< 模块 ID 已存在：拒绝重复注册。
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"模块 ID 重复：%1").arg(moduleId);
        return false;
    }

    Registration* registration = new Registration; ///< 本模块的所有权记录，后续由总线管理。
    registration->endpoint = endpoint;
    registration->publishedTopics = published;
    registration->subscribedTopics = subscribed;
    registration->queue = new ModuleQueue(moduleId, endpoint, !deliveryEnabled_);
    // deliveryEnabled_ 为 false 时先启动线程但暂停消费，等待框架统一放行。
    modules_.insert(moduleId, registration);
    endpoint->bindHost(moduleId, this);
    registration->queue->start();
    return true;
}

// 同步 Registration 和 ModuleEndpoint 的发布开关；未知模块返回 false。
/// @brief 设置模块的发布权限，并同步到端点。
/// @param moduleId 目标模块 ID。
/// @param running 是否允许发布；总线停止时一律视为不允许。
/// @return 模块存在返回 true，未知模块返回 false。
bool MessageBus::setModuleRunning(const QString& moduleId, bool running)
{
    QMutexLocker locker(&mutex_);
    Registration* registration = modules_.value(moduleId, nullptr); ///< 目标模块的注册记录。
    if (registration == nullptr)
        return false;
    // running 由生命周期协调器设置；关闭中的总线即使收到 true 也拒绝发布。
    registration->publishingEnabled = running && accepting_;
    registration->endpoint->setRunning(registration->publishingEnabled);
    return true;
}

// 单模块停止先关闭入口，不等待其消息线程；ProcessSupervisor 必须在发送 stop 前调用。
/// @brief 关闭单个模块的发布入口与消息入口，但不等待其线程结束。
/// @param moduleId 目标模块 ID。
/// @param discardPendingMessages 为 true 时丢弃尚未消费的等待项。
/// @return 模块存在返回 true，未知模块返回 false。
bool MessageBus::beginModuleStop(const QString& moduleId, bool discardPendingMessages)
{
    QMutexLocker locker(&mutex_);
    Registration* registration = modules_.value(moduleId, nullptr); ///< 目标模块的注册记录。
    if (registration == nullptr)
        return false;
    registration->publishingEnabled = false;
    registration->endpoint->setRunning(false);
    registration->queue->beginStop(discardPendingMessages);
    return true;
}

// 一次性暂停或恢复所有模块消费者，主要用于确定性的启动阶段。
/// @brief 统一暂停或恢复所有模块的消息消费。
/// @param enabled true 表示放行消费，false 表示暂停。
/// @return 无；队列内容保持不变。
void MessageBus::setDeliveryEnabled(bool enabled)
{
    QMutexLocker locker(&mutex_);
    // 只改变消费暂停标记，不清空队列；启动阶段积累的消息仍可按顺序处理。
    deliveryEnabled_ = enabled;
    for (Registration* registration : modules_) ///< registration：逐个同步暂停状态。
        registration->queue->setPaused(!enabled);
}

// 全局关闭发布入口，并解除暂停以便在停止预算内消费已有消息。
/// @brief 全局关闭发布入口，并解除暂停以排空已有消息。
/// @return 无。
void MessageBus::beginShutdown()
{
    QMutexLocker locker(&mutex_);
    // 关闭先切断生产入口，再解除暂停，让已有消息有机会在预算内排空。
    accepting_ = false;
    deliveryEnabled_ = true;
    for (Registration* registration : modules_) { ///< registration：逐个关闭入口并解除暂停。
        registration->publishingEnabled = false;
        registration->endpoint->setRunning(false);
        registration->queue->setPaused(false);
    }
}

// 在一份总 deadline 内回收全部模块线程；超时队列留在注册表中 quarantine。
/// @brief 在共享的总 deadline 内停止全部模块队列，并标记超时项为隔离。
/// @param drainTimeoutMs 全局排空预算毫秒数。
/// @return 停止结果报告，区分已停止与超时的模块 ID。
MessageBusStopReport MessageBus::stopQueues(int drainTimeoutMs)
{
    // 用一份总 deadline 分配给所有模块，先停止生产再尽量排空可靠消息；某一模块超时
    // 会进入 quarantine，调用方不能删除仍可能执行 onMessage 的线程对象。
    QVector<QPair<QString, ModuleQueue*>> queues; ///< 模块 ID 与其队列的配对快照，锁外逐个等待。
    {
        QMutexLocker locker(&mutex_);
        for (Registration* registration : modules_) { ///< registration：先统一关闭入队入口。
            if (!stopQueuesRequested_)
                registration->queue->beginStop(false);
        }
        for (QHash<QString, Registration*>::const_iterator iterator = modules_.constBegin();
             iterator != modules_.constEnd(); ++iterator) { ///< iterator：遍历注册表收集队列快照。
            queues.append(qMakePair(iterator.key(), iterator.value()->queue));
        }
        stopQueuesRequested_ = true;
    }

    MessageBusStopReport report; ///< 停止结果，逐模块累加已停止/超时 ID。
    QDeadlineTimer deadline(qMax(1, drainTimeoutMs)); ///< 所有模块共享的总超时截止时间。
    for (const QPair<QString, ModuleQueue*>& item : queues) { ///< item：当前等待的模块与其队列。
        const int remaining = static_cast<int>(qMax<qint64>(0, deadline.remainingTime())); ///< 该模块可用的剩余毫秒数。
        const ModuleQueueStopResult result = item.second->finishStop(remaining); ///< 该模块的停止结果。
        if (result == ModuleQueueStopResult::Stopped)
            report.stoppedModuleIds.append(item.first);
        else
            report.timedOutModuleIds.append(item.first);

        QMutexLocker locker(&mutex_);
        Registration* registration = modules_.value(item.first, nullptr); ///< 该模块的注册记录，用于写入隔离标记。
        if (registration != nullptr)
            registration->quarantined = result == ModuleQueueStopResult::TimedOut;
    }
    return report;
}

// 从路由表移除模块、停止专属线程、解绑宿主并释放 Registration。
/// @brief 注销一个模块：停止其队列、解绑端点并释放注册记录。
/// @param moduleId 目标模块 ID。
/// @param drainPendingMessages 为 true 时先排空待处理消息再停止。
/// @return 成功注销返回 true；模块不存在或线程超时（已隔离）返回 false。
bool MessageBus::unregisterModule(const QString& moduleId, bool drainPendingMessages)
{
    Registration* registration = nullptr; ///< 目标模块的注册记录。
    {
        QMutexLocker locker(&mutex_);
        registration = modules_.value(moduleId, nullptr);
    }
    if (registration == nullptr)
        return false;

    // 先关闭入口，但只有线程确定停止后才从注册表摘除并删除对象。
    registration->publishingEnabled = false;
    registration->endpoint->setRunning(false);
    if (registration->queue->isRunning()) { ///< 线程仍在运行：先有界等待其停止。
        registration->queue->beginStop(!drainPendingMessages);
        if (registration->queue->finishStop(config_.shutdownDrainTimeoutMs) ==
            ModuleQueueStopResult::TimedOut) { ///< 超时：标记隔离并保留对象，避免访问失效线程。
            QMutexLocker locker(&mutex_);
            registration->quarantined = true;
            return false;
        }
    }
    {
        QMutexLocker locker(&mutex_);
        if (modules_.value(moduleId, nullptr) != registration)
            return false;
        modules_.remove(moduleId);
    }
    registration->endpoint->bindHost(QString(), nullptr);
    delete registration->queue;
    delete registration;
    return true;
}

// 返回当前注册 ID 快照，顺序由 QHash 决定，不表示配置启动顺序。
/// @brief 返回当前已注册模块 ID 的快照。
/// @return 模块 ID 列表，顺序由哈希决定，不代表启动顺序。
QStringList MessageBus::moduleIds() const
{
    QMutexLocker locker(&mutex_);
    return modules_.keys();
}

// 返回指定模块队列统计；未知模块返回全零默认结构。
/// @brief 查询指定模块队列的投递统计。
/// @param moduleId 目标模块 ID。
/// @return 统计快照；未知模块返回全零默认值。
ModuleQueueStats MessageBus::queueStats(const QString& moduleId) const
{
    QMutexLocker locker(&mutex_);
    Registration* registration = modules_.value(moduleId, nullptr); ///< 目标模块的注册记录。
    return registration == nullptr ? ModuleQueueStats() : registration->queue->stats();
}

/// @brief 判断指定模块的消息队列是否已经停止。
/// @param moduleId 目标模块 ID。
/// @return 模块不存在或线程已结束返回 true，仍在运行返回 false。
bool MessageBus::isModuleQueueStopped(const QString& moduleId) const
{
    QMutexLocker locker(&mutex_);
    Registration* registration = modules_.value(moduleId, nullptr); ///< 目标模块的注册记录。
    return registration == nullptr || !registration->queue->isRunning();
}

/// @brief 列出停止超时、仍在运行且需隔离保留的模块 ID。
/// @return 已标记隔离且线程仍运行的模块 ID 列表。
QStringList MessageBus::quarantinedModuleIds() const
{
    QStringList result; ///< 隔离模块 ID 收集结果。
    QMutexLocker locker(&mutex_);
    for (QHash<QString, Registration*>::const_iterator iterator = modules_.constBegin();
         iterator != modules_.constEnd(); ++iterator) { ///< iterator：遍历注册表筛选隔离项。
        if (iterator.value()->quarantined && iterator.value()->queue->isRunning())
            result.append(iterator.key());
    }
    return result;
}

// 执行发布权限、主题大小和订阅路由检查，再把值对象复制进各订阅者队列。
/// @brief 发布一条字节数组消息，内部转换为共享载荷后走统一发布路径。
/// @param moduleId 发布者模块 ID。
/// @param topic 目标主题。
/// @param data 待发送的字节内容。
/// @return 全部订阅者都接受返回 true，否则返回 false。
bool MessageBus::publishFromModule(const QString& moduleId,
                                   const QString& topic,
                                   const QByteArray& data)
{
    // 兼容直接调用宿主旧接口的代码；统一包装后进入共享发布实现。
    return publishSharedFromModule(moduleId, topic, makeMessagePayload(data));
}

// 执行发布权限、主题大小和订阅路由检查，再共享同一个不可变载荷引用。
/// @brief 发布一条共享载荷消息，向所有订阅者队列入队。
/// @param moduleId 发布者模块 ID。
/// @param topic 目标主题。
/// @param payload 共享不可变载荷。
/// @return 全部订阅者都接受返回 true，否则返回 false。
bool MessageBus::publishSharedFromModule(const QString& moduleId,
                                         const QString& topic,
                                         const MessagePayload& payload)
{
    // 发布的所有权限、主题和大小检查都在总线锁内完成，然后逐订阅者入队；
    // 某个订阅者满了只影响返回值，不回滚已经成功进入其他队列的副本。
    QMutexLocker locker(&mutex_);
    // 下面所有校验在同一把总线锁内完成，保证注册/关闭不会与广播交错。
    Registration* sender = modules_.value(moduleId, nullptr); ///< 发布者的注册记录。
    if (!accepting_ || sender == nullptr || !sender->publishingEnabled) { ///< 总线停止或发布者无权限：拒绝发布。
        logRejected(moduleId, QString::fromUtf8(u8"模块未运行或消息总线正在停止"));
        return false;
    }
    if (!sender->publishedTopics.contains(topic)) { ///< 主题未在注册时声明：拒绝发布。
        logRejected(moduleId, QString::fromUtf8(u8"未声明发布主题：%1").arg(topic));
        return false;
    }

    if (payload.isNull()) { ///< 共享载荷为空：拒绝发布。
        logRejected(moduleId, QString::fromUtf8(u8"共享消息载荷为空"));
        return false;
    }

    const TopicConfig effective = topicConfig(topic); ///< 该主题实际生效的容量与策略。
    if (payload->size() > effective.maxMessageBytes) { ///< 载荷超过主题上限：拒绝发布。
        logRejected(moduleId, QString::fromUtf8(u8"消息超过大小上限：%1").arg(topic));
        return false;
    }

    QueuedMessage message; ///< 要投递到各订阅队列的消息值对象。
    message.topic = topic;
    message.senderModuleId = moduleId;
    message.payload = payload;
    bool accepted = true; ///< 是否所有订阅者都接受，任一拒绝即置 false。
    // 每个订阅者独立入队；只要有一个订阅者拒绝，返回值就是 false，
    // 但其他订阅者已经成功接收的消息不会回滚。
    for (Registration* recipient : modules_) { ///< recipient：逐个筛选并投递的候选订阅者。
        if (!recipient->subscribedTopics.contains(topic))
            continue;
        if (!recipient->queue->enqueue(message, effective))
            accepted = false;
    }
    if (!accepted)
        logRejected(moduleId, QString::fromUtf8(u8"至少一个订阅者队列已满：%1").arg(topic));
    return accepted;
}

// 模块日志不进入消息主题，直接交给集中 Logger，避免形成递归消息链。
/// @brief 把模块日志直接转发给集中 Logger。
/// @param level 日志级别。
/// @param moduleId 记录来源模块 ID。
/// @param text 日志文本。
/// @return 无。
void MessageBus::logFromModule(LogLevel level,
                               const QString& moduleId,
                               const QString& text)
{
    Logger::instance().log(level, moduleId, text);
}

// 查询主题专用规则；未配置时把全局默认值组合成完整 TopicConfig。
/// @brief 取指定主题的生效配置，缺失时用全局默认值补齐。
/// @param topic 目标主题名。
/// @return 该主题的容量、载荷上限与策略。
TopicConfig MessageBus::topicConfig(const QString& topic) const
{
    // 主题没有专用配置时复制全局默认值，调用方随后可安全调整本地副本。
    if (config_.topics.contains(topic))
        return config_.topics.value(topic);
    TopicConfig result; ///< 由全局默认值组合出的完整主题配置。
    result.queueCapacity = config_.defaultQueueCapacity;
    result.maxMessageBytes = config_.maxMessageBytes;
    result.policy = config_.defaultPolicy;
    return result;
}

// 统一记录发布拒绝原因，保持调用点的错误文字和来源格式一致。
/// @brief 记录一次发布拒绝，并对同一模块的重复原因做限频。
/// @param moduleId 触发拒绝的模块 ID。
/// @param reason 拒绝原因文本。
/// @return 无；处于限频窗口内只累计次数不写日志。
void MessageBus::logRejected(const QString& moduleId, const QString& reason) const
{
    // 日志限频只针对 Warning 文本，不改变真正的 rejected 统计，也不改变 Reliable
    // 拒绝或 Latest 覆盖。窗口内首条立即记录，重复项只累计次数，下一条日志再附带
    // “此前 N 次相同拒绝已限频”，帮助初学者看出错误是否持续发生。
    constexpr qint64 kLogIntervalMs = 1000; ///< 同一拒绝原因的最小记录间隔。
    const QString key = moduleId + QChar(0x1f) + reason; ///< 模块 ID 与原因的限频键。
    const qint64 now = QDateTime::currentMSecsSinceEpoch(); ///< 当前时间戳，用于判断限频窗口。
    RejectionLogState& state = rejectionLogs_[key]; ///< 该限频键对应的累计状态。
    if (state.lastLoggedUtcMs > 0
        && now - state.lastLoggedUtcMs < kLogIntervalMs) { ///< 仍在限频窗口内：只累计抑制次数。
        ++state.suppressed;
        return;
    }
    QString text = reason; ///< 实际写入日志的文本，必要时附带被限频次数。
    if (state.suppressed > 0) {
        text += QString::fromUtf8(u8"（此前 %1 次相同拒绝已限频）")
                    .arg(state.suppressed);
        state.suppressed = 0;
    }
    state.lastLoggedUtcMs = now;
    Logger::instance().log(LogLevel::Warning, moduleId, text);
}
}
