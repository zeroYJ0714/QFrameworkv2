#include "MessageBus.h"

#include <QDeadlineTimer>
#include <QMutexLocker>
#include <QQueue>
#include <QSet>
#include <QThread>
#include <QWaitCondition>

#include <exception>

#include "Logger.h"

// 本文件的两个层次：ModuleQueue 负责一个模块的线程安全输入队列，
// MessageBus 负责注册、权限检查、广播和全局关闭顺序。

namespace qframework
{
namespace
{
struct QueuedMessage
{
    // 消息在总线内部的完整副本；发送者 ID 随数据一起传给订阅模块。
    QString topic;
    QString senderModuleId;
    QByteArray data;
};

// 一个模块对应一个消费者线程和一条有界输入队列。
// 该隔离保证慢订阅者只耗尽自己的槽位，不会阻塞其他模块的 onMessage()。
class ModuleQueue : public QThread
{
public:
    // paused=true 用于启动阶段：允许先排队，但要等所有模块注册后再消费。
    ModuleQueue(const QString& moduleId, ModuleEndpoint* endpoint, bool paused)
        : moduleId_(moduleId),
          endpoint_(endpoint),
          accepting_(true),
          paused_(paused),
          stopping_(false)
    {
    }

    // 按主题容量入队：Reliable 满时拒绝，Latest 满时删除同主题最旧等待项。
    bool enqueue(const QueuedMessage& message, const TopicConfig& config)
    {
        QMutexLocker locker(&mutex_);
        // 生产者只在锁内修改队列和统计，消费者取出副本后才调用业务回调。
        if (!accepting_) {
            ++stats_.rejected;
            return false;
        }

        int topicCount = 0;
        int oldestTopicIndex = -1;
        for (int i = 0; i < queue_.size(); ++i) {
            if (queue_.at(i).topic == message.topic) {
                if (oldestTopicIndex < 0)
                    oldestTopicIndex = i;
                ++topicCount;
            }
        }
        if (topicCount >= config.queueCapacity) {
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
    void setPaused(bool paused)
    {
        QMutexLocker locker(&mutex_);
        // 启动阶段暂停只影响消费，不影响已注册的生产者入队。
        paused_ = paused;
        if (!paused_)
            available_.wakeOne();
    }

    // 切断生产入口并唤醒线程；调用方可选择立即丢弃或尽量排空等待项。
    void beginStop(bool discardPending)
    {
        QMutexLocker locker(&mutex_);
        // 先禁止新消息，再唤醒 run()；discardPending 只决定是否清掉尚未消费的项。
        accepting_ = false;
        paused_ = false;
        stopping_ = true;
        if (discardPending) {
            stats_.dropped += static_cast<quint64>(queue_.size());
            queue_.clear();
        }
        available_.wakeOne();
    }

    // 只做一次有界等待；超时表示回调仍在执行，调用方不得删除本 QThread。
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
    ModuleQueueStats stats() const
    {
        QMutexLocker locker(&mutex_);
        return stats_;
    }

protected:
    // 串行取消息并在锁外调用模块回调，停止且队列清空后退出。
    void run() override
    {
        // 一个 ModuleQueue 一个线程，保证同一模块的 onMessage 串行且顺序稳定。
        for (;;) {
            QueuedMessage message;
            {
                QMutexLocker locker(&mutex_);
                // 条件变量同时等待“有消息、解除暂停或停止”；beginStop 会唤醒它。
                while ((queue_.isEmpty() || paused_) && !stopping_)
                    available_.wait(&mutex_);
                if (queue_.isEmpty() && stopping_)
                    break;
                if (paused_)
                    continue;
                message = queue_.dequeue();
            }

            try {
                // 锁已释放，业务回调可以再次 publish，不会形成队列自锁。
                endpoint_->onMessage(message.topic, message.senderModuleId, message.data);
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
    QString moduleId_;
    ModuleEndpoint* endpoint_;
    mutable QMutex mutex_;
    QWaitCondition available_;
    QQueue<QueuedMessage> queue_;
    bool accepting_;
    bool paused_;
    bool stopping_;
    ModuleQueueStats stats_;
};
}

struct MessageBus::Registration
{
    // Registration 是总线对一个模块的所有权记录；endpoint 由外部创建，
    // queue 和本结构由 MessageBus 创建并在 unregister 时释放。
    ModuleEndpoint* endpoint = nullptr;
    ModuleQueue* queue = nullptr;
    QSet<QString> publishedTopics;
    QSet<QString> subscribedTopics;
    bool publishingEnabled = false;
    bool quarantined = false;
};

// 保存配置快照；初始允许注册/发布，但消息投递保持暂停直到框架统一放行。
MessageBus::MessageBus(const MessageBusConfig& config)
    : config_(config),
      accepting_(true),
      deliveryEnabled_(false),
      stopQueuesRequested_(false)
{
}

MessageBus::~MessageBus()
{
    // 正常 FrameworkRuntime 会在析构前完成唯一一次停止；这里仅做兜底。
    beginShutdown();
    const MessageBusStopReport report = stopQueues(config_.shutdownDrainTimeoutMs);
    const QStringList ids = moduleIds();
    for (const QString& id : ids) {
        if (!report.timedOutModuleIds.contains(id))
            unregisterModule(id, false);
    }
}

// 校验模块 ID 与主题声明，创建专属 ModuleQueue，并把 ModuleEndpoint 绑定到总线。
bool MessageBus::registerModule(const QString& moduleId,
                                ModuleEndpoint* endpoint,
                                QString* errorMessage)
{
    // 主题声明在注册时冻结，后续 publish 不再接受动态新增主题。
    if (moduleId.trimmed().isEmpty() || endpoint == nullptr) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"模块 ID 或端点为空");
        return false;
    }

    const QStringList publishedList = endpoint->publishedTopics();
    const QStringList subscribedList = endpoint->subscribedTopics();
    const QSet<QString> published(publishedList.cbegin(), publishedList.cend());
    const QSet<QString> subscribed(subscribedList.cbegin(), subscribedList.cend());
    if (published.contains(QString()) || subscribed.contains(QString()) ||
        published.size() != publishedList.size() || subscribed.size() != subscribedList.size()) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"模块 %1 的主题声明为空或重复")
                .arg(moduleId);
        return false;
    }

