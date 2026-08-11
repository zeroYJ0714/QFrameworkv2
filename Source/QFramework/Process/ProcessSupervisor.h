#pragma once

// 文件职责：声明主进程子进程监督器。
// 它拥有 QProcess/QLocalServer/QLocalSocket，并把 MessageBus 消息放入按主题
// 有界队列；心跳、注册、重启、窗口句柄和 ACK 都在同一 Qt 线程协调。

#include <QProcess>
#include <QDeadlineTimer>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QSet>
#include <QTimer>
#include <QVector>

#include "FrameworkConfig.h"
#include "MessagePayload.h"
#include "QFrameworkGlobal.h"

namespace qframework
{
class MessageBus;

struct ProcessQueueStats
{
    quint64 dropped = 0; ///< `dropped` 对应的数值配置、计数或时间状态。
    quint64 rejected = 0; ///< `rejected` 对应的数值配置、计数或时间状态。
    quint64 abandoned = 0; ///< `abandoned` 对应的数值配置、计数或时间状态。
    int pending = 0; ///< `pending` 对应的数值配置、计数或时间状态。
    int inFlight = 0; ///< `inFlight` 对应的数值配置、计数或时间状态。
};

class QFRAMEWORK_EXPORT ProcessSupervisor : public QObject
{
    Q_OBJECT

public:
    // messageBus 是借用指针；配置按值保存，避免启动后被外部修改。
    /// @brief 创建 ProcessSupervisor 对象并初始化其基础状态。
    /// @param messageBus 传给该操作的 `messageBus` 参数；取值应符合声明类型和函数用途。
    /// @param messageBusConfig MessageBus 配置值。
    /// @param processConfig 子进程监督配置值。
    /// @param parent Qt 父对象或父控件；用于建立所有权关系，允许为空。
    /// @return 无（构造函数不返回值）。
    explicit ProcessSupervisor(MessageBus* messageBus,
                               const MessageBusConfig& messageBusConfig,
                               const ProcessConfig& processConfig,
                               QObject* parent = nullptr); ///< `parent` 对应的对象指针；所有权和线程归属见本类文件级说明。
    /// @brief 销毁 ProcessSupervisor 对象并释放其拥有的资源。
    /// @return 无（析构函数不返回值）。
    ~ProcessSupervisor() override;

    // 启动所有 ProcessUi/ProcessNonUi 模块；失败模块写入 errors 但继续处理其他模块。
    /// @brief 启动 `startAll` 对应的框架操作。
    /// @param modules 模块配置列表。
    /// @param errors 可选错误输出列表。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool startAll(const QVector<ModuleConfig>& modules,
                  QStringList* errors = nullptr); ///< `errors` 对应的对象指针；所有权和线程归属见本类文件级说明。
    // 兼容入口只提交异步请求，不再在调用线程等待子进程。
    /// @brief 停止 `stop` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool stop(const QString& moduleId, QString* errorMessage = nullptr);
    /// @brief 重启 `restart` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool restart(const QString& moduleId, QString* errorMessage = nullptr);
    /// @brief 请求 `requestStop` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool requestStop(const QString& moduleId, QString* errorMessage = nullptr);
    /// @brief 请求 `requestRestart` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool requestRestart(const QString& moduleId, QString* errorMessage = nullptr);
    /// @brief 显示 `showWindow` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool showWindow(const QString& moduleId, QString* errorMessage = nullptr);
    /// @brief 显示 `showWindow` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param width 目标宽度，单位像素。
    /// @param height 目标高度，单位像素。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool showWindow(const QString& moduleId,
                    int width,
                    int height,
                    QString* errorMessage = nullptr); ///< `errorMessage` 对应的对象指针；所有权和线程归属见本类文件级说明。
    // 只调整已嵌入 ProcessUi 的客户区，不改变子进程显示状态。
    /// @brief 调整 `resizeWindow` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param width 目标宽度，单位像素。
    /// @param height 目标高度，单位像素。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool resizeWindow(const QString& moduleId,
                      int width,
                      int height,
                      QString* errorMessage = nullptr); ///< `errorMessage` 对应的对象指针；所有权和线程归属见本类文件级说明。
    // 强制结束用于故障注入和监督器上层的紧急停止；正常关闭应使用 stop/shutdown。
    /// @brief 终止 `terminate` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool terminate(const QString& moduleId);
    /// @brief 关闭 `shutdown` 对应的框架操作。
    /// @return 无。
    void shutdown();

    // 查询只返回状态快照；Entry 的所有权始终属于监督器。
    /// @brief 执行 `runningModuleIds` 所定义的类职责。
    /// @return 返回对应 Qt 值或容器的安全副本。
    QStringList runningModuleIds() const;
    /// @brief 执行 `state` 所定义的类职责。
    /// @param moduleId 稳定模块 ID。
    /// @return 返回对应 Qt 值或容器的安全副本。
    QString state(const QString& moduleId) const;
    // 返回父到子有界队列的只读快照，供诊断和边界测试使用。
    /// @brief 排队 `queueStats` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @return 返回声明类型的结果值。
    ProcessQueueStats queueStats(const QString& moduleId) const;

