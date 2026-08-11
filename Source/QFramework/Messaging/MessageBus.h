#pragma once

// 初学者阅读提示：MessageBus 是进程内的“邮局”。模块 publish 一条消息后，总线
// 根据主题声明把同一个 Qt 值载荷分别放进每个订阅模块的专属 ModuleQueue；模块
// 自己的队列线程再串行调用 onMessage。一个模块处理慢，只会耗尽自己的容量。
//
// 两种队列策略：Reliable 满时拒绝新消息，旧消息一条不覆盖；Latest 满时只删除
// 同主题最老的等待项，再保留最新项，其他主题和顺序不受影响。ModuleQueueStats
// 的 delivered/dropped/rejected 分别记录已回调、策略丢弃、准入拒绝，拒绝日志另按
// “模块+原因”每秒限频，不改变 rejected 计数和队列行为。
//
// 线程与所有权：MessageBus 的注册表由 mutex_ 保护；每个 ModuleQueue 是一条 QThread，
// endpoint 是借用指针，Registration/queue 由 MessageBus 创建并回收。停止先禁止发布，
// 再唤醒队列；所有等待有总 deadline，超时模块会 quarantine，不能被立即删除，避免
// 正在执行的 onMessage 使用悬空对象。没有 BlockingQueuedConnection。

#include <QHash>
#include <QMutex>
#include <QStringList>

#include "FrameworkConfig.h"
#include "ModuleEndpoint.h"
#include "QFrameworkGlobal.h"

namespace qframework
{
enum class ModuleQueueStopResult
{
    Stopped,
    TimedOut
};

struct MessageBusStopReport
{
    QStringList stoppedModuleIds;
    QStringList timedOutModuleIds;

    bool allStopped() const { return timedOutModuleIds.isEmpty(); }
    explicit operator bool() const { return allStopped(); }
};

struct ModuleQueueStats
{
    // delivered 是已调用 onMessage 的数量；dropped 是 Latest/停止丢弃数量；
    // rejected 是 Reliable 满、停止或非法请求拒绝数量。
    quint64 delivered = 0;
    quint64 dropped = 0;
    quint64 rejected = 0;
};

class QFRAMEWORK_EXPORT MessageBus : public ModuleHost
{
public:
    // config 按值保存，保证总线运行期间配置快照稳定。
    explicit MessageBus(const MessageBusConfig& config);
    ~MessageBus() override;

    // 注册主题声明并启动模块专属队列；重复 ID 或空主题会失败。
    bool registerModule(const QString& moduleId,
                        ModuleEndpoint* endpoint,
                        QString* errorMessage = nullptr);
    // 设置 publish 是否可用，不等于是否已经收到消息。
    bool setModuleRunning(const QString& moduleId, bool running);
    // 立即关闭单个模块的发布和接收入口；线程回收留给后续有界停止流程。
    bool beginModuleStop(const QString& moduleId, bool discardPendingMessages);
    // 启动阶段先暂停队列，所有模块注册完成后一次性开启投递。
    void setDeliveryEnabled(bool enabled);
    // 关闭发布入口并唤醒所有暂停队列。
    void beginShutdown();
    // 在一个总预算内停止全部队列，并区分已停止与仍在回调中的模块。
    MessageBusStopReport stopQueues(int drainTimeoutMs);
    // 移除一个模块；drainPendingMessages 决定停止前是否保留待处理消息。
    bool unregisterModule(const QString& moduleId, bool drainPendingMessages);

    // 以下查询只返回快照，不把内部锁或队列所有权交给调用方。
    QStringList moduleIds() const;
    ModuleQueueStats queueStats(const QString& moduleId) const;
    bool isModuleQueueStopped(const QString& moduleId) const;
    QStringList quarantinedModuleIds() const;

    // ModuleEndpoint::publish 的宿主实现：校验发布权限和大小后，
    // 把同一消息复制到所有订阅者各自的有界队列。
    bool publishFromModule(const QString& moduleId,
                           const QString& topic,
                           const QByteArray& data) override;
    // ModuleEndpoint::publishShared 的主进程宿主实现；校验完成后把同一个
    // MessagePayload 引用放入所有订阅者队列，不复制 QByteArray 对象。
    bool publishSharedFromModule(const QString& moduleId,
                                 const QString& topic,
                                 const MessagePayload& payload) override;
    // ModuleEndpoint 的日志宿主实现，转交单例 Logger。
    void logFromModule(LogLevel level,
                       const QString& moduleId,
                       const QString& text) override;

private:
    struct Registration;
    struct RejectionLogState
    {
        // 所有字段只在 MessageBus::mutex_ 保护下访问；重复拒绝只累计，不创建
        // 新线程或等待磁盘，下一次允许记录时把 suppressed 合并到一条摘要日志。
        qint64 lastLoggedUtcMs = 0;
        quint64 suppressed = 0;
    };

    TopicConfig topicConfig(const QString& topic) const;
    // 调用方已持有 mutex_；日志本身仍走 Logger 的异步队列，避免每次拒绝同步写盘。
    void logRejected(const QString& moduleId, const QString& reason) const;

    // mutex_ 保护模块注册表和 accepting/deliveryEnabled 状态。
    MessageBusConfig config_;
    mutable QMutex mutex_;
    QHash<QString, Registration*> modules_;
    // key 是“模块 + 原因”的稳定组合；1 秒窗口内不重复输出 Warning，但 rejected
    // 统计仍由 ModuleQueue 每次拒绝递增，可靠/最新消息策略完全不受影响。
    mutable QHash<QString, RejectionLogState> rejectionLogs_;
    bool accepting_;
    bool deliveryEnabled_;
    bool stopQueuesRequested_;
};
}
