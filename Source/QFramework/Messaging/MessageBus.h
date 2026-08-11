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
    QStringList stoppedModuleIds; ///< 保存 `stoppedModuleIds` 相关值的 Qt 容器。
    QStringList timedOutModuleIds; ///< 保存 `timedOutModuleIds` 相关值的 Qt 容器。

    bool allStopped() const { return timedOutModuleIds.isEmpty(); }
    explicit operator bool() const { return allStopped(); }
};

struct ModuleQueueStats
{
    // delivered 是已调用 onMessage 的数量；dropped 是 Latest/停止丢弃数量；
    // rejected 是 Reliable 满、停止或非法请求拒绝数量。
    quint64 delivered = 0; ///< `delivered` 对应的数值配置、计数或时间状态。
    quint64 dropped = 0; ///< `dropped` 对应的数值配置、计数或时间状态。
    quint64 rejected = 0; ///< `rejected` 对应的数值配置、计数或时间状态。
};

class QFRAMEWORK_EXPORT MessageBus : public ModuleHost
{
public:
    // config 按值保存，保证总线运行期间配置快照稳定。
    /// @brief 创建 MessageBus 对象并初始化其基础状态。
    /// @param config 运行配置值。
    /// @return 无（构造函数不返回值）。
    explicit MessageBus(const MessageBusConfig& config);
    /// @brief 销毁 MessageBus 对象并释放其拥有的资源。
    /// @return 无（析构函数不返回值）。
    ~MessageBus() override;

    // 注册主题声明并启动模块专属队列；重复 ID 或空主题会失败。
    /// @brief 注册 `registerModule` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param endpoint 传给该操作的 `endpoint` 参数；取值应符合声明类型和函数用途。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool registerModule(const QString& moduleId,
                        ModuleEndpoint* endpoint,
                        QString* errorMessage = nullptr); ///< `errorMessage` 对应的对象指针；所有权和线程归属见本类文件级说明。
    // 设置 publish 是否可用，不等于是否已经收到消息。
    /// @brief 设置 `setModuleRunning` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param running 目标运行状态。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool setModuleRunning(const QString& moduleId, bool running);
    // 立即关闭单个模块的发布和接收入口；线程回收留给后续有界停止流程。
    /// @brief 开始 `beginModuleStop` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param discardPendingMessages 传给该操作的 `discardPendingMessages` 参数；取值应符合声明类型和函数用途。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool beginModuleStop(const QString& moduleId, bool discardPendingMessages);
    // 启动阶段先暂停队列，所有模块注册完成后一次性开启投递。
    /// @brief 设置 `setDeliveryEnabled` 对应的框架操作。
    /// @param enabled 目标启用状态。
    /// @return 无。
    void setDeliveryEnabled(bool enabled);
    // 关闭发布入口并唤醒所有暂停队列。
    /// @brief 开始 `beginShutdown` 对应的框架操作。
    /// @return 无。
    void beginShutdown();
    // 在一个总预算内停止全部队列，并区分已停止与仍在回调中的模块。
    /// @brief 停止 `stopQueues` 对应的框架操作。
    /// @param drainTimeoutMs 停止队列的总排空预算，单位毫秒。
    /// @return 返回声明类型的结果值。
    MessageBusStopReport stopQueues(int drainTimeoutMs);
    // 移除一个模块；drainPendingMessages 决定停止前是否保留待处理消息。
    /// @brief 注销 `unregisterModule` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param drainPendingMessages 传给该操作的 `drainPendingMessages` 参数；取值应符合声明类型和函数用途。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool unregisterModule(const QString& moduleId, bool drainPendingMessages);

    // 以下查询只返回快照，不把内部锁或队列所有权交给调用方。
    /// @brief 执行 `moduleIds` 所定义的类职责。
    /// @return 返回对应 Qt 值或容器的安全副本。
    QStringList moduleIds() const;
    /// @brief 排队 `queueStats` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @return 返回声明类型的结果值。
    ModuleQueueStats queueStats(const QString& moduleId) const;
    /// @brief 检查 `isModuleQueueStopped` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool isModuleQueueStopped(const QString& moduleId) const;
    /// @brief 执行 `quarantinedModuleIds` 所定义的类职责。
    /// @return 返回对应 Qt 值或容器的安全副本。
    QStringList quarantinedModuleIds() const;

    // ModuleEndpoint::publish 的宿主实现：校验发布权限和大小后，
    // 把同一消息复制到所有订阅者各自的有界队列。
    /// @brief 发布 `publishFromModule` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param topic 消息主题名称。
    /// @param data 原始字节数据，可包含 NUL。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool publishFromModule(const QString& moduleId,
                           const QString& topic,
                           const QByteArray& data) override; ///< 保存 `override` 对应的对象状态或配置值。
    // ModuleEndpoint::publishShared 的主进程宿主实现；校验完成后把同一个
    // MessagePayload 引用放入所有订阅者队列，不复制 QByteArray 对象。
    /// @brief 发布 `publishSharedFromModule` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param topic 消息主题名称。
    /// @param payload 不可变或独立拥有的消息载荷。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool publishSharedFromModule(const QString& moduleId,
                                 const QString& topic,
                                 const MessagePayload& payload) override; ///< 保存 `override` 对应的对象状态或配置值。
    // ModuleEndpoint 的日志宿主实现，转交单例 Logger。
    /// @brief 记录 `logFromModule` 对应的框架操作。
    /// @param level 日志严重级别。
    /// @param moduleId 稳定模块 ID。
    /// @param text 待显示、发送或记录的文本。
    /// @return 无。
    void logFromModule(LogLevel level,
                       const QString& moduleId,
                       const QString& text) override; ///< 保存 `override` 对应的对象状态或配置值。

