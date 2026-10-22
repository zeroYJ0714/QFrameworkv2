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

// 父到子发送队列的诊断快照，由 queueStats() 返回，只用于观测，不参与控制。
struct ProcessQueueStats
{
    quint64 dropped = 0; ///< 因 Latest 覆盖、停止时清队或写入 Socket 前失败而丢弃的累计条数。
    quint64 rejected = 0; ///< 因载荷超限、已停止或 Reliable 队列已满而被本地拒绝的累计条数。
    quint64 abandoned = 0; ///< 清理本轮运行时仍未收到 deliveryAck、被直接放弃的累计条数。
    int pending = 0; ///< 仍留在等待队列、尚未写入 Socket 的条数。
    int inFlight = 0; ///< 已写入 Socket 但还没收到 deliveryAck 的条数。
};

class QFRAMEWORK_EXPORT ProcessSupervisor : public QObject
{
    Q_OBJECT

public:
    /// @brief 保存借用的消息总线和两份配置副本，创建并启动状态机心跳定时器；此时还没有任何子进程，要启动模块需再调 startAll。
    /// @param messageBus 消息总线借用指针，用于注册子进程桥接器；必须比本对象活得更久，本类不负责销毁。
    /// @param messageBusConfig 消息总线配置副本，决定各主题队列容量、消息大小上限和 Latest/Reliable 策略。
    /// @param processConfig 子进程监督配置副本，提供心跳间隔、注册/停止超时和重启窗口策略。
    /// @param parent Qt 父对象（通常是 FrameworkRuntime），可为空；子进程相关对象都以本对象为父。
    /// @return 无（构造函数不返回值）。
    explicit ProcessSupervisor(MessageBus* messageBus,
                               const MessageBusConfig& messageBusConfig,
                               const ProcessConfig& processConfig,
                               QObject* parent = nullptr);
    /// @brief 析构时先幂等调用 shutdown 停掉全部子进程，再删除所有 Entry 记录。
    /// @return 无（析构函数不返回值）。
    ~ProcessSupervisor() override;

