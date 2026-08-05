#include "ProcessRuntime.h"

#include <QCoreApplication>
#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QMutexLocker>
#include <QQueue>
#include <QSharedMemory>
#include <QThread>
#include <QUuid>
#include <QWaitCondition>
#include <QWidget>

#include <cstring>
#include <exception>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "ProcessProtocol.h"

// 子进程运行时的数据流：命令行参数 -> register -> 模块生命周期；
// 父到子消息进入 MessageQueue，子到父 publish 进入 PublishQueue。
// 本文件只负责传输和生命周期，业务规则仍由 ModuleEndpoint 实现。

namespace qframework
{
namespace
{
// 与 ProcessSupervisor 保持一致的内部帧 type；模块业务代码不会直接使用。
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
const QString kResizeWindow = QStringLiteral("resizeWindow");
const QString kWindowWidth = QStringLiteral("windowWidth");
const QString kWindowHeight = QStringLiteral("windowHeight");
const QString kSharedAck = QStringLiteral("sharedAck");
const QString kDeliveryAck = QStringLiteral("deliveryAck");
const QString kPublishAck = QStringLiteral("publishAck");
const QString kDebugWaitTimeout = QStringLiteral("debugWaitTimeout");
const QString kStyleSheet = QStringLiteral("styleSheet");

bool parsePositiveInt(const QString& value, int defaultValue, int* result)
{
    // 子进程命令行中的容量/超时必须为正数，否则使用安全默认值。
    bool ok = false;
    const int parsed = value.toInt(&ok);
    if (!ok || parsed <= 0)
        *result = defaultValue;
    else
        *result = parsed;
    return ok && parsed > 0;
}

QString sharedKey(const QString& prefix)
{
    // 子进程创建的大消息共享段使用模块前缀和 UUID，避免名称冲突。
    return QStringLiteral("QFramework_%1_%2")
        .arg(prefix, QUuid::createUuid().toString(QUuid::Id128));
}

void showEmbeddedWidget(QWidget* widget)
{
    // showWindow 只负责改变可见状态；尺寸变化由独立 resizeWindow 帧处理。
    if (widget == nullptr)
        return;

    widget->show();
}

void resizeEmbeddedWidget(QWidget* widget, int width, int height)
{
    // resizeWindow 不重复 show，避免拖动 Dock 时触发重入的窗口生命周期事件。
    if (widget == nullptr || width <= 0 || height <= 0)
        return;
    widget->resize(width, height);
}

struct OutboundMessage
{
    // 子进程 publish() 先把数据放进这个轻量对象，再由运行时线程发送。
    // 等待队列中的对象没有 messageId；只有真正进入“已发送、等待父进程 ACK”
    // 状态时才分配 ID，避免被 Latest 覆盖的项留下悬挂状态。
    QString topic;
    QByteArray data;
};

int queuedTopicCount(const QQueue<OutboundMessage>& queue,
                     const QString& topic)
{
    // PublishQueue 的容量按主题计数，互不相关的主题不会互相覆盖。
    int count = 0;
    for (const OutboundMessage& message : queue) {
        if (message.topic == topic)
            ++count;
    }
    return count;
}
}

// 子进程模块看到的宿主适配器。它不直接触碰 QLocalSocket，而是把 publish/log
// 转回 ProcessRuntime 的有界队列接口。
class ProcessRuntime::RuntimeHost final : public ModuleHost
{
public:
    // RuntimeHost 是 ModuleEndpoint 与 ProcessRuntime 之间的进程内适配器。
    // runtime 的生命周期包住 RuntimeHost；这里仅保存借用指针。
    explicit RuntimeHost(ProcessRuntime* runtime)
        : runtime_(runtime)
    {
    }

    bool publishFromModule(const QString& moduleId,
                           const QString& topic,
                           const QByteArray& data) override
    {
        // 运行时已经绑定唯一模块，moduleId 只满足统一接口，不再重复校验。
        Q_UNUSED(moduleId)
        return runtime_->queuePublish(topic, data);
    }

    void logFromModule(LogLevel level,
                       const QString& moduleId,
                       const QString& text) override
    {
        // 日志通过 queued 调用切回 Runtime/Socket 所在线程。
        Q_UNUSED(moduleId)
        runtime_->queueLog(level, text);
    }

private:
    ProcessRuntime* runtime_;
};

// 子到父的线程安全有界发布队列。
// 等待队列和 ACK 在途表共同限制每主题占用；Latest 只影响尚未发送的等待项。
class ProcessRuntime::PublishQueue final
{
public:
    struct Stats
    {
        quint64 dropped = 0;
        quint64 rejected = 0;
        quint64 abandoned = 0;
    };

    // runtime 用于安排合并后的 drainPublishQueue 唤醒事件。
    explicit PublishQueue(ProcessRuntime* runtime)
        : runtime_(runtime),
          accepting_(true),
          stopping_(false),
          wakeScheduled_(false),
          inFlightCount_(0)
    {
    }