private:
    struct Registration;
    struct RejectionLogState
    {
        // 所有字段只在 MessageBus::mutex_ 保护下访问；重复拒绝只累计，不创建
        // 新线程或等待磁盘，下一次允许记录时把 suppressed 合并到一条摘要日志。
        qint64 lastLoggedUtcMs = 0; ///< `lastLoggedUtcMs` 对应的数值配置、计数或时间状态。
        quint64 suppressed = 0; ///< `suppressed` 对应的数值配置、计数或时间状态。
    };

    /// @brief 执行 `topicConfig` 所定义的类职责。
    /// @param topic 消息主题名称。
    /// @return 返回声明类型的结果值。
    TopicConfig topicConfig(const QString& topic) const;
    // 调用方已持有 mutex_；日志本身仍走 Logger 的异步队列，避免每次拒绝同步写盘。
    /// @brief 记录 `logRejected` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param reason 操作原因。
    /// @return 无。
    void logRejected(const QString& moduleId, const QString& reason) const;

    // mutex_ 保护模块注册表和 accepting/deliveryEnabled 状态。
    MessageBusConfig config_; ///< 当前对象持有的配置副本。
    mutable QMutex mutex_; ///< 保护同一注释分组内共享状态的互斥量。
    QHash<QString, Registration*> modules_; ///< `modules_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    // key 是“模块 + 原因”的稳定组合；1 秒窗口内不重复输出 Warning，但 rejected
    // 统计仍由 ModuleQueue 每次拒绝递增，可靠/最新消息策略完全不受影响。
    mutable QHash<QString, RejectionLogState> rejectionLogs_; ///< 保存 `rejectionLogs` 相关值的 Qt 容器。
    bool accepting_; ///< `accepting_` 对应的布尔状态标志。
    bool deliveryEnabled_; ///< `deliveryEnabled_` 对应的布尔状态标志。
    bool stopQueuesRequested_; ///< `stopQueuesRequested_` 对应的布尔状态标志。
};
}