    /// @brief 为每个启用的 ProcessUi/ProcessNonUi 模块建立 Entry 并异步提交启动；单个模块失败只写入 errors，不中断其余模块。
    /// @param modules 模块配置列表；enabled 为 false 或类型不属于子进程的项会被跳过。
    /// @param errors 可选输出参数，接收重复 ID 和启动提交失败的中文原因，可为空。
    /// @return 全部启动请求都被接受返回 true；存在重复 ID 或某个模块提交失败返回 false（其余模块仍继续启动，最终结果看 startupBatchFinished）。
    bool startAll(const QVector<ModuleConfig>& modules,
                  QStringList* errors = nullptr);
    /// @brief 兼容入口：直接转交 requestStop 提交异步停止请求，不在调用线程等待子进程退出。
    /// @param moduleId 目标模块的稳定 ID。
    /// @param errorMessage 可选输出参数，失败时写入中文原因，可为空。
    /// @return 请求被接受返回 true；模块不存在或已在停止/重启流程中返回 false。
    bool stop(const QString& moduleId, QString* errorMessage = nullptr);
    /// @brief 兼容入口：直接转交 requestRestart 提交异步重启请求，不等待新进程真正启动完成。
    /// @param moduleId 目标模块的稳定 ID。
    /// @param errorMessage 可选输出参数，失败时写入中文原因，可为空。
    /// @return 请求被接受返回 true；模块不存在、正在整体关闭或已有操作进行中返回 false。
    bool restart(const QString& moduleId, QString* errorMessage = nullptr);
    /// @brief 提交异步停止请求：先关闭父子两层消息入口并发出 stop 帧，随后由信号和心跳推进停止。
    /// @param moduleId 目标模块的稳定 ID。
    /// @param errorMessage 可选输出参数，失败时写入中文原因，可为空。
    /// @return 请求被接受返回 true；模块不存在或已处于停止、等待重启阶段返回 false。
    bool requestStop(const QString& moduleId, QString* errorMessage = nullptr);
    /// @brief 提交异步重启请求：进程仍在运行时先完整停止，退出后再启动新一代，复用同一个 Entry 以免新旧运行时混用。
    /// @param moduleId 目标模块的稳定 ID。
    /// @param errorMessage 可选输出参数，失败时写入中文原因，可为空。
    /// @return 请求被接受返回 true；模块不存在、正在整体关闭、已有操作进行中或当前阶段不允许重启返回 false。
    bool requestRestart(const QString& moduleId, QString* errorMessage = nullptr);
    /// @brief 请求 ProcessUi 子进程显示自己的窗口，不携带宿主尺寸。
    /// @param moduleId 目标模块的稳定 ID。
    /// @param errorMessage 可选输出参数，失败时写入中文原因，可为空。
    /// @return showWindow 控制帧写入成功返回 true；模块不存在、不是 ProcessUi、尚未 Running 或写入失败返回 false（写入失败同时标记该模块故障）。
    bool showWindow(const QString& moduleId, QString* errorMessage = nullptr);
    /// @brief 兼容入口：先请求显示窗口，再按需下发一帧尺寸调整，不重复触发子进程的 show()。
    /// @param moduleId 目标模块的稳定 ID。
    /// @param width 目标客户区宽度，单位像素；非正时跳过尺寸调整。
    /// @param height 目标客户区高度，单位像素；非正时跳过尺寸调整。
    /// @param errorMessage 可选输出参数，失败时写入中文原因，可为空。
    /// @return 显示请求成功，且尺寸非正或尺寸调整也成功时返回 true，否则返回 false。
    bool showWindow(const QString& moduleId,
                    int width,
                    int height,
                    QString* errorMessage = nullptr);
    /// @brief 只调整已嵌入 ProcessUi 子进程的客户区尺寸，不改变它的显示状态。
    /// @param moduleId 目标模块的稳定 ID。
    /// @param width 目标客户区宽度，单位像素，必须为正。
    /// @param height 目标客户区高度，单位像素，必须为正。
    /// @param errorMessage 可选输出参数，失败时写入中文原因，可为空。
    /// @return 尺寸控制帧写入成功返回 true；模块不存在、不是 ProcessUi、尺寸非正、尚未 Running 或写入失败返回 false（写入失败同时标记该模块故障）。
    bool resizeWindow(const QString& moduleId,
                      int width,
                      int height,
                      QString* errorMessage = nullptr);
    /// @brief 立即 kill 指定模块的子进程，用于故障注入和监督器上层的紧急停止；正常关闭请走 stop/shutdown。
    /// @param moduleId 目标模块的稳定 ID。
    /// @return 已发出 kill 返回 true；模块不存在、没有进程或进程已不在运行返回 false（后续收尾由统一的 finished/fault 路径完成）。
    bool terminate(const QString& moduleId);
    /// @brief 幂等关闭全部子进程和心跳定时器：先并行发 stop，再用一份总 deadline 等 ACK 与退出，超时逐级升级为 terminate、kill。
    /// @return 无；总预算耗尽仍未退出的进程会被标记为 Failed，本函数不会调用 QThread::terminate。
    void shutdown();

    // Asynchronous exit gate: business IPC and heartbeat remain active until ready.
    void beginPrepareForExit();
    bool isReadyForExit() const;
    QStringList pendingExitModules() const;
    void cancelPrepareForExit();

