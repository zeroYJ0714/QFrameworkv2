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
    Stopped, ///< 投递线程已在预算内结束，Registration 可以安全回收。
    TimedOut ///< 等待超时，线程可能仍在执行 onMessage，只能标记隔离保留。
};

struct MessageBusStopReport
{
    QStringList stoppedModuleIds; ///< 已在停止预算内结束投递线程的模块 ID。
    QStringList timedOutModuleIds; ///< 等待超时、可能仍在执行 onMessage 的模块 ID；这类模块必须隔离保留。

    /// @brief 判断是否所有模块队列都已在预算内停止。
    /// @return 无超时模块返回 true；timedOutModuleIds 非空返回 false。
    bool allStopped() const { return timedOutModuleIds.isEmpty(); }
    /// @brief 布尔上下文判断，等价于 allStopped()。
    /// @return 无超时模块返回 true，可直接用于 if (!report) 这类判断。
    explicit operator bool() const { return allStopped(); }
};

struct ModuleQueueStats
{
    quint64 delivered = 0; ///< 已完成 onMessage 调用的消息条数，回调抛异常也计入。
    quint64 dropped = 0; ///< 被 Latest 淘汰的同主题最旧等待项，以及停止时清空队列丢弃的条数。
    quint64 rejected = 0; ///< 未被队列接受的消息条数：Reliable 已满或入队入口已关闭时递增。
};

class QFRAMEWORK_EXPORT MessageBus : public ModuleHost
{
public:
    /// @brief 创建消息总线：保存配置快照，此时允许注册模块，但消息投递保持暂停。
    /// @param config 消息总线配置快照，按值保存、运行期不再变化，提供默认容量、单条消息上限与停止预算。
    /// @return 无（构造函数不返回值）。
    explicit MessageBus(const MessageBusConfig& config);
    /// @brief 析构兜底：先 beginShutdown()，再在配置预算内停止全部队列，只回收未超时的模块注册。
    /// @return 无（析构函数不返回值）；停止超时的模块连同其投递线程故意不释放，避免仍在执行的 onMessage 访问已销毁对象。
    ~MessageBus() override;

    /// @brief 注册模块：冻结其发布/订阅主题声明，创建专属投递线程并把端点绑定到本总线。
    /// @param moduleId 模块唯一 ID，去掉首尾空白后不能为空，也不能与已注册模块重复。
    /// @param endpoint 模块端点借用指针，不能为空；总线只调用其主题声明与回调接口，不拥有它。
    /// @param errorMessage 可选的失败原因输出参数，可为空；仅在返回 false 时写入。
    /// @return 注册成功返回 true；ID 为空、端点为 nullptr、主题声明含空项或重复、总线正在停止、ID 已存在时返回 false。
    bool registerModule(const QString& moduleId,
                        ModuleEndpoint* endpoint,
                        QString* errorMessage = nullptr);
    /// @brief 开关模块的发布门禁并同步给端点；它只决定能否 publish，不代表模块已启动或已收到消息。
    /// @param moduleId 目标模块 ID。
    /// @param running true 表示允许发布，false 表示立即禁止；总线正在停止时传 true 也会降级为禁止。
    /// @return 模块已注册返回 true；模块不存在返回 false。
    bool setModuleRunning(const QString& moduleId, bool running);
    /// @brief 立即关闭单个模块的发布与接收入口；不等待其投递线程结束，线程回收留给后续有界停止流程。
    /// @param moduleId 目标模块 ID。
    /// @param discardPendingMessages true 表示清空尚未消费的等待项并计入 dropped，false 表示继续排空已有消息。
    /// @return 模块已注册返回 true；模块不存在返回 false。
    bool beginModuleStop(const QString& moduleId, bool discardPendingMessages);
    /// @brief 统一放行或暂停所有模块的消息消费，不清空已排队消息；启动阶段用它把投递推迟到全部模块注册完成之后。
    /// @param enabled true 表示放行消费，false 表示暂停消费。
    /// @return 无；已排队的消息按原顺序保留。
    void setDeliveryEnabled(bool enabled);
    /// @brief 全局关闭发布入口并解除消费暂停，让已排队消息有机会在停止预算内排空。
    /// @return 无；不等待任何线程结束，随后应调用 stopQueues()。
    void beginShutdown();
    /// @brief 用一份共享的 deadline 停止全部模块队列，并区分已停止与仍在回调中的模块。
    /// @param drainTimeoutMs 所有队列共享的总排空预算，单位毫秒；非正值按 1 毫秒处理。
    /// @return 停止报告：stoppedModuleIds 为已结束的模块，timedOutModuleIds 为仍可能回调、必须隔离保留的模块。
    MessageBusStopReport stopQueues(int drainTimeoutMs);
    /// @brief 注销模块：关闭入口、有界等待投递线程结束，然后解绑端点并释放注册记录与其队列。
    /// @param moduleId 目标模块 ID。
    /// @param drainPendingMessages true 表示先尽量排空待处理消息再停止，false 表示立即丢弃等待项。
    /// @return 注销成功返回 true；模块不存在、或线程在配置预算内未停（已标记隔离保留）返回 false。
    bool unregisterModule(const QString& moduleId, bool drainPendingMessages);

