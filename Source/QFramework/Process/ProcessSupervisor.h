#pragma once

// 文件职责：声明主进程子进程监督器。
// 它拥有 QProcess/QLocalServer/QLocalSocket，并把 MessageBus 消息放入按主题
// 有界队列；心跳、注册、重启、窗口句柄和 ACK 都在同一 Qt 线程协调。

#include <QProcess>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include "FrameworkConfig.h"
#include "QFrameworkGlobal.h"

namespace qframework
{
class MessageBus;

class QFRAMEWORK_EXPORT ProcessSupervisor : public QObject
{
    Q_OBJECT

public:
    // messageBus 是借用指针；配置按值保存，避免启动后被外部修改。
    explicit ProcessSupervisor(MessageBus* messageBus,
                               const MessageBusConfig& messageBusConfig,
                               const ProcessConfig& processConfig,
                               QObject* parent = nullptr);
    ~ProcessSupervisor() override;

    // 启动所有 ProcessUi/ProcessNonUi 模块；失败模块写入 errors 但继续处理其他模块。
    bool startAll(const QVector<ModuleConfig>& modules,
                  QStringList* errors = nullptr);
    // 正常 stop 等待 stopAck；restart 先清理旧 IPC，再重新创建随机令牌和队列。
    bool stop(const QString& moduleId, QString* errorMessage = nullptr);
    bool restart(const QString& moduleId, QString* errorMessage = nullptr);
    bool showWindow(const QString& moduleId, QString* errorMessage = nullptr);
    bool showWindow(const QString& moduleId,
                    int width,
                    int height,
                    QString* errorMessage = nullptr);
    // 强制结束用于故障注入和监督器上层的紧急停止；正常关闭应使用 stop/shutdown。
    bool terminate(const QString& moduleId);
    void shutdown();

    // 查询只返回状态快照；Entry 的所有权始终属于监督器。
    QStringList runningModuleIds() const;
    QString state(const QString& moduleId) const;

signals:
    // UI 通过这些信号显示生命周期和故障，不直接访问 Entry。
    void moduleStateChanged(const QString& moduleId,
                            const QString& state,
                            const QString& detail);
    void moduleFault(const QString& moduleId, const QString& detail);
    void windowHandleReady(const QString& moduleId, quintptr windowId);

public slots:
    void applyStyleSheet(const QString& styleSheet);

private slots:
    // 有新子进程连接时接受并校验对应 Entry 的本地服务器连接。
    void onServerConnection();
    // 累积并解析来自任一子进程的完整协议帧。
    void onSocketReadyRead();
    // 连接断开时进入故障或正常停止清理流程。
    void onSocketDisconnected();
    // QProcess 报告启动/运行错误时转换为模块故障信号。
    void onProcessError(QProcess::ProcessError error);
    // 进程退出后统一判断是否预期、是否需要重启。
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    // 周期检查注册 deadline、心跳、延迟重启和停止超时。
    void onSupervisionTick();
    // 由合并后的 queued 唤醒执行有限批次发送；不是“每条消息一个事件”。
    void drainChildQueue(const QString& moduleId);

private:
    friend class ProcessBridge;
    struct Entry;

    // 以下查找函数都只返回监督器拥有的 Entry 借用指针。
    Entry* findEntry(const QString& moduleId) const;
    Entry* findEntryByServer(QObject* object) const;
    Entry* findEntryBySocket(QObject* object) const;
    Entry* findEntryByProcess(QObject* object) const;
    // 为一个 Entry 建立服务端、进程和桥接器资源。
    bool startEntry(Entry* entry, QString* errorMessage);
    // 在有限时间内泵送事件，等待注册和 started 两个阶段完成。
    bool waitForRunning(Entry* entry, int timeoutMs, QString* errorMessage);
    // 编码并写入已经认证的子进程 Socket。
    bool sendFrame(Entry* entry, const QJsonObject& frame);
    // 将 MessageBus 回调放入父到子有界队列。返回值只表示是否进入等待队列。
    bool enqueueMessageToChild(const QString& moduleId,
                               const QString& topic,
                               const QString& senderModuleId,
                               const QByteArray& data);
    // 处理子进程 deliveryAck，并回收在途计数及可选共享内存。
    void acknowledgeChildMessage(Entry* entry,
                                 const QString& messageId,
                                 bool accepted);
    // 根据 type 分派注册、心跳、ACK、窗口和业务消息。
    void handleFrame(Entry* entry, const QJsonObject& frame);
    // 标记故障、清理资源，并按重启策略安排下一次启动。
    void handleFault(Entry* entry, const QString& detail);
    // 释放一轮运行时对象并清空所有等待/在途/共享内存状态。
    void destroyRuntime(Entry* entry);
    // 在锁外 detach/delete 所有未完成的大消息共享段。
    void clearOutgoingShared(Entry* entry);
    void emitState(Entry* entry, const QString& state, const QString& detail);

    // entries_、socket 和进程对象均由监督器拥有；messageBus_ 只是借用。
    // messageBus_ 借用；Entry、Socket、QProcess 和定时器由本类拥有。
    MessageBus* messageBus_;
    MessageBusConfig messageBusConfig_;
    ProcessConfig processConfig_;
    QVector<Entry*> entries_;
    QTimer* supervisionTimer_;
    QString styleSheet_;
    // shutdown 后阻止新连接、重启和队列入队。
    bool shuttingDown_;
};
}