    // 查询只返回状态快照；Entry 的所有权始终属于监督器。
    /// @brief 列出当前处于 Running 阶段的模块。
    /// @return 运行中模块的稳定 ID 列表；没有运行中的模块时返回空列表。
    QStringList runningModuleIds() const;
    /// @brief 查询指定模块对外可见的生命周期状态文本。
    /// @param moduleId 目标模块的稳定 ID。
    /// @return 最近一次发布的状态文本（Starting/Running/Stopping/Stopped/Failed/Restarting）；模块不存在时返回空字符串。
    QString state(const QString& moduleId) const;
    /// @brief 读取指定模块父到子发送队列的统计快照，供诊断和边界测试使用。
    /// @param moduleId 目标模块的稳定 ID。
    /// @return 该模块的丢弃、拒绝、放弃累计数与等待、在途条数；模块不存在时返回全零。
    ProcessQueueStats queueStats(const QString& moduleId) const;

signals:
    // UI 通过这些信号显示生命周期和故障，不直接访问 Entry。
    /// @brief 模块生命周期状态发生变化时发射一次。
    /// @param moduleId 状态变化的模块稳定 ID。
    /// @param state 新状态文本（Starting/Running/Stopping/Stopped/Failed/Restarting）。
    /// @param detail 状态补充说明，通常为空。
    /// @return 无。
    void moduleStateChanged(const QString& moduleId,
                            const QString& state,
                            const QString& detail);
    /// @brief 模块进入无法自行恢复的故障状态时发射，UI 据此提示用户。
    /// @param moduleId 发生故障的模块稳定 ID。
    /// @param detail 故障原因文本，与 moduleStateChanged 的 detail 一致。
    /// @return 无。
    void moduleFault(const QString& moduleId, const QString& detail);
    /// @brief 子进程上报原生窗口句柄后发射，宿主据此完成窗口嵌入。
    /// @param moduleId 上报窗口的模块稳定 ID。
    /// @param windowId 子进程的原生窗口句柄，取值由子进程所在平台决定。
    /// @return 无。
    void windowHandleReady(const QString& moduleId, quintptr windowId);
    /// @brief 一次手动重启结束时发射，成功和失败都会发一次。
    /// @param moduleId 被重启的模块稳定 ID。
    /// @param success 新一代进程启动成功为 true，重启失败为 false。
    /// @param detail 失败原因文本；成功时为空。
    /// @return 无。
    void restartFinished(const QString& moduleId, bool success, const QString& detail);
    void stopFinished(const QString& moduleId, bool success, const QString& detail);
    /// @brief 模块进入或离开停止/重启操作时发射，UI 据此禁用重复点击。
    /// @param moduleId 状态变化的模块稳定 ID。
    /// @param busy 为 true 表示该模块正在停止或重启，此时不接受新的操作请求。
    /// @return 无。
    void operationBusyChanged(const QString& moduleId, bool busy);
    /// @brief startAll 批次内所有模块都结算（Running 或 Failed）后延迟发射一次。
    /// @param errors 启动失败模块的「模块 ID: 原因」文本列表；全部成功时为空。
    /// @return 无。
    void startupBatchFinished(const QStringList& errors);

public slots:
    /// @brief 保存全局 QSS 并下发给所有已注册的 ProcessUi 子进程；单个子进程发送失败只标记该模块故障。
    /// @param styleSheet 完整 QSS 文本，原样转发并在本地留存，供之后注册的子进程补发。
    /// @return 无。
    void applyStyleSheet(const QString& styleSheet);

private slots:
    /// @brief 接受子进程的本地连接，每个模块只保留第一个 Socket，多余连接立即断开并延迟销毁。
    /// @return 无；找不到对应 Entry 时忽略本次连接。
    void onServerConnection();
    /// @brief 读取 Socket 数据并循环解析完整帧，逐帧交给 handleFrame；帧非法时转入故障处理。
    /// @return 无；数据不足一帧时保留半帧等下次可读，进入停止/失败阶段后不再继续解析。
    void onSocketReadyRead();
    /// @brief 处理子进程 IPC 连接断开：非停止阶段按模块故障处理，可能触发自动重启。
    /// @return 无；停止中、已失败、等待重启或正在整体关闭时不重复报故障。
    void onSocketDisconnected();
    /// @brief 处理 QProcess 启动成功信号，把阶段从 StartingProcess 推进到 WaitingRegistration。
    /// @return 无；阶段不匹配（迟到信号）时不改变状态。
    void onProcessStarted();
    /// @brief 处理 QProcess 错误：只把无法启动和崩溃升级为模块故障，其余错误交给 finished/Socket 路径。
    /// @param error QProcess 上报的错误类型，只有 FailedToStart 和 Crashed 在这里被处理。
    /// @return 无；停止阶段或正在整体关闭时不重复报故障。
    void onProcessError(QProcess::ProcessError error);
    /// @brief 处理子进程退出：停止阶段走统一收尾，非预期退出按退出码或崩溃转成模块故障。
    /// @param exitCode 子进程退出码，仅用于生成故障文本。
    /// @param exitStatus 正常退出还是崩溃退出；崩溃时忽略 exitCode。
    /// @return 无。
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    /// @brief 状态机心跳：推进到期重启、注册/启动超时、停止超时升级（terminate 到 kill）和心跳探测。
    /// @return 无；本槽不进入嵌套事件循环，也不等待任何子进程。
    void onSupervisionTick();
    /// @brief 由合并后的 queued 唤醒在监督器线程执行，最多取 8 条仍有余量的等待项写入 Socket 并登记在途记录。
    /// @param moduleId 目标模块的稳定 ID。
    /// @return 无；连接不可用或没有可用在途槽位时结束本轮，仍有可发送项时再安排下一次唤醒。
    void drainChildQueue(const QString& moduleId);

private:
    friend class ProcessBridge;
    // 单个模块的生命周期状态机，由 QProcess 信号、Socket 帧和 onSupervisionTick 共同推进。
    enum class LifecyclePhase
    {
        Stopped, ///< 未启动，或本轮已完全停止并释放了运行时资源。
        StartingProcess, ///< 已调用 QProcess::start，等待 started 信号。
        WaitingRegistration, ///< 进程已启动，等待子进程发来 register 帧。
        WaitingStarted, ///< 注册已确认，等待子进程 onStart 完成的 started 或 startFailed 帧。
        Running, ///< 业务运行中，参与心跳并收发消息。
        StopRequested, ///< 已关闭消息入口，即将发出 stop 帧。
        WaitingStopAck, ///< stop 帧已发出，等待子进程回 stopAck。
        WaitingProcessExit, ///< 已要求进程退出（可能已升级为 terminate/kill），等待 QProcess 退出。
        RestartDelay, ///< 等待 restartAtMs 到期后启动下一代进程。
        Failed ///< 本轮已失败，资源已回收，等待自动或手动重启。
    };
    // 停止原因，决定进程退出后走 Stopped、Failed 还是排队重启。
    enum class StopPurpose
    {
        None, ///< 不是停止流程，例如运行期故障或非预期退出。
        Stop, ///< 外部调用 stop()/requestStop() 触发的正常停止。
        ManualRestart, ///< 手动 requestRestart() 触发的先停后启。
        AutoRestart, ///< 故障后按重启窗口策略安排的自动重启。
        Shutdown ///< 应用整体关闭（shutdown()）触发的停止。
    };
    struct Entry;
    void beginPrepareEntry(Entry* entry, StopPurpose purpose);
    void cancelPrepareEntry(Entry* entry);
    bool pollPrepareEntry(Entry* entry);