    // 以下查询只返回快照，不把内部锁或队列所有权交给调用方。
    /// @brief 取当前已注册模块 ID 的快照。
    /// @return 模块 ID 列表，顺序由 QHash 决定，不代表配置或启动顺序。
    QStringList moduleIds() const;
    /// @brief 查询指定模块队列的投递计数快照。
    /// @param moduleId 目标模块 ID。
    /// @return delivered/dropped/rejected 的值副本；模块不存在时返回全零默认值。
    ModuleQueueStats queueStats(const QString& moduleId) const;
    /// @brief 判断指定模块的投递线程是否已经结束。
    /// @param moduleId 目标模块 ID。
    /// @return 线程已结束、或模块从未注册（已注销）返回 true，线程仍在运行时返回 false。
    bool isModuleQueueStopped(const QString& moduleId) const;
    /// @brief 列出停止超时、仍需隔离保留的模块 ID。
    /// @return 已标记隔离且投递线程仍在运行的模块 ID 列表；可安全回收的模块不会出现。
    QStringList quarantinedModuleIds() const;

    /// @brief 实现 ModuleEndpoint::publish：把字节数组复制成共享载荷，再走 publishSharedFromModule 的校验与路由。
    /// @param moduleId 发布者模块 ID，必须已注册且当前允许发布。
    /// @param topic 目标主题，必须是该模块注册时声明过的发布主题。
    /// @param data 消息字节内容，可包含 NUL；空数组也会被复制成合法的零字节载荷。
    /// @return true 只表示所有订阅队列都收下了这条消息，不代表订阅方业务处理成功；发布资格、主题或大小不合法、任一订阅队列拒绝时返回 false。
    bool publishFromModule(const QString& moduleId,
                           const QString& topic,
                           const QByteArray& data) override;
    /// @brief 实现 ModuleEndpoint::publishShared：在总线锁内校验发布资格后，把同一份共享载荷放入所有订阅者队列，不复制 QByteArray。
    /// @param moduleId 发布者模块 ID，必须已注册且当前允许发布。
    /// @param topic 目标主题，必须是该模块注册时声明过的发布主题。
    /// @param payload 共享不可变载荷，按引用入队；空载荷（isNull）会被拒绝。
    /// @return 全部订阅者都接受返回 true；总线正在停止、发布者无权、主题未声明、载荷为空、超过该主题字节上限或任一订阅队列拒绝时返回 false，其他订阅者已入队的副本不回滚。
    bool publishSharedFromModule(const QString& moduleId,
                                 const QString& topic,
                                 const MessagePayload& payload) override;
    /// @brief 实现 ModuleEndpoint::log：把模块日志直接交给单例 Logger，不经过消息主题，避免形成递归消息链。
    /// @param level 日志严重级别。
    /// @param moduleId 产生日志的模块 ID，作为日志来源标识。
    /// @param text 日志正文。
    /// @return 无；日志进入 Logger 的异步队列，本函数不等待落盘。
    void logFromModule(LogLevel level,
                       const QString& moduleId,
                       const QString& text) override;

private:
    // Registration 的完整定义在 MessageBus.cpp：它记录总线对单个模块的端点、队列与主题声明。
    struct Registration;
    struct RejectionLogState
    {
        // 所有字段只在 MessageBus::mutex_ 保护下访问；重复拒绝只累计，不创建
        // 新线程或等待磁盘，下一次允许记录时把 suppressed 合并到一条摘要日志。
        qint64 lastLoggedUtcMs = 0; ///< 上次真正写出日志的 UTC 毫秒时间戳，0 表示尚未记录过。
        quint64 suppressed = 0; ///< 上次记录之后被限频抑制掉的同类拒绝次数，随摘要日志清零。
    };
    /// @brief 取指定主题生效的队列规则；主题没有专用配置时用全局默认值补齐。
    /// @param topic 目标主题名，可以是未单独配置过的主题。
    /// @return 完整可用的 TopicConfig（容量、单条字节上限、策略），调用方拿到的是可安全修改的副本。
    TopicConfig topicConfig(const QString& topic) const;
    /// @brief 记录一次发布拒绝，并对同一「模块 + 原因」限频：调用方必须已持有 mutex_，本函数只操作 rejectionLogs_。
    /// @param moduleId 触发拒绝的模块 ID，同时作为日志来源。
    /// @param reason 拒绝原因文本，同时参与限频键的构造。
    /// @return 无；处于 1 秒限频窗口内只累计 suppressed，不写日志，下一次记录时附带被抑制的次数。
    void logRejected(const QString& moduleId, const QString& reason) const;

    MessageBusConfig config_; ///< 构造时按值保存的配置快照，运行期只读，提供默认容量、单条消息上限与停止预算。
    mutable QMutex mutex_; ///< 总线级互斥量，保护 modules_、rejectionLogs_ 和下面三个状态标志。
    QHash<QString, Registration*> modules_; ///< 模块 ID 到注册记录的映射；Registration 及其投递线程由本类创建并释放。
    // 限频只影响 Warning 文本：rejected 统计仍由 ModuleQueue 每次拒绝递增，
    // Reliable 拒绝与 Latest 淘汰行为完全不受影响。
    mutable QHash<QString, RejectionLogState> rejectionLogs_; ///< 限频键（模块 ID + 0x1f + 原因）到抑制状态的映射，总线生命周期内只增不删。
    bool accepting_; ///< 是否仍接受注册与发布；beginShutdown() 置 false 后不再复位。
    bool deliveryEnabled_; ///< 是否放行消费；构造时为 false，全部模块注册完成后由 setDeliveryEnabled(true) 打开。
    bool stopQueuesRequested_; ///< stopQueues() 是否已向各队列下发过停止请求，保证重复调用不重复 beginStop。
};
}