    // 从任意模块线程入队；满队列按 TopicSettings 选择覆盖或拒绝。
    bool enqueue(const QString& topic,
                 const QByteArray& data,
                 const ProcessRuntime::TopicSettings& config,
                 bool* logWarning,
                 Stats* stats)
    {
        bool schedule = false;
        bool changed = false;
        {
            QMutexLocker locker(&mutex_);
            const auto finish = [this, &changed, logWarning, stats](bool result) {
                if (logWarning != nullptr) {
                    *logWarning = false;
                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                    if (changed && now - lastWarningMs_ >= 1000) {
                        lastWarningMs_ = now;
                        *logWarning = true;
                    }
                }
                if (stats != nullptr)
                    *stats = stats_;
                return result;
            };
            // accepting_ 在断线、stop 和析构时会被关闭；所有生产者都先检查它，
            // 因此停止过程不需要依赖一个可能永远不返回的阻塞发送。
            if (!accepting_) {
                ++stats_.rejected;
                changed = true;
                return finish(false);
            }
            if (data.size() > config.maxMessageBytes) {
                ++stats_.rejected;
                changed = true;
                return finish(false);
            }

            int topicCount = queuedTopicCount(queue_, topic);
            if (topicCount >= config.queueCapacity) {
                if (!config.latest) {
                    // Reliable 的“满”是明确拒绝，不允许用新帧覆盖旧帧。
                    ++stats_.rejected;
                    changed = true;
                    return finish(false);
                }
                int oldestIndex = -1;
                for (int index = 0; index < queue_.size(); ++index) {
                    if (queue_.at(index).topic == topic) {
                        oldestIndex = index;
                        break;
                    }
                }
                if (oldestIndex < 0) {
                    ++stats_.rejected;
                    changed = true;
                    return finish(false);
                }
                // Latest 只替换同主题等待队列中最早的一项；已经发出并等待
                // publishAck 的项不在 queue_ 中，所以不会被覆盖。
                queue_.removeAt(oldestIndex);
                ++stats_.dropped;
                changed = true;
            }

            OutboundMessage message;
            message.topic = topic;
            message.data = data;
            queue_.enqueue(message);
            if (!wakeScheduled_) {
                // 只安排一次无参数 Qt 唤醒。后续消息仍留在 queue_，由 drain
                // 以有限批次取走，避免“一次 publish 一个 queued event”。
                wakeScheduled_ = true;
                schedule = true;
            }
            finish(true);
        }
        if (schedule)
            QMetaObject::invokeMethod(runtime_,
                                      "drainPublishQueue",
                                      Qt::QueuedConnection);
        return true;
    }

    // 在运行时线程取出一条有可用在途槽位的消息，并原子增加计数。
    bool takeNext(const QHash<QString, ProcessRuntime::TopicSettings>& topicConfigs,
                  const ProcessRuntime::TopicSettings& defaultConfig,
                  OutboundMessage* result)
    {
        if (result == nullptr)
            return false;
        QMutexLocker locker(&mutex_);
        if (!accepting_)
            return false;
        for (int index = 0; index < queue_.size(); ++index) {
            const OutboundMessage& candidate = queue_.at(index);
            const ProcessRuntime::TopicSettings config =
                topicConfigs.value(candidate.topic, defaultConfig);
            if (inFlightByTopic_.value(candidate.topic, 0) >= config.queueCapacity)
                continue;
            // 从等待队列移到在途表后，槽位才真正算“占用”。在途表按主题计数，
            // 所以父进程未 ACK 时，新的同主题消息会暂缓而不是无限写 Socket。
            *result = queue_.takeAt(index);
            ++inFlightByTopic_[result->topic];
            ++inFlightCount_;
            return true;
        }
        return false;
    }

    // Socket 写成功后登记 messageId -> topic，等待父进程 publishAck。
    void registerMessage(const QString& messageId, const QString& topic)
    {
        QMutexLocker locker(&mutex_);
        // 父进程 ACK 只有 messageId；保存主题是为了 ACK 到达时归还正确的
        // per-topic 在途计数。
        messageTopics_.insert(messageId, topic);
    }

    // 收到 ACK 后释放对应主题在途槽位，并唤醒可能等待发送的队列。
    bool acknowledge(const QString& messageId, bool acceptedValue)
    {
        QMutexLocker locker(&mutex_);
        const QString topic = messageTopics_.take(messageId);
        if (topic.isEmpty())
            return false;
        // acceptedValue 只用于上层统计“父进程拒绝了多少条”；无论结果如何，
        // ACK 都代表这条在途消息已经有最终结果，必须释放一个槽位并唤醒排空。
        Q_UNUSED(acceptedValue)
        const int count = inFlightByTopic_.value(topic, 0);
        if (count <= 1)
            inFlightByTopic_.remove(topic);
        else
            inFlightByTopic_.insert(topic, count - 1);
        inFlightCount_ = qMax(0, inFlightCount_ - 1);
        return true;
    }

    // Socket/共享内存尚未接受这一项时按 dropped 回收，不把它算成远端拒绝。
    void discardBeforeSend(const QString& messageId)
    {
        QMutexLocker locker(&mutex_);
        const QString topic = messageTopics_.take(messageId);
        if (topic.isEmpty())
            return;
        const int count = inFlightByTopic_.value(topic, 0);
        if (count <= 1)
            inFlightByTopic_.remove(topic);
        else
            inFlightByTopic_.insert(topic, count - 1);
        inFlightCount_ = qMax(0, inFlightCount_ - 1);
        ++stats_.dropped;
    }

    // ACK 释放槽位后判断是否需要再次安排合并唤醒。
    bool scheduleIfNeeded(
        const QHash<QString, ProcessRuntime::TopicSettings>& topicConfigs,
        const ProcessRuntime::TopicSettings& defaultConfig)
    {
        QMutexLocker locker(&mutex_);
        if (!accepting_ || stopping_ || wakeScheduled_)
            return false;
        for (const OutboundMessage& candidate : queue_) {
            const ProcessRuntime::TopicSettings config =
                topicConfigs.value(candidate.topic, defaultConfig);
            if (inFlightByTopic_.value(candidate.topic, 0) < config.queueCapacity) {
                // ACK 刚释放了槽位，若还有可发送项才重新安排一次 drain。
                wakeScheduled_ = true;
                return true;
            }
        }
        return false;
    }

    // 本轮有限 drain 结束时清除/保留 wakeScheduled_ 标记。
    bool finishDrain(
        const QHash<QString, ProcessRuntime::TopicSettings>& topicConfigs,
        const ProcessRuntime::TopicSettings& defaultConfig)
    {
        QMutexLocker locker(&mutex_);
        if (!accepting_ || stopping_) {
            wakeScheduled_ = false;
            return false;
        }
        for (const OutboundMessage& candidate : queue_) {
            const ProcessRuntime::TopicSettings config =
                topicConfigs.value(candidate.topic, defaultConfig);
            if (inFlightByTopic_.value(candidate.topic, 0) < config.queueCapacity)
                return true;
        }
        // 没有可发送项时清除标记；下一次 enqueue 才会重新创建一个唤醒事件。
        wakeScheduled_ = false;
        return false;
    }