    // 以下查找函数都只返回监督器拥有的 Entry 借用指针，调用方不得删除或长期持有。
    /// @brief 按模块 ID 查找监督记录。
    /// @param moduleId 目标模块的稳定 ID。
    /// @return 命中的 Entry 借用指针；未找到返回 nullptr。
    Entry* findEntry(const QString& moduleId) const;
    /// @brief 按发出 newConnection 信号的本地服务器反查监督记录。
    /// @param object 信号发送者，实际为某个 Entry 的 QLocalServer。
    /// @return 命中的 Entry 借用指针；未找到返回 nullptr。
    Entry* findEntryByServer(QObject* object) const;
    /// @brief 按发出 readyRead/disconnected 信号的套接字反查监督记录。
    /// @param object 信号发送者，实际为某个 Entry 的 QLocalSocket。
    /// @return 命中的 Entry 借用指针；未找到返回 nullptr。
    Entry* findEntryBySocket(QObject* object) const;
    /// @brief 按发出错误/退出信号的子进程句柄反查监督记录。
    /// @param object 信号发送者，实际为某个 Entry 的 QProcess。
    /// @return 命中的 Entry 借用指针；未找到返回 nullptr。
    Entry* findEntryByProcess(QObject* object) const;
    /// @brief 准备并启动一轮子进程：重置代次与队列记账，重新生成服务器名和令牌，建好 QLocalServer 与 QProcess 后立即返回。
    /// @param entry 目标监督记录，必须已填好 config。
    /// @param errorMessage 可选输出参数，失败时写入中文原因，可为空。
    /// @return 已调用 QProcess::start 返回 true；旧进程未退出、可执行文件不存在或 IPC 服务器监听失败返回 false（同时标记故障）。
    bool beginStartEntry(Entry* entry, QString* errorMessage);
    /// @brief 开始单个模块的停止流程：关闭消息入口、建立 stop deadline 并发出 stop 帧，不等待进程退出。
    /// @param entry 目标监督记录。
    /// @param purpose 本次停止的目的，决定停止完成后进入 Stopped、Failed 还是 RestartDelay。
    /// @return 无；进程本来就没在运行时直接进入收尾。
    void beginStopEntry(Entry* entry, StopPurpose purpose);
    /// @brief 关闭该模块的两层消息入口（父到子闸门和总线单模块入口），并清空尚未写出的等待队列。
    /// @param entry 目标监督记录。
    /// @return 无；被清掉的等待项计入 dropped，重复调用不会重复累计。
    void closeEntryIngress(Entry* entry);
    /// @brief 收到 stopAck 后把阶段从 WaitingStopAck 推进到 WaitingProcessExit。
    /// @param entry 目标监督记录。
    /// @return 无；阶段不是 WaitingStopAck 或 ACK 标记未置位时不改变状态。
    void advanceStopEntry(Entry* entry);
    /// @brief QProcess 已退出后释放本轮运行时资源，并按停止目的决定 Stopped、Failed 还是排队下一次启动。
    /// @param entry 目标监督记录。
    /// @param detail 停止或失败的补充说明；为空时回退使用 entry->lastError。
    /// @return 无；需要重启时只设置 RestartDelay，不在这里启动进程。
    void finishEntryAfterProcessExit(Entry* entry, const QString& detail);
    /// @brief 更新模块「停止或重启操作进行中」标志，变化时发出 operationBusyChanged。
    /// @param entry 目标监督记录。
    /// @param busy 为 true 表示该模块正忙，新的停止/重启请求会被拒绝。
    /// @return 无；标志未变化时不发信号。
    void setOperationBusy(Entry* entry, bool busy);
    /// @brief 把模块从启动批次待结算集合中移除并记下失败原因，必要时安排批次完成信号。
    /// @param entry 目标监督记录。
    /// @param error 启动失败原因；为空表示该模块已成功进入 Running。
    /// @return 无；该模块已结算过时不重复处理。
    void settleStartupEntry(Entry* entry, const QString& error);
    /// @brief 启动批次全部结算后，用一次延迟调用把 startupBatchFinished 合并发出。
    /// @return 无；批次未激活、仍有待结算模块或已经安排过时直接返回。
    void scheduleStartupBatchFinished();
    /// @brief 判断生命周期阶段是否属于停止过渡态。
    /// @param phase 待判断的生命周期阶段。
    /// @return StopRequested、WaitingStopAck、WaitingProcessExit 三者返回 true，否则返回 false。
    bool isStoppingPhase(LifecyclePhase phase) const;
    /// @brief 判断生命周期阶段是否已完成注册（此后才允许收发业务消息）。
    /// @param phase 待判断的生命周期阶段。
    /// @return WaitingStarted、Running 以及停止过渡态返回 true，其余返回 false。
    bool isRegisteredPhase(LifecyclePhase phase) const;
    /// @brief 把控制帧编码后整帧写入该模块已连接的 Socket，只报告 Qt 是否接受了写入。
    /// @param entry 目标监督记录。
    /// @param frame 待发送的协议帧对象。
    /// @return 整帧字节全部交给 Qt 返回 true；Socket 不存在或未处于连接状态返回 false。
    bool sendFrame(Entry* entry, const QJsonObject& frame);
    /// @brief 把 MessageBus 回调投递的消息放进该子进程的按主题有界等待队列，并在需要时合并安排一次排空唤醒。
    /// @param moduleId 目标子进程模块稳定 ID。
    /// @param topic 消息主题，决定队列容量、大小上限和 Latest/Reliable 策略；为空时直接拒绝。
    /// @param senderModuleId 原始发送方模块 ID，随消息一起转发给子进程。
    /// @param payload 不可变消息载荷，入队后由等待队列持有，直到写入 Socket。
    /// @return 已进入本地等待队列返回 true；模块不存在、主题为空、载荷为空、超限、已停止或 Reliable 队列已满返回 false（超限只表示未入队）。
    bool enqueueMessageToChild(const QString& moduleId,
                               const QString& topic,
                               const QString& senderModuleId,
                               const MessagePayload& payload);
    /// @brief 处理子进程 deliveryAck：回收该消息的在途槽位、主题计数和共享内存段，并可能安排下一次排空。
    /// @param entry 目标监督记录。
    /// @param messageId 被确认的消息 ID，用于反查主题。
    /// @param accepted 子进程是否接受该消息；拒绝只影响统计和告警，槽位照样回收。
    /// @return 无；重复或迟到的 ACK 找不到在途记录时被忽略。
    void acknowledgeChildMessage(Entry* entry,
                                 const QString& messageId,
                                 bool accepted);
    /// @brief 消息还没写入 Socket 就失败时，回收它的在途槽位、主题计数和共享段，并计入 dropped 而不是 rejected。
    /// @param entry 目标监督记录。
    /// @param messageId 未能发出的消息 ID。
    /// @return 无；消息未登记在途表时直接返回。
    void discardChildMessageBeforeSend(Entry* entry, const QString& messageId);
    /// @brief 按帧的 type 字段分派注册、started/startFailed、pong、stopAck、窗口句柄、deliveryAck/sharedAck、日志和子到父业务消息。
    /// @param entry 帧所属模块的监督记录。
    /// @param frame 已解析完成的协议帧。
    /// @return 无；注册校验失败或消息超出主题上限时转入故障处理或拒绝路径。
    void handleFrame(Entry* entry, const QJsonObject& frame);
    /// @brief 统一故障入口：记录错误、关闭消息入口、发出 Failed 状态，并在重启窗口允许时安排一次自动重启。
    /// @param entry 目标监督记录。
    /// @param detail 面向用户的故障原因文本。
    /// @return 无；进程仍在运行时先 kill 并等退出信号再收尾，正在整体关闭或正常停止途中不报故障。
    void handleFault(Entry* entry, const QString& detail);
    /// @brief 释放本轮运行时对象（桥接器、Socket、Server、QProcess）并清空等待/在途/共享内存状态。
    /// @param entry 目标监督记录；调用前该轮 QProcess 应已退出。
    /// @return 无；等待项计入 dropped、未确认的在途项计入 abandoned，未停止的桥接器只记录错误并隔离。
    void destroyRuntime(Entry* entry);
    /// @brief 释放该模块所有还没等到 ACK 的大消息共享内存段。
    /// @param entry 目标监督记录。
    /// @return 无；先在锁内取出指针、再在锁外 detach 和删除，缩短互斥锁持有时间。
    void clearOutgoingShared(Entry* entry);
    /// @brief 先更新 entry->state 快照再发出 moduleStateChanged，保证调用方查询 state() 与信号看到的是同一状态。
    /// @param entry 目标监督记录。
    /// @param state 新的状态文本。
    /// @param detail 状态附带的详细说明，可为空。
    /// @return 无。
    void emitState(Entry* entry, const QString& state, const QString& detail);