signals:
    // UI 通过这些信号显示生命周期和故障，不直接访问 Entry。
    /// @brief 执行 `moduleStateChanged` 所定义的类职责。
    /// @param moduleId 稳定模块 ID。
    /// @param state 状态值。
    /// @param detail 状态或故障详情。
    /// @return 无。
    void moduleStateChanged(const QString& moduleId,
                            const QString& state,
                            const QString& detail);
    /// @brief 执行 `moduleFault` 所定义的类职责。
    /// @param moduleId 稳定模块 ID。
    /// @param detail 状态或故障详情。
    /// @return 无。
    void moduleFault(const QString& moduleId, const QString& detail);
    /// @brief 执行 `windowHandleReady` 所定义的类职责。
    /// @param moduleId 稳定模块 ID。
    /// @param windowId 传给该操作的 `windowId` 参数；取值应符合声明类型和函数用途。
    /// @return 无。
    void windowHandleReady(const QString& moduleId, quintptr windowId);
    /// @brief 重启 `restartFinished` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param success 传给该操作的 `success` 参数；取值应符合声明类型和函数用途。
    /// @param detail 状态或故障详情。
    /// @return 无。
    void restartFinished(const QString& moduleId, bool success, const QString& detail);
    /// @brief 执行 `operationBusyChanged` 所定义的类职责。
    /// @param moduleId 稳定模块 ID。
    /// @param busy 目标忙碌状态。
    /// @return 无。
    void operationBusyChanged(const QString& moduleId, bool busy);
    /// @brief 启动 `startupBatchFinished` 对应的框架操作。
    /// @param errors 可选错误输出列表。
    /// @return 无。
    void startupBatchFinished(const QStringList& errors);

public slots:
    /// @brief 应用 `applyStyleSheet` 对应的框架操作。
    /// @param styleSheet 完整 QSS 文本。
    /// @return 无。
    void applyStyleSheet(const QString& styleSheet);

private slots:
    // 有新子进程连接时接受并校验对应 Entry 的本地服务器连接。
    /// @brief 处理 `onServerConnection` 对应的框架操作。
    /// @return 无。
    void onServerConnection();
    // 累积并解析来自任一子进程的完整协议帧。
    /// @brief 处理 `onSocketReadyRead` 对应的框架操作。
    /// @return 无。
    void onSocketReadyRead();
    // 连接断开时进入故障或正常停止清理流程。
    /// @brief 处理 `onSocketDisconnected` 对应的框架操作。
    /// @return 无。
    void onSocketDisconnected();
    // QProcess 报告启动/运行错误时转换为模块故障信号。
    /// @brief 处理 `onProcessStarted` 对应的框架操作。
    /// @return 无。
    void onProcessStarted();
    /// @brief 处理 `onProcessError` 对应的框架操作。
    /// @param error 错误对象或错误输出参数。
    /// @return 无。
    void onProcessError(QProcess::ProcessError error);
    // 进程退出后统一判断是否预期、是否需要重启。
    /// @brief 处理 `onProcessFinished` 对应的框架操作。
    /// @param exitCode 传给该操作的 `exitCode` 参数；取值应符合声明类型和函数用途。
    /// @param exitStatus 传给该操作的 `exitStatus` 参数；取值应符合声明类型和函数用途。
    /// @return 无。
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    // 周期检查注册 deadline、心跳、延迟重启和停止超时。
    /// @brief 处理 `onSupervisionTick` 对应的框架操作。
    /// @return 无。
    void onSupervisionTick();
    // 由合并后的 queued 唤醒执行有限批次发送；不是“每条消息一个事件”。
    /// @brief 排空 `drainChildQueue` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @return 无。
    void drainChildQueue(const QString& moduleId);