    // 停止时原子切断生产、清空等待/在途状态并唤醒所有潜在等待者。
    Stats stop()
    {
        QMutexLocker locker(&mutex_);
        // 同一轮可能从 stop、断线和析构重复进入；容器清空后再次调用不会重复计数。
        stats_.dropped += static_cast<quint64>(queue_.size());
        stats_.abandoned += static_cast<quint64>(messageTopics_.size());
        accepting_ = false;
        stopping_ = true;
        queue_.clear();
        inFlightByTopic_.clear();
        messageTopics_.clear();
        inFlightCount_ = 0;
        wakeScheduled_ = false;
        return stats_;
    }

private:
    // runtime_ 只用于投递合并唤醒，队列本身不拥有运行时。
    ProcessRuntime* runtime_;
    // mutex_ 保护等待队列、在途表、统计和状态标志；本队列没有阻塞等待者。
    QMutex mutex_;
    // 尚未写入 Socket 的业务消息。
    QQueue<OutboundMessage> queue_;
    // 每主题在途数和 messageId -> topic 反向索引。
    QHash<QString, int> inFlightByTopic_;
    QHash<QString, QString> messageTopics_;
    // accepting_/stopping_ 控制生命周期；wakeScheduled_ 合并 Qt queued 唤醒。
    bool accepting_;
    bool stopping_;
    bool wakeScheduled_;
    // 仅用于诊断和清理一致性检查，不直接决定容量。
    int inFlightCount_;
    Stats stats_;
    qint64 lastWarningMs_ = 0;
};

// 父到子的线程安全输入队列。收到消息后可立即 deliveryAck，消费者速度不再
// 反向占用 Socket 发送方的无限事件队列。
class ProcessRuntime::MessageQueue final : public QThread
{
public:
    enum class StopResult
    {
        Stopped,
        TimedOut
    };

    // 保存模块借用指针和 registerAck 下发的主题快照。
    MessageQueue(ModuleEndpoint* module,
                 const QHash<QString, ProcessRuntime::TopicSettings>& topicConfigs,
                 const ProcessRuntime::TopicSettings& defaultConfig)
        : module_(module),
          topicConfigs_(topicConfigs),
          defaultConfig_(defaultConfig),
          accepting_(true),
          stopping_(false)
    {
    }

    // 进入输入边界；Latest 覆盖同主题最旧等待项，Reliable 满时返回 false。
    bool enqueue(const QString& topic,
                 const QString& senderModuleId,
                 const QByteArray& data)
    {
        QMutexLocker locker(&mutex_);
        // 这是子进程的输入边界：父进程收到消息后只需把数据复制到这里，
        // 随即发送 deliveryAck，不会等待真正的 onMessage() 消费速度。
        if (!accepting_)
            return false;
        const ProcessRuntime::TopicSettings config =
            topicConfigs_.value(topic, defaultConfig_);
        int topicCount = 0;
        int oldestTopicIndex = -1;
        for (int index = 0; index < messages_.size(); ++index) {
            if (messages_.at(index).topic == topic) {
                if (oldestTopicIndex < 0)
                    oldestTopicIndex = index;
                ++topicCount;
            }
        }
        if (topicCount >= config.queueCapacity) {
            if (!config.latest)
                // Reliable 输入队列满时拒绝新消息，旧消息原样保留。
                return false;
            if (oldestTopicIndex < 0)
                return false;
            // Latest 只覆盖同主题的最旧等待消息；不同主题共享容器但互不影响。
            messages_.removeAt(oldestTopicIndex);
        }
        Message message;
        message.topic = topic;
        message.senderModuleId = senderModuleId;
        message.data = data;
        messages_.enqueue(message);
        available_.wakeOne();
        return true;
    }

    // 禁止新入队并在有限时间内等待 onMessage 线程结束。
    StopResult stopAndDrain(int timeoutMs)
    {
        {
            QMutexLocker locker(&mutex_);
            // 修改停止状态后立即 wakeAll，唤醒 wait() 中的消息线程；timeout
            // 是硬上限，避免异常模块让框架永久卡住。
            accepting_ = false;
            stopping_ = true;
            available_.wakeAll();
        }
        if (wait(static_cast<unsigned long>(qMax(1, timeoutMs))))
            return StopResult::Stopped;
        // 回调超时后保留线程和模块对象；调用方将结束整个子进程隔离边界。
        return StopResult::TimedOut;
    }

protected:
    // 消费线程串行调用模块 onMessage；每次等待最多 100 ms 并响应 stop 唤醒。
    void run() override
    {
        for (;;) {
            Message message;
            {
                QMutexLocker locker(&mutex_);
                // wait 带 100 ms 超时，即使漏掉唤醒也会周期性检查 stopping_；
                // stopAndDrain 的 wakeAll 则负责正常路径的即时唤醒。
                while (messages_.isEmpty() && !stopping_)
                    available_.wait(&mutex_, 100);
                if (messages_.isEmpty() && stopping_)
                    return;
                message = messages_.dequeue();
            }
            // 回调在独立消息线程中串行执行；队列锁在调用 onMessage 前已经释放，
            // 因此业务回调可以发布新消息而不会形成自锁。
            try {
                module_->onMessage(message.topic,
                                   message.senderModuleId,
                                   message.data);
            } catch (...) {
                module_->logError(QString::fromUtf8(u8"onMessage 未知异常"));
            }
        }
    }

private:
    // 输入消息按值保存，离开 Socket/共享内存解析作用域后仍然有效。
    struct Message
    {
        // 主题和发送者用于业务路由，data 是完整负载副本。
        QString topic;
        QString senderModuleId;
        QByteArray data;
    };

