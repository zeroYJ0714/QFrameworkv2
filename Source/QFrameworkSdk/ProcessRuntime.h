#pragma once

// 文件职责：声明子进程运行时。它把模块 SDK 调用翻译成父子 IPC 帧，
// 让 ProcessUi/ProcessNonUi 模块无需了解 QLocalSocket 和共享内存细节。

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QLocalSocket>

#include "ModuleEndpoint.h"
#include "QFrameworkGlobal.h"

class QCoreApplication;
class QLocalSocket;
class QSharedMemory;

namespace qframework
{
class QFRAMEWORK_EXPORT ProcessRuntime : public QObject
{
    Q_OBJECT

public:
    // 子进程 main.cpp 只负责创建 QApplication/QCoreApplication 和模块实例；
    // run() 会读取监督器参数、注册主题、启动模块并进入 Qt 事件循环。
    static int run(QCoreApplication* application, ModuleEndpoint* module);

private slots:
    // Socket 连接成功后发送 register 帧并开始等待 registerAck。
    void onSocketConnected();
    // 收到父进程数据时累积半帧并逐帧分派协议控制/业务消息。
    void onSocketReadyRead();
    // 断线时停止新发布、唤醒队列并以故障码退出事件循环。
    void onSocketDisconnected();
    // 记录连接错误；注册前错误直接结束，注册后由断线路径统一清理。
    void onSocketError(QLocalSocket::LocalSocketError error);
    // 从子到父有界发送队列取有限批次，避免每条 publish 产生一个事件。
    void drainPublishQueue();
    // 把模块日志切回运行时线程，再编码成控制帧写入 Socket。
    void onSendLog(int level, const QString& text);

private:
    struct TopicSettings
    {
        // 父进程在 registerAck 中下发这些值。queueCapacity 同时限制等待队列
        // 和已发送但未收到 publishAck/deliveryAck 的在途数量。
        int queueCapacity = 256;
        int maxMessageBytes = 16 * 1024 * 1024;
        // true 表示队列满时丢弃同主题最旧等待项；false 表示拒绝新消息。
        bool latest = false;
    };

    class RuntimeHost;
    class MessageQueue;
    class PublishQueue;

    // 仅由静态 run() 创建，保证 application/module 的所有权边界集中处理。
    ProcessRuntime(QCoreApplication* application, ModuleEndpoint* module);
    ~ProcessRuntime() override;

    // 执行一次完整子进程生命周期，返回 finish() 保存的退出码。
    int execute();
    // 以下三个函数读取监督器注入的命令行开关。
    bool parseArguments();
    bool hasArgument(const QString& name) const;
    QString argumentValue(const QString& name) const;
    // 处理一帧父进程协议，必要时调用模块回调或改变生命周期状态。
    void handleFrame(const QJsonObject& frame);
    // 发送只写入 QLocalSocket；payload 是否走共享内存由 drainPublishQueue 决定。
    // 只负责控制帧编码和 Socket 写入；业务大负载由调用方先放入共享段。
    void sendFrame(const QJsonObject& frame);
    // 父到子：消息进入子进程输入队列后立即确认，不等待 onMessage()。
    void sendDeliveryAck(const QString& messageId, bool accepted);
    // 子到父：父进程 MessageBus 接收/拒绝后确认，释放本地在途槽位。
    void handlePublishAck(const QJsonObject& frame);
    // 记录第一次退出原因并请求 Qt 事件循环结束，析构负责最终资源回收。
    void finish(int exitCode);
    void clearSharedSegments();
    // 读取主题专用设置，不存在时返回经过正值保护的默认设置。
    TopicSettings topicConfig(const QString& topic) const;
    // 解析 registerAck 下发的 QJsonArray 主题规则。
    bool parseTopicConfigs(const QJsonObject& frame);
    // publish() 的非阻塞本地入口；返回值不代表父进程最终结果。
    bool queuePublish(const QString& topic, const QByteArray& data);
    // 从任意模块线程安全地排队一条小型日志。
    void queueLog(LogLevel level, const QString& text);

    // application_ 和 module_ 由 run() 的调用方创建，ProcessRuntime 负责 module_
    // 的最终销毁；其余对象由当前运行时拥有并在析构时释放。
    QCoreApplication* application_;
    ModuleEndpoint* module_;
    // host_ 由 ModuleEndpoint 借用；它把 SDK 调用转回当前 ProcessRuntime。
    RuntimeHost* host_;
    // 父到子输入线程和子到父发送队列，均在停止时显式清空/唤醒。
    MessageQueue* messageQueue_;
    PublishQueue* publishQueue_;
    // Socket 由当前 Qt 事件循环线程拥有，其他线程不直接写它。
    QLocalSocket* socket_;
    // Socket 可能一次读到半帧或多帧，inputBuffer_ 保存未完成的协议字节。
    QByteArray inputBuffer_;
    // 保存解析后的完整 argv，便于启动阶段重复查询开关和值。
    QStringList arguments_;
    QString moduleId_;
    QString moduleType_;
    QString serverName_;
    QString token_;
    // 传输策略和队列默认值来自 registerAck；未收到前使用安全初值。
    int sharedMemoryThresholdBytes_;
    int maxMessageBytes_;
    int defaultQueueCapacity_;
    int defaultMaxMessageBytes_;
    bool defaultLatest_;
    int shutdownDrainTimeoutMs_;
    bool waitForDebugger_;
    int debuggerWaitTimeoutMs_;
    // 只有收到 accepted 的 registerAck 后才允许 onStart 和业务发布。
    bool registrationAcknowledged_;
    bool running_;
    bool stopping_;
    int exitCode_;
    // 父进程拒绝统计用于限频诊断，不改变 publish() 的同步返回语义。
    quint64 publishRejectedCount_;
    qint64 lastPublishRejectWarningMs_;
    QHash<QString, TopicSettings> topicConfigs_;
    // messageId -> 共享内存句柄；直到 publishAck 或故障清理前保持所有权。
    QHash<QString, QSharedMemory*> outgoingSharedSegments_;
};
}
