#include "MessageBus.h"

#include <QElapsedTimer>
#include <QMutexLocker>
#include <QQueue>
#include <QSet>
#include <QThread>
#include <QWaitCondition>

#include <exception>

#include "Logger.h"

namespace qframework
{
namespace
{
struct QueuedMessage
{
    QString topic;
    QString senderModuleId;
    QByteArray data;
};

class ModuleQueue : public QThread
{
public:
    ModuleQueue(const QString& moduleId, ModuleEndpoint* endpoint, bool paused)
        : moduleId_(moduleId),
          endpoint_(endpoint),
          accepting_(true),
          paused_(paused),
          stopping_(false)
    {
    }

    bool enqueue(const QueuedMessage& message, const TopicConfig& config)
    {
        QMutexLocker locker(&mutex_);
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
                ++stats_.rejected;
                return false;
            }
            if (oldestTopicIndex >= 0) {
                queue_.removeAt(oldestTopicIndex);
                ++stats_.dropped;
            }
        }

        queue_.enqueue(message);
        available_.wakeOne();
        return true;
    }

    void setPaused(bool paused)
    {
        QMutexLocker locker(&mutex_);
        paused_ = paused;
        if (!paused_)
            available_.wakeOne();
    }

    void beginStop(bool discardPending)
    {
        QMutexLocker locker(&mutex_);
        accepting_ = false;
        paused_ = false;
        stopping_ = true;
        if (discardPending) {
            stats_.dropped += static_cast<quint64>(queue_.size());
            queue_.clear();
        }
        available_.wakeOne();
    }

    bool finishStop(int timeoutMs)
    {
        if (wait(static_cast<unsigned long>(qMax(1, timeoutMs))))
            return true;

        {
            QMutexLocker locker(&mutex_);
            stats_.dropped += static_cast<quint64>(queue_.size());
            queue_.clear();
            available_.wakeOne();
        }
        wait();
        return false;
    }

    ModuleQueueStats stats() const
    {
        QMutexLocker locker(&mutex_);
        return stats_;
    }

protected:
    void run() override
    {
        for (;;) {
            QueuedMessage message;
            {
                QMutexLocker locker(&mutex_);
                while ((queue_.isEmpty() || paused_) && !stopping_)
                    available_.wait(&mutex_);
                if (queue_.isEmpty() && stopping_)
                    break;
                if (paused_)
                    continue;
                message = queue_.dequeue();
            }

            try {
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
    ModuleEndpoint* endpoint = nullptr;
    ModuleQueue* queue = nullptr;
    QSet<QString> publishedTopics;
    QSet<QString> subscribedTopics;
    bool publishingEnabled = false;
};

MessageBus::MessageBus(const MessageBusConfig& config)
    : config_(config),
      accepting_(true),
      deliveryEnabled_(false)
{
}

MessageBus::~MessageBus()
{
    beginShutdown();
    stopQueues(config_.shutdownDrainTimeoutMs);
    const QStringList ids = moduleIds();
    for (const QString& id : ids)
        unregisterModule(id, false);
}

bool MessageBus::registerModule(const QString& moduleId,
                                ModuleEndpoint* endpoint,
                                QString* errorMessage)
{
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
    modules_.insert(moduleId, registration);
    endpoint->bindHost(moduleId, this);
    registration->queue->start();
    return true;
}

bool MessageBus::setModuleRunning(const QString& moduleId, bool running)
{
    QMutexLocker locker(&mutex_);
    Registration* registration = modules_.value(moduleId, nullptr);
    if (registration == nullptr)
        return false;
    registration->publishingEnabled = running && accepting_;
    registration->endpoint->setRunning(registration->publishingEnabled);
    return true;
}

void MessageBus::setDeliveryEnabled(bool enabled)
{
    QMutexLocker locker(&mutex_);
    deliveryEnabled_ = enabled;
    for (Registration* registration : modules_)
        registration->queue->setPaused(!enabled);
}

void MessageBus::beginShutdown()
{
    QMutexLocker locker(&mutex_);
    accepting_ = false;
    deliveryEnabled_ = true;
    for (Registration* registration : modules_) {
        registration->publishingEnabled = false;
        registration->endpoint->setRunning(false);
        registration->queue->setPaused(false);
    }
}

bool MessageBus::stopQueues(int drainTimeoutMs)
{
    QVector<ModuleQueue*> queues;
    {
        QMutexLocker locker(&mutex_);
        for (Registration* registration : modules_) {
            registration->queue->beginStop(false);
            queues.append(registration->queue);
        }
    }

    QElapsedTimer timer;
    timer.start();
    bool drained = true;
    for (ModuleQueue* queue : queues) {
        const int remaining = qMax(1, drainTimeoutMs - static_cast<int>(timer.elapsed()));
        if (!queue->finishStop(remaining))
            drained = false;
    }
    return drained;
}

bool MessageBus::unregisterModule(const QString& moduleId, bool drainPendingMessages)
{
    Registration* registration = nullptr;
    {
        QMutexLocker locker(&mutex_);
        registration = modules_.take(moduleId);
    }
    if (registration == nullptr)
        return false;

    registration->publishingEnabled = false;
    registration->endpoint->setRunning(false);
    if (registration->queue->isRunning()) {
        registration->queue->beginStop(!drainPendingMessages);
        registration->queue->finishStop(config_.shutdownDrainTimeoutMs);
    }
    registration->endpoint->bindHost(QString(), nullptr);
    delete registration->queue;
    delete registration;
    return true;
}

QStringList MessageBus::moduleIds() const
{
    QMutexLocker locker(&mutex_);
    return modules_.keys();
}

ModuleQueueStats MessageBus::queueStats(const QString& moduleId) const
{
    QMutexLocker locker(&mutex_);
    Registration* registration = modules_.value(moduleId, nullptr);
    return registration == nullptr ? ModuleQueueStats() : registration->queue->stats();
}

bool MessageBus::publishFromModule(const QString& moduleId,
                                   const QString& topic,
                                   const QByteArray& data)
{
    QMutexLocker locker(&mutex_);
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

void MessageBus::logFromModule(LogLevel level,
                               const QString& moduleId,
                               const QString& text)
{
    Logger::instance().log(level, moduleId, text);
}

TopicConfig MessageBus::topicConfig(const QString& topic) const
{
    if (config_.topics.contains(topic))
        return config_.topics.value(topic);
    TopicConfig result;
    result.queueCapacity = config_.defaultQueueCapacity;
    result.maxMessageBytes = config_.maxMessageBytes;
    result.policy = config_.defaultPolicy;
    return result;
}

void MessageBus::logRejected(const QString& moduleId, const QString& reason) const
{
    Logger::instance().log(LogLevel::Warning, moduleId, reason);
}
}