    // module_ 是 ProcessRuntime 拥有的业务对象；配置快照构造后不再修改。
    ModuleEndpoint* module_;
    QHash<QString, ProcessRuntime::TopicSettings> topicConfigs_;
    ProcessRuntime::TopicSettings defaultConfig_;
    QMutex mutex_;
    QWaitCondition available_;
    QQueue<Message> messages_;
    bool accepting_;
    bool stopping_;
};

// 构造运行时并连接 Socket 信号；此时尚未连接父进程，也不会调用模块回调。
ProcessRuntime::ProcessRuntime(QCoreApplication* application, ModuleEndpoint* module)
    : QObject(application),
      application_(application),
      module_(module),
      host_(new RuntimeHost(this)),
      messageQueue_(nullptr),
      publishQueue_(new PublishQueue(this)),
      socket_(new QLocalSocket(this)),
      sharedMemoryThresholdBytes_(1024 * 1024),
      maxMessageBytes_(16 * 1024 * 1024),
      defaultQueueCapacity_(256),
      defaultMaxMessageBytes_(16 * 1024 * 1024),
      defaultLatest_(false),
      shutdownDrainTimeoutMs_(3000),
      waitForDebugger_(false),
      debuggerWaitTimeoutMs_(30000),
      registrationAcknowledged_(false),
      running_(false),
      stopping_(false),
      unsafeMessageThread_(false),
      exitCode_(0),
      publishRejectedCount_(0),
      publishDroppedCount_(0),
      publishLocalRejectedCount_(0),
      publishAbandonedCount_(0),
      lastPublishRejectWarningMs_(0)
{
    connect(socket_, &QLocalSocket::connected,
            this, &ProcessRuntime::onSocketConnected);
    connect(socket_, &QLocalSocket::readyRead,
            this, &ProcessRuntime::onSocketReadyRead);
    connect(socket_, &QLocalSocket::disconnected,
            this, &ProcessRuntime::onSocketDisconnected);
    connect(socket_,
            QOverload<QLocalSocket::LocalSocketError>::of(&QLocalSocket::errorOccurred),
            this,
            &ProcessRuntime::onSocketError);
}

void ProcessRuntime::assertSocketThread() const
{
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(socket_ == nullptr || socket_->thread() == QThread::currentThread());
}

bool ProcessRuntime::stopPublishQueue(const QString& reason, bool report)
{
    PublishQueue::Stats stats;
    {
        QMutexLocker locker(&publishQueueMutex_);
        if (publishQueue_ == nullptr)
            return true;
        stats = publishQueue_->stop();
    }
    publishDroppedCount_ = stats.dropped;
    publishLocalRejectedCount_ = stats.rejected;
    publishAbandonedCount_ = stats.abandoned;
    if (!report || (stats.dropped == 0 && stats.rejected == 0 &&
                    stats.abandoned == 0 && publishRejectedCount_ == 0)) {
        return true;
    }

    // 汇总帧只从 Runtime 线程发送；传输本身失败时由调用者进入统一 fault/finish。
    assertSocketThread();
    QJsonObject frame;
    frame.insert(QStringLiteral("type"), kLog);
    frame.insert(QStringLiteral("level"), static_cast<int>(LogLevel::Warning));
    frame.insert(
        QStringLiteral("text"),
        QString::fromUtf8(
            u8"子进程发送队列汇总（%1）：dropped=%2，rejected=%3，abandoned=%4，remoteRejected=%5")
            .arg(reason)
            .arg(stats.dropped)
            .arg(stats.rejected)
            .arg(stats.abandoned)
            .arg(publishRejectedCount_));
    return sendFrame(frame);
}

// 析构是所有退出路径的统一资源兜底，包括正常 stop、断线和协议错误。
ProcessRuntime::~ProcessRuntime()
{
    // 析构顺序与数据流相反：先停止生产者和消息线程，再释放模块，最后
    // 清理尚未收到 ACK 的共享内存。这样模块不会在 host 已失效后继续 publish。
    if (module_ != nullptr)
        module_->setRunning(false);
    stopPublishQueue(QString(), false);
    if (messageQueue_ != nullptr) {
        Q_ASSERT(!messageQueue_->isRunning());
        delete messageQueue_;
        messageQueue_ = nullptr;
    }
    if (module_ != nullptr) {
        module_->bindHost(QString(), nullptr);
        delete module_;
        module_ = nullptr;
    }
    delete host_;
    host_ = nullptr;
    {
        // queuePublish() 持有同一把锁直到 enqueue 返回，故此处不会删除在用指针。
        QMutexLocker locker(&publishQueueMutex_);
        delete publishQueue_;
        publishQueue_ = nullptr;
    }
    clearSharedSegments();
}

// 对外唯一入口：安全停止时正常析构；卡死回调时泄漏到进程退出，避免删除活线程。
int ProcessRuntime::run(QCoreApplication* application, ModuleEndpoint* module)
{
    if (application == nullptr || module == nullptr)
        return 2;
    ProcessRuntime* runtime = new ProcessRuntime(application, module);
    const int result = runtime->execute();
    if (!runtime->prepareForExit()) {
        // 解除 QObject 父子关系，防止 QCoreApplication 析构时删除仍被回调借用的对象。
        runtime->setParent(nullptr);
        return result == 0 ? 7 : result;
    }
    runtime->setParent(nullptr);
    delete runtime;
    return result;
}

// 参数有效后连接父进程并进入现有 Qt 事件循环，最终返回内部退出码。
int ProcessRuntime::execute()
{
    if (!parseArguments())
        return 2;
    socket_->connectToServer(serverName_);
    application_->exec();
    return exitCode_;
}

// 读取监督器注入的内部参数，校验模块类型，并把 ModuleEndpoint 绑定到 RuntimeHost。
bool ProcessRuntime::parseArguments()
{
    arguments_ = application_->arguments();
    moduleId_ = argumentValue(QStringLiteral("--qframework-module-id"));
    moduleType_ = argumentValue(QStringLiteral("--qframework-module-type"));
    serverName_ = argumentValue(QStringLiteral("--qframework-server"));
    token_ = argumentValue(QStringLiteral("--qframework-token"));
    if (moduleId_.isEmpty() || moduleType_.isEmpty() || serverName_.isEmpty() || token_.isEmpty())
        return false;

    parsePositiveInt(argumentValue(QStringLiteral("--qframework-shared-memory-threshold")),
                     sharedMemoryThresholdBytes_,
                     &sharedMemoryThresholdBytes_);
    parsePositiveInt(argumentValue(QStringLiteral("--qframework-max-message-bytes")),
                     maxMessageBytes_,
                     &maxMessageBytes_);
    parsePositiveInt(argumentValue(QStringLiteral("--qframework-shutdown-drain-timeout")),
                     shutdownDrainTimeoutMs_,
                     &shutdownDrainTimeoutMs_);
    waitForDebugger_ = hasArgument(QStringLiteral("--qframework-wait-for-debugger"));
    parsePositiveInt(argumentValue(QStringLiteral("--qframework-debugger-timeout-ms")),
                     debuggerWaitTimeoutMs_,
                     &debuggerWaitTimeoutMs_);

    if (moduleType_ == QStringLiteral("ProcessUi") && dynamic_cast<QWidget*>(module_) == nullptr)
        return false;
    if (moduleType_ == QStringLiteral("ProcessNonUi") && dynamic_cast<QWidget*>(module_) != nullptr)
        return false;
    module_->bindHost(moduleId_, host_);
    return true;
}

// 仅用于无值开关，例如 --qframework-wait-for-debugger。
bool ProcessRuntime::hasArgument(const QString& name) const
{
    return arguments_.contains(name);
}

// 返回命令行开关后的一个值；缺失或位于末尾都返回空字符串。
QString ProcessRuntime::argumentValue(const QString& name) const
{
    const int index = arguments_.indexOf(name);
    if (index < 0 || index + 1 >= arguments_.size())
        return QString();
    return arguments_.at(index + 1);
}

// 查询主题专用规则，不存在时构造默认规则，并保证容量/大小至少为 1。
ProcessRuntime::TopicSettings ProcessRuntime::topicConfig(const QString& topic) const
{
    TopicSettings result = topicConfigs_.value(topic);
    if (!topicConfigs_.contains(topic)) {
        result.queueCapacity = defaultQueueCapacity_;
        result.maxMessageBytes = defaultMaxMessageBytes_;
        result.latest = defaultLatest_;
    }
    result.queueCapacity = qMax(1, result.queueCapacity);
    result.maxMessageBytes = qMax(1, result.maxMessageBytes);
    return result;
}

// 把 registerAck 中的 JSON 队列规则转换为强类型 TopicSettings 快照。
bool ProcessRuntime::parseTopicConfigs(const QJsonObject& frame)
{
    // registerAck 携带的是父进程的最终配置。子进程把它解析成自己的快照，
    // 后续入队和大小检查都只读这份快照，保证父子两边对容量和策略的判断一致。
    int defaultCapacity = frame.value(QStringLiteral("defaultQueueCapacity")).toInt(
        defaultQueueCapacity_);
    int defaultMaxBytes = frame.value(QStringLiteral("defaultMaxMessageBytes")).toInt(
        defaultMaxMessageBytes_);
    if (defaultCapacity <= 0)
        defaultCapacity = 256;
    if (defaultMaxBytes <= 0)
        defaultMaxBytes = maxMessageBytes_;

    const QString defaultPolicyValue = frame.value(QStringLiteral("defaultPolicy"))
        .toString(QStringLiteral("Reliable"));
    const bool defaultLatest = defaultPolicyValue.compare(
        QStringLiteral("Latest"), Qt::CaseInsensitive) == 0;
    if (!defaultLatest && defaultPolicyValue.compare(
            QStringLiteral("Reliable"), Qt::CaseInsensitive) != 0)
        return false;

    QHash<QString, TopicSettings> parsed;
    const QJsonValue value = frame.value(QStringLiteral("topicConfigs"));
    if (value.isArray()) {
        for (const QJsonValue& itemValue : value.toArray()) {
            if (!itemValue.isObject())
                return false;
            const QJsonObject item = itemValue.toObject();
            const QString topic = item.value(QStringLiteral("topic")).toString();
            const int capacity = item.value(QStringLiteral("queueCapacity")).toInt();
            const int maxBytes = item.value(QStringLiteral("maxMessageBytes")).toInt();
            const QString policyValue = item.value(QStringLiteral("policy")).toString();
            const bool latest = policyValue.compare(
                QStringLiteral("Latest"), Qt::CaseInsensitive) == 0;
            if (topic.isEmpty() || capacity <= 0 || maxBytes <= 0 ||
                (!latest && policyValue.compare(
                    QStringLiteral("Reliable"), Qt::CaseInsensitive) != 0)) {
                return false;
            }
            TopicSettings config;
            config.queueCapacity = capacity;
            config.maxMessageBytes = maxBytes;
            config.latest = latest;
            parsed.insert(topic, config);
        }
    } else if (!value.isUndefined()) {
        return false;
    }
    defaultQueueCapacity_ = defaultCapacity;
    defaultMaxMessageBytes_ = qMin(defaultMaxBytes, maxMessageBytes_);
    defaultLatest_ = defaultLatest;
    topicConfigs_ = parsed;
    return true;
}

// Socket 建立后首先发送身份、随机令牌以及发布/订阅主题声明。
void ProcessRuntime::onSocketConnected()
{
    assertSocketThread();
    QJsonObject frame;
    frame.insert(QStringLiteral("type"), kRegister);
    frame.insert(QStringLiteral("moduleId"), moduleId_);
    frame.insert(QStringLiteral("moduleType"), moduleType_);
    frame.insert(QStringLiteral("token"), token_);
    frame.insert(QStringLiteral("pid"), static_cast<qint64>(QCoreApplication::applicationPid()));
    QJsonArray published;
    for (const QString& topic : module_->publishedTopics())
        published.append(topic);
    QJsonArray subscribed;
    for (const QString& topic : module_->subscribedTopics())
        subscribed.append(topic);
    frame.insert(QStringLiteral("publishedTopics"), published);
    frame.insert(QStringLiteral("subscribedTopics"), subscribed);
    if (!sendFrame(frame))
        finish(4);
}

// 累积任意长度的 Socket 数据，循环取出所有完整帧；半帧留到下一次 readyRead。
void ProcessRuntime::onSocketReadyRead()
{
    assertSocketThread();
    inputBuffer_.append(socket_->readAll());
    for (;;) {
        QJsonObject frame;
        QString error;
        const process::FrameResult result = process::takeFrame(
            &inputBuffer_,
            &frame,
            qMax(64 * 1024, maxMessageBytes_ * 2),
            &error);
        if (result == process::FrameResult::Incomplete)
            return;
        if (result == process::FrameResult::Invalid) {
            Q_UNUSED(error)
            finish(5);
            return;
        }
        handleFrame(frame);
        if (stopping_)
            return;
    }
}

// 非正常停止期间断线视为运行故障，使用专用退出码结束子进程。
void ProcessRuntime::onSocketDisconnected()
{
    assertSocketThread();
    if (!stopping_)
        finish(4);
}

// 注册前的连接错误无法恢复；注册后的断线会由 disconnected 路径处理。
void ProcessRuntime::onSocketError(QLocalSocket::LocalSocketError error)
{
    assertSocketThread();
    Q_UNUSED(error)
    if (!registrationAcknowledged_)
        finish(3);
}

// 统一分派注册、心跳、样式、窗口、停止、ACK 和业务消息控制帧。
void ProcessRuntime::handleFrame(const QJsonObject& frame)
{
    const QString type = frame.value(QStringLiteral("type")).toString();
    if (type == kRegisterAck) {
        if (!frame.value(QStringLiteral("accepted")).toBool()) {
            finish(3);
            return;
        }
        if (!parseTopicConfigs(frame)) {
            finish(5);
            return;
        }
        registrationAcknowledged_ = true;
        QApplication* widgetApplication = qobject_cast<QApplication*>(application_);
        if (widgetApplication != nullptr &&
            frame.value(QStringLiteral("styleSheet")).isString()) {
            widgetApplication->setStyleSheet(
                frame.value(QStringLiteral("styleSheet")).toString());
        }
#ifdef Q_OS_WIN
#ifdef _DEBUG
        if (waitForDebugger_) {
            QElapsedTimer timer;
            timer.start();
            while (!IsDebuggerPresent() && timer.elapsed() < debuggerWaitTimeoutMs_)
                QThread::msleep(50);
            if (IsDebuggerPresent()) {
                // 附加后先停在同步断点，给 Visual Studio 时间绑定模块符号和初始化断点。
                DebugBreak();
            } else {
                QJsonObject timeoutFrame;
                timeoutFrame.insert(QStringLiteral("type"), kDebugWaitTimeout);
                if (!sendFrame(timeoutFrame)) {
                    finish(4);
                    return;
                }
            }
        }
#endif
#endif
        module_->setRunning(true);
        running_ = true;
        bool started = false;
        try {
            started = module_->onStart();
        } catch (const std::exception& exception) {
            QJsonObject failure;
            failure.insert(QStringLiteral("type"), kStartFailed);
            failure.insert(QStringLiteral("detail"), QString::fromUtf8(exception.what()));
            const bool sent = sendFrame(failure);
            module_->setRunning(false);
            running_ = false;
            finish(sent ? 6 : 4);
            return;
        } catch (...) {
            QJsonObject failure;
            failure.insert(QStringLiteral("type"), kStartFailed);
            failure.insert(QStringLiteral("detail"), QString::fromUtf8(u8"未知启动异常"));
            const bool sent = sendFrame(failure);
            module_->setRunning(false);
            running_ = false;
            finish(sent ? 6 : 4);
            return;
        }
        if (!started) {
            QJsonObject failure;
            failure.insert(QStringLiteral("type"), kStartFailed);
            failure.insert(QStringLiteral("detail"), QString::fromUtf8(u8"onStart 返回 false"));
            const bool sent = sendFrame(failure);
            module_->setRunning(false);
            running_ = false;
            finish(sent ? 6 : 4);
            return;
        }
        messageQueue_ = new MessageQueue(module_,
                                         topicConfigs_,
                                         topicConfig(QString()));
        messageQueue_->start();
        QJsonObject startedFrame;
        startedFrame.insert(QStringLiteral("type"), kStarted);
        if (!sendFrame(startedFrame)) {
            finish(4);
            return;
        }
        QWidget* widget = dynamic_cast<QWidget*>(module_);
        if (widget != nullptr) {
            QJsonObject windowFrame;
            windowFrame.insert(QStringLiteral("type"), kWindowReady);
            windowFrame.insert(QStringLiteral("windowId"),
                               QString::number(static_cast<qulonglong>(widget->winId())));
            if (!sendFrame(windowFrame)) {
                finish(4);
                return;
            }
        }
        return;
    }
    if (type == kPing) {
        QJsonObject pong;
        pong.insert(QStringLiteral("type"), kPong);
        if (!sendFrame(pong))
            finish(4);
        return;
    }
    if (type == kStyleSheet) {
        QApplication* widgetApplication = qobject_cast<QApplication*>(application_);
        if (widgetApplication != nullptr)
            widgetApplication->setStyleSheet(
                frame.value(QStringLiteral("styleSheet")).toString());
        return;
    }
    if (type == kShowWindow && running_) {
        QWidget* widget = dynamic_cast<QWidget*>(module_);
        showEmbeddedWidget(widget);
        return;
    }
    if (type == kResizeWindow && running_) {
        QWidget* widget = dynamic_cast<QWidget*>(module_);
        resizeEmbeddedWidget(widget,
                             frame.value(kWindowWidth).toInt(),
                             frame.value(kWindowHeight).toInt());
        return;
    }
    if (type == kStop) {
        // 停止消息先关闭两个有界队列，再等待输入回调在限定时间内排空；
        // 无论 onStop() 是否抛异常，都要给父进程发送 stopAck，随后退出事件循环。
        stopping_ = true;
        module_->setRunning(false);
        running_ = false;
        if (!stopPublishQueue(QString::fromUtf8(u8"stop"), true)) {
            finish(4);
            return;
        }
        MessageQueue::StopResult queueStop = MessageQueue::StopResult::Stopped;
        if (messageQueue_ != nullptr)
            queueStop = messageQueue_->stopAndDrain(shutdownDrainTimeoutMs_);
        if (queueStop == MessageQueue::StopResult::TimedOut) {
            unsafeMessageThread_ = true;
            QJsonObject ack;
            ack.insert(QStringLiteral("type"), kStopAck);
            ack.insert(QStringLiteral("clean"), false);
            ack.insert(QStringLiteral("detail"),
                       QString::fromUtf8(u8"子进程消息回调停止超时，将结束整个进程"));
            if (!sendFrame(ack)) {
                finish(4);
                return;
            }
            socket_->flush();
            socket_->waitForBytesWritten(500);
            // 不调用 onStop，不删除 MessageQueue/ModuleEndpoint；main 返回后整个进程结束。
            finish(7);
            return;
        }
        try {
            module_->onStop();
        } catch (...) {
            // 停止边界必须隔离模块异常，随后仍发送确认。
        }
        QJsonObject ack;
        ack.insert(QStringLiteral("type"), kStopAck);
        ack.insert(QStringLiteral("clean"), true);
        if (!sendFrame(ack)) {
            finish(4);
            return;
        }
        socket_->flush();
        socket_->waitForBytesWritten(500);
        finish(0);
        return;
    }
    if (type == kPublishAck || type == kSharedAck) {
        handlePublishAck(frame);
        return;
    }
    if (type != kMessage || !running_)
        return;

    // 这是父到子消息的接收端。控制帧只携带 inline 数据或共享内存 key，
    // 共享内存内容在这里复制成模块 API 所需的 QByteArray，然后马上 detach。
    const QString topic = frame.value(QStringLiteral("topic")).toString();
    const QString sender = frame.value(QStringLiteral("senderModuleId")).toString();
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
                if (declaredSize > 0 && copySize == declaredSize) {
                    data = QByteArray(static_cast<const char*>(shared.constData()),
                                      copySize);
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
    const ProcessRuntime::TopicSettings config = topicConfig(topic);
    if (data.size() > maxMessageBytes_ || data.size() > config.maxMessageBytes)
        valid = false;
    const bool accepted = valid && messageQueue_ != nullptr &&
                          messageQueue_->enqueue(topic, sender, data);
    // delivery ACK 表示消息已经进入子进程有界队列；不等待慢消费者的
    // onMessage() 返回，父进程即可释放在途槽位和共享内存。
    if (!sendDeliveryAck(messageId, accepted))
        finish(4);
}

// 从子到父发送队列取有限批次，并按大小选择 inline 或 shared 传输。
void ProcessRuntime::drainPublishQueue()
{
    assertSocketThread();
    if (publishQueue_ == nullptr)
        return;

    // publish() 只负责入队；真正的 IPC 写入在运行时线程按小批次完成。
    // 这样高频调用只产生合并后的唤醒事件，且每个主题的在途数量有上限。
    const int batchLimit = 8;
    for (int sent = 0; sent < batchLimit; ++sent) {
        if (!running_ || socket_->state() != QLocalSocket::ConnectedState) {
            publishQueue_->finishDrain(topicConfigs_, topicConfig(QString()));
            return;
        }

        OutboundMessage message;
        if (!publishQueue_->takeNext(topicConfigs_, topicConfig(QString()), &message))
            break;

        // 从等待队列取出后才分配 messageId。父进程会把这个 ID 原样放回
        // publishAck，子进程据此释放对应的 in-flight 槽位。
        const QString messageId = QUuid::createUuid().toString(QUuid::Id128);
        publishQueue_->registerMessage(messageId, message.topic);
        QJsonObject frame;
        frame.insert(QStringLiteral("type"), kMessage);
        frame.insert(QStringLiteral("topic"), message.topic);
        frame.insert(QStringLiteral("senderModuleId"), moduleId_);
        frame.insert(QStringLiteral("messageId"), messageId);
        if (message.data.size() >= sharedMemoryThresholdBytes_) {
            // 共享内存只承载大 payload；Socket 发送的 JSON 是很小的控制信息。
            // 句柄放入 outgoingSharedSegments_，在父进程 ACK 前不能释放。
            const QString key = sharedKey(moduleId_);
            QSharedMemory* shared = new QSharedMemory(key, this);
            if (!shared->create(message.data.size())) {
                delete shared;
                publishQueue_->discardBeforeSend(messageId);
                continue;
            }
            if (!shared->lock()) {
                shared->detach();
                delete shared;
                publishQueue_->discardBeforeSend(messageId);
                continue;
            }
            std::memcpy(shared->data(),
                        message.data.constData(),
                        static_cast<size_t>(message.data.size()));
            shared->unlock();
            outgoingSharedSegments_.insert(messageId, shared);
            frame.insert(QStringLiteral("transport"), QStringLiteral("shared"));
            frame.insert(QStringLiteral("sharedKey"), key);
            frame.insert(QStringLiteral("size"), message.data.size());
        } else {
            // 小 payload 使用 base64 放进控制帧。它仍然通过有界队列进入，
            // 不会因为 Qt::QueuedConnection 而为每帧复制一个无上限事件参数。
            frame.insert(QStringLiteral("transport"), QStringLiteral("inline"));
            frame.insert(QStringLiteral("data"),
                         QString::fromLatin1(message.data.toBase64()));
        }
        if (!sendFrame(frame)) {
            publishQueue_->discardBeforeSend(messageId);
            QSharedMemory* shared = outgoingSharedSegments_.take(messageId);
            if (shared != nullptr) {
                shared->detach();
                delete shared;
            }
            finish(4);
            return;
        }
    }

    if (publishQueue_->finishDrain(topicConfigs_, topicConfig(QString())))
        QMetaObject::invokeMethod(this,
                                  "drainPublishQueue",
                                  Qt::QueuedConnection);
}

// 在运行时 Qt 线程中把模块日志编码成控制帧，避免跨线程直接使用 Socket。
void ProcessRuntime::onSendLog(int level, const QString& text)
{
    assertSocketThread();
    QJsonObject frame;
    frame.insert(QStringLiteral("type"), kLog);
    frame.insert(QStringLiteral("level"), level);
    frame.insert(QStringLiteral("text"), text);
    if (!sendFrame(frame))
        finish(4);
}

// ModuleHost 的发布入口：业务线程只进入受锁保护的本地有界队列。
bool ProcessRuntime::queuePublish(const QString& topic, const QByteArray& data)
{
    bool logWarning = false;
    PublishQueue::Stats stats;
    bool accepted = false;
    {
        // 这把指针锁只包住一次非阻塞 enqueue；stop/delete 使用同一把锁。
        QMutexLocker locker(&publishQueueMutex_);
        if (publishQueue_ == nullptr)
            return false;
        ProcessRuntime::TopicSettings config = topicConfig(topic);
        config.maxMessageBytes = qMin(config.maxMessageBytes, maxMessageBytes_);
        accepted = publishQueue_->enqueue(topic, data, config, &logWarning, &stats);
    }
    if (logWarning) {
        queueLog(LogLevel::Warning,
                 QString::fromUtf8(u8"子进程发送队列发生丢弃/拒绝：dropped=%1，rejected=%2")
                     .arg(stats.dropped)
                     .arg(stats.rejected));
    }
    // 返回值只代表“本地发送队列收下了”。跨进程的最终结果稍后由
    // handlePublishAck() 处理，publish() 本身不会同步等待父进程。
    return accepted;
}

// 日志体较小，使用 queued 调用切回 socket_ 所属线程发送。
void ProcessRuntime::queueLog(LogLevel level, const QString& text)
{
    QMetaObject::invokeMethod(
        this,
        "onSendLog",
        Qt::QueuedConnection,
        Q_ARG(int, static_cast<int>(level)),
        Q_ARG(QString, text));
}

// 所有控制帧最终经过同一个编码函数写入 QLocalSocket。
bool ProcessRuntime::sendFrame(const QJsonObject& frame)
{
    assertSocketThread();
    if (socket_->state() != QLocalSocket::ConnectedState)
        return false;
    const QByteArray encoded = process::encodeFrame(frame);
    return socket_->write(encoded) == encoded.size();
}

// 向父进程确认“父到子消息是否进入本地输入队列”。
bool ProcessRuntime::sendDeliveryAck(const QString& messageId, bool accepted)
{
    if (messageId.isEmpty())
        return true;
    QJsonObject ack;
    // ACK 很小，只包含 ID 和结果；它不携带 payload，因此不会反向扩大队列。
    ack.insert(QStringLiteral("type"), kDeliveryAck);
    ack.insert(QStringLiteral("messageId"), messageId);
    ack.insert(QStringLiteral("accepted"), accepted);
    return sendFrame(ack);
}

// 处理“子到父消息”的最终结果，并回收在途计数和共享内存。
void ProcessRuntime::handlePublishAck(const QJsonObject& frame)
{
    const QString messageId = frame.value(QStringLiteral("messageId")).toString();
    if (messageId.isEmpty() || publishQueue_ == nullptr)
        return;
    const bool accepted = frame.value(QStringLiteral("type")).toString() == kSharedAck
        ? true
        : frame.value(QStringLiteral("accepted")).toBool();
    const bool known = publishQueue_->acknowledge(messageId, accepted);
    // 无论父进程接受还是拒绝，ACK 都是共享段生命周期的结束信号。
    // known=false 通常表示重复/迟到 ACK，此时不会再次减少计数。
    QSharedMemory* shared = outgoingSharedSegments_.take(messageId);
    if (shared != nullptr) {
        shared->detach();
        delete shared;
    }
    if (known && !accepted) {
        ++publishRejectedCount_;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - lastPublishRejectWarningMs_ >= 1000) {
            lastPublishRejectWarningMs_ = now;
            queueLog(LogLevel::Warning,
                     QString::fromUtf8(u8"父进程拒绝消息，累计 %1 条")
                         .arg(publishRejectedCount_));
        }
    }
    if (publishQueue_->scheduleIfNeeded(topicConfigs_, topicConfig(QString())))
        QMetaObject::invokeMethod(this,
                                  "drainPublishQueue",
                                  Qt::QueuedConnection);
}

// 记录退出原因、停止新发布并请求 Qt 事件循环退出；实际资源由析构回收。
void ProcessRuntime::finish(int exitCode)
{
    // finish 可能由断线、坏帧或正常 stop 触发；统一关闭 publish 队列，
    // 让后续 publish() 立即失败，并由析构路径释放剩余共享段。
    exitCode_ = exitCode;
    stopping_ = true;
    running_ = false;
    if (module_ != nullptr)
        module_->setRunning(false);
    stopPublishQueue(QString(), false);
    if (application_ != nullptr)
        application_->quit();
}

bool ProcessRuntime::prepareForExit()
{
    stopPublishQueue(QString(), false);
    if (module_ != nullptr)
        module_->setRunning(false);
    running_ = false;
    if (unsafeMessageThread_)
        return false;
    if (messageQueue_ != nullptr && messageQueue_->isRunning() &&
        messageQueue_->stopAndDrain(shutdownDrainTimeoutMs_) ==
            MessageQueue::StopResult::TimedOut) {
        unsafeMessageThread_ = true;
        if (exitCode_ == 0)
            exitCode_ = 7;
        return false;
    }
    return true;
}

// 清理断线/停止时仍未收到 ACK 的所有子进程所有共享段。
void ProcessRuntime::clearSharedSegments()
{
    const QList<QSharedMemory*> segments = outgoingSharedSegments_.values();
    outgoingSharedSegments_.clear();
    for (QSharedMemory* shared : segments) {
        // 进程即将退出时没有机会再等 ACK；所有仍由本进程持有的段都在这里
        // 主动 detach，避免异常断线把系统共享内存对象遗留到下一次重启。
        shared->detach();
        delete shared;
    }
}
}