    QMutexLocker locker(&mutex_);
    if (!accepting_) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"消息总线正在停止，拒绝新模块注册");
        return false;
    }
    if (modules_.contains(moduleId)) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"模块 ID 重复：%1").arg(moduleId);
        return false;
    }

    Registration* registration = new Registration;
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
bool MessageBus::setModuleRunning(const QString& moduleId, bool running)
{
    QMutexLocker locker(&mutex_);
    Registration* registration = modules_.value(moduleId, nullptr);
    if (registration == nullptr)
        return false;
    // running 由生命周期协调器设置；关闭中的总线即使收到 true 也拒绝发布。
    registration->publishingEnabled = running && accepting_;
    registration->endpoint->setRunning(registration->publishingEnabled);
    return true;
}

// 单模块停止先关闭入口，不等待其消息线程；ProcessSupervisor 必须在发送 stop 前调用。
bool MessageBus::beginModuleStop(const QString& moduleId, bool discardPendingMessages)
{
    QMutexLocker locker(&mutex_);
    Registration* registration = modules_.value(moduleId, nullptr);
    if (registration == nullptr)
        return false;
    registration->publishingEnabled = false;
    registration->endpoint->setRunning(false);
    registration->queue->beginStop(discardPendingMessages);
    return true;
}

// 一次性暂停或恢复所有模块消费者，主要用于确定性的启动阶段。
void MessageBus::setDeliveryEnabled(bool enabled)
{
    QMutexLocker locker(&mutex_);
    // 只改变消费暂停标记，不清空队列；启动阶段积累的消息仍可按顺序处理。
    deliveryEnabled_ = enabled;
    for (Registration* registration : modules_)
        registration->queue->setPaused(!enabled);
}

// 全局关闭发布入口，并解除暂停以便在停止预算内消费已有消息。
void MessageBus::beginShutdown()
{
    QMutexLocker locker(&mutex_);
    // 关闭先切断生产入口，再解除暂停，让已有消息有机会在预算内排空。
    accepting_ = false;
    deliveryEnabled_ = true;
    for (Registration* registration : modules_) {
        registration->publishingEnabled = false;
        registration->endpoint->setRunning(false);
        registration->queue->setPaused(false);
    }
}

// 在一份总 deadline 内回收全部模块线程；超时队列留在注册表中 quarantine。
MessageBusStopReport MessageBus::stopQueues(int drainTimeoutMs)
{
    QVector<QPair<QString, ModuleQueue*>> queues;
    {
        QMutexLocker locker(&mutex_);
        for (Registration* registration : modules_) {
            if (!stopQueuesRequested_)
                registration->queue->beginStop(false);
        }
        for (QHash<QString, Registration*>::const_iterator iterator = modules_.constBegin();
             iterator != modules_.constEnd(); ++iterator) {
            queues.append(qMakePair(iterator.key(), iterator.value()->queue));
        }
        stopQueuesRequested_ = true;
    }

    MessageBusStopReport report;
    QDeadlineTimer deadline(qMax(1, drainTimeoutMs));
    for (const QPair<QString, ModuleQueue*>& item : queues) {
        const int remaining = static_cast<int>(qMax<qint64>(0, deadline.remainingTime()));
        const ModuleQueueStopResult result = item.second->finishStop(remaining);
        if (result == ModuleQueueStopResult::Stopped)
            report.stoppedModuleIds.append(item.first);
        else
            report.timedOutModuleIds.append(item.first);

        QMutexLocker locker(&mutex_);
        Registration* registration = modules_.value(item.first, nullptr);
        if (registration != nullptr)
            registration->quarantined = result == ModuleQueueStopResult::TimedOut;
    }
    return report;
}