private:
    friend class ProcessBridge;
    enum class LifecyclePhase
    {
        Stopped,
        StartingProcess,
        WaitingRegistration,
        WaitingStarted,
        Running,
        StopRequested,
        WaitingStopAck,
        WaitingProcessExit,
        RestartDelay,
        Failed
    };
    enum class StopPurpose
    {
        None,
        Stop,
        ManualRestart,
        AutoRestart,
        Shutdown
    };
    struct Entry;

    // 以下查找函数都只返回监督器拥有的 Entry 借用指针。
    /// @brief 查找 `findEntry` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @return 返回借用指针；找不到目标时返回 nullptr。
    Entry* findEntry(const QString& moduleId) const;
    /// @brief 查找 `findEntryByServer` 对应的框架操作。
    /// @param object 用于反查所属记录的 QObject 指针。
    /// @return 返回借用指针；找不到目标时返回 nullptr。
    Entry* findEntryByServer(QObject* object) const;
    /// @brief 查找 `findEntryBySocket` 对应的框架操作。
    /// @param object 用于反查所属记录的 QObject 指针。
    /// @return 返回借用指针；找不到目标时返回 nullptr。
    Entry* findEntryBySocket(QObject* object) const;
    /// @brief 查找 `findEntryByProcess` 对应的框架操作。
    /// @param object 用于反查所属记录的 QObject 指针。
    /// @return 返回借用指针；找不到目标时返回 nullptr。
    Entry* findEntryByProcess(QObject* object) const;
    // 异步建立服务端和 QProcess；started/register/started-frame 信号继续推进。
    /// @brief 开始 `beginStartEntry` 对应的框架操作。
    /// @param entry 监督器拥有的子进程记录借用指针。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool beginStartEntry(Entry* entry, QString* errorMessage);
    /// @brief 开始 `beginStopEntry` 对应的框架操作。
    /// @param entry 监督器拥有的子进程记录借用指针。
    /// @param purpose 停止操作目的。
    /// @return 无。
    void beginStopEntry(Entry* entry, StopPurpose purpose);
    /// @brief 关闭 `closeEntryIngress` 对应的框架操作。
    /// @param entry 监督器拥有的子进程记录借用指针。
    /// @return 无。
    void closeEntryIngress(Entry* entry);
    /// @brief 执行 `advanceStopEntry` 所定义的类职责。
    /// @param entry 监督器拥有的子进程记录借用指针。
    /// @return 无。
    void advanceStopEntry(Entry* entry);
    /// @brief 执行 `finishEntryAfterProcessExit` 所定义的类职责。
    /// @param entry 监督器拥有的子进程记录借用指针。
    /// @param detail 状态或故障详情。
    /// @return 无。
    void finishEntryAfterProcessExit(Entry* entry, const QString& detail);
    /// @brief 设置 `setOperationBusy` 对应的框架操作。
    /// @param entry 监督器拥有的子进程记录借用指针。
    /// @param busy 目标忙碌状态。
    /// @return 无。
    void setOperationBusy(Entry* entry, bool busy);
    /// @brief 设置 `settleStartupEntry` 对应的框架操作。
    /// @param entry 监督器拥有的子进程记录借用指针。
    /// @param error 错误对象或错误输出参数。
    /// @return 无。
    void settleStartupEntry(Entry* entry, const QString& error);
    /// @brief 执行 `scheduleStartupBatchFinished` 所定义的类职责。
    /// @return 无。
    void scheduleStartupBatchFinished();
    /// @brief 检查 `isStoppingPhase` 对应的框架操作。
    /// @param phase 生命周期阶段。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool isStoppingPhase(LifecyclePhase phase) const;
    /// @brief 检查 `isRegisteredPhase` 对应的框架操作。
    /// @param phase 生命周期阶段。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool isRegisteredPhase(LifecyclePhase phase) const;
    // 编码并写入已经认证的子进程 Socket。
    /// @brief 发送 `sendFrame` 对应的框架操作。
    /// @param entry 监督器拥有的子进程记录借用指针。
    /// @param frame 协议帧或媒体帧值。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool sendFrame(Entry* entry, const QJsonObject& frame);
    // 将 MessageBus 回调放入父到子有界队列。返回值只表示是否进入等待队列。
    /// @brief 执行 `enqueueMessageToChild` 所定义的类职责。
    /// @param moduleId 稳定模块 ID。
    /// @param topic 消息主题名称。
    /// @param senderModuleId 发送模块 ID。
    /// @param payload 不可变或独立拥有的消息载荷。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool enqueueMessageToChild(const QString& moduleId,
                               const QString& topic,
                               const QString& senderModuleId,
                               const MessagePayload& payload);
    // 处理子进程 deliveryAck，并回收在途计数及可选共享内存。
    /// @brief 执行 `acknowledgeChildMessage` 所定义的类职责。
    /// @param entry 监督器拥有的子进程记录借用指针。
    /// @param messageId 消息关联 ID。
    /// @param accepted 消息是否被接受。
    /// @return 无。
    void acknowledgeChildMessage(Entry* entry,
                                 const QString& messageId,
                                 bool accepted);
    // 本地尚未写入 Socket 即失败时回收槽位并计为 dropped，而不是远端拒绝。
    /// @brief 执行 `discardChildMessageBeforeSend` 所定义的类职责。
    /// @param entry 监督器拥有的子进程记录借用指针。
    /// @param messageId 消息关联 ID。
    /// @return 无。
    void discardChildMessageBeforeSend(Entry* entry, const QString& messageId);
    // 根据 type 分派注册、心跳、ACK、窗口和业务消息。
    /// @brief 处理 `handleFrame` 对应的框架操作。
    /// @param entry 监督器拥有的子进程记录借用指针。
    /// @param frame 协议帧或媒体帧值。
    /// @return 无。
    void handleFrame(Entry* entry, const QJsonObject& frame);
    // 标记故障、清理资源，并按重启策略安排下一次启动。
    /// @brief 处理 `handleFault` 对应的框架操作。
    /// @param entry 监督器拥有的子进程记录借用指针。
    /// @param detail 状态或故障详情。
    /// @return 无。
    void handleFault(Entry* entry, const QString& detail);
    // 释放一轮运行时对象并清空所有等待/在途/共享内存状态。
    /// @brief 执行 `destroyRuntime` 所定义的类职责。
    /// @param entry 监督器拥有的子进程记录借用指针。
    /// @return 无。
    void destroyRuntime(Entry* entry);
    // 在锁外 detach/delete 所有未完成的大消息共享段。
    /// @brief 清理 `clearOutgoingShared` 对应的框架操作。
    /// @param entry 监督器拥有的子进程记录借用指针。
    /// @return 无。
    void clearOutgoingShared(Entry* entry);
    /// @brief 执行 `emitState` 所定义的类职责。
    /// @param entry 监督器拥有的子进程记录借用指针。
    /// @param state 状态值。
    /// @param detail 状态或故障详情。
    /// @return 无。
    void emitState(Entry* entry, const QString& state, const QString& detail);

    // entries_、socket 和进程对象均由监督器拥有；messageBus_ 只是借用。
    // messageBus_ 借用；Entry、Socket、QProcess 和定时器由本类拥有。
    MessageBus* messageBus_; ///< `messageBus_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    MessageBusConfig messageBusConfig_; ///< 保存 `messageBusConfig` 对应的对象状态或配置值。
    ProcessConfig processConfig_; ///< 保存 `processConfig` 对应的对象状态或配置值。
    QVector<Entry*> entries_; ///< `entries_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QTimer* supervisionTimer_; ///< `supervisionTimer_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QString styleSheet_; ///< 保存 `styleSheet` 对应的对象状态或配置值。
    // shutdown 后阻止新连接、重启和队列入队。
    bool shuttingDown_; ///< 全局关闭门禁；置位后拒绝新工作。
    QSet<QString> startupPendingModules_; ///< 保存 `startupPendingModules` 相关值的 Qt 容器。
    QStringList startupErrors_; ///< 保存 `startupErrors` 相关值的 Qt 容器。
    bool startupBatchActive_; ///< `startupBatchActive_` 对应的布尔状态标志。
    bool startupBatchSignalScheduled_; ///< `startupBatchSignalScheduled_` 对应的布尔状态标志。
};
}