    // entries_、Socket、QProcess 和定时器均由本类拥有；messageBus_ 只是借用。
    MessageBus* messageBus_; ///< 消息总线借用指针，用于注册和注销子进程桥接器；本类不负责销毁。
    MessageBusConfig messageBusConfig_; ///< 消息总线配置副本，提供各主题队列容量、大小上限、策略和共享内存阈值。
    ProcessConfig processConfig_; ///< 子进程监督配置副本，提供心跳间隔、注册/停止超时和重启窗口策略。
    QVector<Entry*> entries_; ///< 全部模块的监督记录，随 startAll 增长，由本类 new 出并在析构时统一删除。
    QTimer* supervisionTimer_; ///< 状态机心跳定时器（间隔限制在 50 到 250 毫秒），驱动 onSupervisionTick。
    QString styleSheet_; ///< 最近一次 applyStyleSheet 保存的全局 QSS，子进程注册确认时随 registerAck 下发。
    bool preparingExit_ = false;
    bool shuttingDown_; ///< 全局关闭门禁；shutdown() 置位后阻止新连接、自动重启和消息入队。
    QSet<QString> startupPendingModules_; ///< 启动批次中尚未结算（还没到 Running 或 Failed）的模块 ID 集合。
    QStringList startupErrors_; ///< 启动批次内失败模块的「模块 ID: 原因」列表，批次结束时随 startupBatchFinished 发出。
    bool startupBatchActive_; ///< 是否有一批 startAll 正在等待结算，防止并发提交新的启动批次。
    bool startupBatchSignalScheduled_; ///< 是否已安排一次合并的 startupBatchFinished 延迟发射，避免重复排队。
};
}