// 从路由表移除模块、停止专属线程、解绑宿主并释放 Registration。
bool MessageBus::unregisterModule(const QString& moduleId, bool drainPendingMessages)
{
    Registration* registration = nullptr;
    {
        QMutexLocker locker(&mutex_);
        registration = modules_.value(moduleId, nullptr);
    }
    if (registration == nullptr)
        return false;

    // 先关闭入口，但只有线程确定停止后才从注册表摘除并删除对象。
    registration->publishingEnabled = false;
    registration->endpoint->setRunning(false);
    if (registration->queue->isRunning()) {
        registration->queue->beginStop(!drainPendingMessages);
        if (registration->queue->finishStop(config_.shutdownDrainTimeoutMs) ==
            ModuleQueueStopResult::TimedOut) {
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
QStringList MessageBus::moduleIds() const
{
    QMutexLocker locker(&mutex_);
    return modules_.keys();
}

// 返回指定模块队列统计；未知模块返回全零默认结构。
ModuleQueueStats MessageBus::queueStats(const QString& moduleId) const
{
    QMutexLocker locker(&mutex_);
    Registration* registration = modules_.value(moduleId, nullptr);
    return registration == nullptr ? ModuleQueueStats() : registration->queue->stats();
}

bool MessageBus::isModuleQueueStopped(const QString& moduleId) const
{
    QMutexLocker locker(&mutex_);
    Registration* registration = modules_.value(moduleId, nullptr);
    return registration == nullptr || !registration->queue->isRunning();
}

QStringList MessageBus::quarantinedModuleIds() const
{
    QStringList result;
    QMutexLocker locker(&mutex_);
    for (QHash<QString, Registration*>::const_iterator iterator = modules_.constBegin();
         iterator != modules_.constEnd(); ++iterator) {
        if (iterator.value()->quarantined && iterator.value()->queue->isRunning())
            result.append(iterator.key());
    }
    return result;
}

// 执行发布权限、主题大小和订阅路由检查，再把值对象复制进各订阅者队列。
bool MessageBus::publishFromModule(const QString& moduleId,
                                   const QString& topic,
                                   const QByteArray& data)
{
    QMutexLocker locker(&mutex_);
    // 下面所有校验在同一把总线锁内完成，保证注册/关闭不会与广播交错。
    Registration* sender = modules_.value(moduleId, nullptr);
    if (!accepting_ || sender == nullptr || !sender->publishingEnabled) {
        logRejected(moduleId, QString::fromUtf8(u8"模块未运行或消息总线正在停止"));
        return false;
    }
    if (!sender->publishedTopics.contains(topic)) {
        logRejected(moduleId, QString::fromUtf8(u8"未声明发布主题：%1").arg(topic));
        return false;
    }

    const TopicConfig effective = topicConfig(topic);
    if (data.size() > effective.maxMessageBytes) {
        logRejected(moduleId, QString::fromUtf8(u8"消息超过大小上限：%1").arg(topic));
        return false;
    }

    QueuedMessage message;
    message.topic = topic;
    message.senderModuleId = moduleId;
    message.data = data;
    bool accepted = true;
    // 每个订阅者独立入队；只要有一个订阅者拒绝，返回值就是 false，
    // 但其他订阅者已经成功接收的消息不会回滚。
    for (Registration* recipient : modules_) {
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
void MessageBus::logFromModule(LogLevel level,
                               const QString& moduleId,
                               const QString& text)
{
    Logger::instance().log(level, moduleId, text);
}

// 查询主题专用规则；未配置时把全局默认值组合成完整 TopicConfig。
TopicConfig MessageBus::topicConfig(const QString& topic) const
{
    // 主题没有专用配置时复制全局默认值，调用方随后可安全调整本地副本。
    if (config_.topics.contains(topic))
        return config_.topics.value(topic);
    TopicConfig result;
    result.queueCapacity = config_.defaultQueueCapacity;
    result.maxMessageBytes = config_.maxMessageBytes;
    result.policy = config_.defaultPolicy;
    return result;
}

// 统一记录发布拒绝原因，保持调用点的错误文字和来源格式一致。
void MessageBus::logRejected(const QString& moduleId, const QString& reason) const
{
    Logger::instance().log(LogLevel::Warning, moduleId, reason);
}
}
