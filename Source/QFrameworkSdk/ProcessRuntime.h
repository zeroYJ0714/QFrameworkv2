#pragma once

// 文件职责：声明进程插件侧运行时，把 ModuleEndpoint 的发布/日志转换成父子 IPC 帧。
// QLocalSocket、心跳定时器、注册 token 和共享内存 ACK 只在运行时线程使用；插件回调仍按
// ModuleEndpoint 生命周期执行。发送队列有 Latest/Reliable 语义和容量，停止会关闭入口、等待 ACK
// 或 deadline，然后报告故障；不把 socket 指针交给 GUI 或业务线程直接操作。

// 文件职责：声明子进程运行时。它把模块 SDK 调用翻译成父子 IPC 帧，
// 让 ProcessUi/ProcessNonUi 模块无需了解 QLocalSocket 和共享内存细节。

#include <QHash>
#include <QJsonObject>
#include <QMutex>
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
    /// @brief 执行 `run` 所定义的类职责。
    /// @param application 子进程的 QCoreApplication 借用指针。
    /// @param module 模块端点指针。
    /// @return 返回整数结果或状态码；具体取值由函数约定决定。
    static int run(QCoreApplication* application, ModuleEndpoint* module);

private slots:
    // Socket 连接成功后发送 register 帧并开始等待 registerAck。
    /// @brief 处理 `onSocketConnected` 对应的框架操作。
    /// @return 无。
    void onSocketConnected();
    // 收到父进程数据时累积半帧并逐帧分派协议控制/业务消息。
    /// @brief 处理 `onSocketReadyRead` 对应的框架操作。
    /// @return 无。
    void onSocketReadyRead();
    // 断线时停止新发布、唤醒队列并以故障码退出事件循环。
    /// @brief 处理 `onSocketDisconnected` 对应的框架操作。
    /// @return 无。
    void onSocketDisconnected();
    // 记录连接错误；注册前错误直接结束，注册后由断线路径统一清理。
    /// @brief 处理 `onSocketError` 对应的框架操作。
    /// @param error 错误对象或错误输出参数。
    /// @return 无。
    void onSocketError(QLocalSocket::LocalSocketError error);
    // 从子到父有界发送队列取有限批次，避免每条 publish 产生一个事件。
    /// @brief 排空 `drainPublishQueue` 对应的框架操作。
    /// @return 无。
    void drainPublishQueue();
    // 把模块日志切回运行时线程，再编码成控制帧写入 Socket。
    /// @brief 处理 `onSendLog` 对应的框架操作。
    /// @param level 日志严重级别。
    /// @param text 待显示、发送或记录的文本。
    /// @return 无。
    void onSendLog(int level, const QString& text);

private:
    struct TopicSettings
    {
        // 父进程在 registerAck 中下发这些值。queueCapacity 同时限制等待队列
        // 和已发送但未收到 publishAck/deliveryAck 的在途数量。
        int queueCapacity = 256; ///< `queueCapacity` 对应的数值配置、计数或时间状态。
        int maxMessageBytes = 16 * 1024 * 1024; ///< `maxMessageBytes` 对应的数值配置、计数或时间状态。
        // true 表示队列满时丢弃同主题最旧等待项；false 表示拒绝新消息。
        bool latest = false; ///< `latest` 对应的布尔状态标志。
    };

    class RuntimeHost;
    class MessageQueue;
    class PublishQueue;

    // 仅由静态 run() 创建，保证 application/module 的所有权边界集中处理。
    /// @brief 创建 ProcessRuntime 对象并初始化其基础状态。
    /// @param application 子进程的 QCoreApplication 借用指针。
    /// @param module 模块端点指针。
    /// @return 无（构造函数不返回值）。
    ProcessRuntime(QCoreApplication* application, ModuleEndpoint* module);
    /// @brief 销毁 ProcessRuntime 对象并释放其拥有的资源。
    /// @return 无（析构函数不返回值）。
    ~ProcessRuntime() override;

    // 执行一次完整子进程生命周期，返回 finish() 保存的退出码。
    /// @brief 执行 `execute` 所定义的类职责。
    /// @return 返回整数结果或状态码；具体取值由函数约定决定。
    int execute();
    // 以下三个函数读取监督器注入的命令行开关。
    /// @brief 解析 `parseArguments` 对应的框架操作。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool parseArguments();
    /// @brief 检查 `hasArgument` 对应的框架操作。
    /// @param name 名称或命令行参数名。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool hasArgument(const QString& name) const;
    /// @brief 执行 `argumentValue` 所定义的类职责。
    /// @param name 名称或命令行参数名。
    /// @return 返回对应 Qt 值或容器的安全副本。
    QString argumentValue(const QString& name) const;
    // 处理一帧父进程协议，必要时调用模块回调或改变生命周期状态。
    /// @brief 处理 `handleFrame` 对应的框架操作。
    /// @param frame 协议帧或媒体帧值。
    /// @return 无。
    void handleFrame(const QJsonObject& frame);
    // 发送只写入 QLocalSocket；payload 是否走共享内存由 drainPublishQueue 决定。
    // 只负责控制帧编码和 Socket 写入；业务大负载由调用方先放入共享段。
    /// @brief 发送 `sendFrame` 对应的框架操作。
    /// @param frame 协议帧或媒体帧值。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool sendFrame(const QJsonObject& frame);
    // 父到子：消息进入子进程输入队列后立即确认，不等待 onMessage()。
    /// @brief 发送 `sendDeliveryAck` 对应的框架操作。
    /// @param messageId 消息关联 ID。
    /// @param accepted 消息是否被接受。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool sendDeliveryAck(const QString& messageId, bool accepted);
    // 子到父：父进程 MessageBus 接收/拒绝后确认，释放本地在途槽位。
    /// @brief 处理 `handlePublishAck` 对应的框架操作。
    /// @param frame 协议帧或媒体帧值。
    /// @return 无。
    void handlePublishAck(const QJsonObject& frame);
    // 记录第一次退出原因并请求 Qt 事件循环结束，析构负责最终资源回收。
    /// @brief 执行 `finish` 所定义的类职责。
    /// @param exitCode 传给该操作的 `exitCode` 参数；取值应符合声明类型和函数用途。
    /// @return 无。
    void finish(int exitCode);
    // 退出前协作停止消息线程；false 表示必须让整个子进程结束且不能析构模块。
    /// @brief 执行 `prepareForExit` 所定义的类职责。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool prepareForExit();
    /// @brief 清理 `clearSharedSegments` 对应的框架操作。
    /// @return 无。
    void clearSharedSegments();
    // 读取主题专用设置，不存在时返回经过正值保护的默认设置。
    /// @brief 执行 `topicConfig` 所定义的类职责。
    /// @param topic 消息主题名称。
    /// @return 返回声明类型的结果值。
    TopicSettings topicConfig(const QString& topic) const;
    // 解析 registerAck 下发的 QJsonArray 主题规则。
    /// @brief 解析 `parseTopicConfigs` 对应的框架操作。
    /// @param frame 协议帧或媒体帧值。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool parseTopicConfigs(const QJsonObject& frame);
    // publish()/publishShared() 的非阻塞本地入口；队列持有不可变共享载荷，
    // 返回值只表示本地队列接收，不代表父进程最终 publishAck 结果。
    /// @brief 排队 `queuePublish` 对应的框架操作。
    /// @param topic 消息主题名称。
    /// @param payload 不可变或独立拥有的消息载荷。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool queuePublish(const QString& topic, const MessagePayload& payload);
    // 从任意模块线程安全地排队一条小型日志。
    /// @brief 排队 `queueLog` 对应的框架操作。
    /// @param level 日志严重级别。
    /// @param text 待显示、发送或记录的文本。
    /// @return 无。
    void queueLog(LogLevel level, const QString& text);
    // 只允许 Runtime 所属线程访问 QLocalSocket；Debug 测试靠断言守住边界。
    /// @brief 执行 `assertSocketThread` 所定义的类职责。
    /// @return 无。
    void assertSocketThread() const;
    // 在指针互斥下关闭发布闸门；可选汇总只从 Runtime 线程发送。
    /// @brief 停止 `stopPublishQueue` 对应的框架操作。
    /// @param reason 操作原因。
    /// @param report 是否发布或记录汇总信息。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool stopPublishQueue(const QString& reason, bool report);

    // application_ 和 module_ 由 run() 的调用方创建，ProcessRuntime 负责 module_
    // 的最终销毁；其余对象由当前运行时拥有并在析构时释放。
    QCoreApplication* application_; ///< `application_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    ModuleEndpoint* module_; ///< `module_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    // host_ 由 ModuleEndpoint 借用；它把 SDK 调用转回当前 ProcessRuntime。
    RuntimeHost* host_; ///< `host_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    // 父到子输入线程和子到父发送队列，均在停止时显式清空/唤醒。
    MessageQueue* messageQueue_; ///< `messageQueue_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    // 与业务线程的 enqueue 同步，防止 stop/delete 与正在进入队列的生产者竞态。
    QMutex publishQueueMutex_; ///< 保存 `publishQueueMutex` 对应的对象状态或配置值。
    PublishQueue* publishQueue_; ///< `publishQueue_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    // Socket 由当前 Qt 事件循环线程拥有，其他线程不直接写它。
    QLocalSocket* socket_; ///< `socket_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    // Socket 可能一次读到半帧或多帧，inputBuffer_ 保存未完成的协议字节。
    QByteArray inputBuffer_; ///< 保存 `inputBuffer` 对应的对象状态或配置值。
    // 保存解析后的完整 argv，便于启动阶段重复查询开关和值。
    QStringList arguments_; ///< 保存 `arguments` 相关值的 Qt 容器。
    QString moduleId_; ///< 框架绑定的稳定模块 ID。
    QString moduleType_; ///< 保存 `moduleType` 对应的对象状态或配置值。
    QString serverName_; ///< 保存 `serverName` 对应的对象状态或配置值。
    QString token_; ///< 保存 `token` 对应的对象状态或配置值。
    // 传输策略和队列默认值来自 registerAck；未收到前使用安全初值。
    int sharedMemoryThresholdBytes_; ///< `sharedMemoryThresholdBytes_` 对应的数值配置、计数或时间状态。
    int maxMessageBytes_; ///< `maxMessageBytes_` 对应的数值配置、计数或时间状态。
    int defaultQueueCapacity_; ///< `defaultQueueCapacity_` 对应的数值配置、计数或时间状态。
    int defaultMaxMessageBytes_; ///< `defaultMaxMessageBytes_` 对应的数值配置、计数或时间状态。
    bool defaultLatest_; ///< `defaultLatest_` 对应的布尔状态标志。
    int shutdownDrainTimeoutMs_; ///< `shutdownDrainTimeoutMs_` 对应的数值配置、计数或时间状态。
    bool waitForDebugger_; ///< `waitForDebugger_` 对应的布尔状态标志。
    int debuggerWaitTimeoutMs_; ///< `debuggerWaitTimeoutMs_` 对应的数值配置、计数或时间状态。
    // 只有收到 accepted 的 registerAck 后才允许 onStart 和业务发布。
    bool registrationAcknowledged_; ///< `registrationAcknowledged_` 对应的布尔状态标志。
    bool running_; ///< 当前生命周期运行门禁。
    bool stopping_; ///< 当前是否正在停止的生命周期门禁。
    bool unsafeMessageThread_; ///< `unsafeMessageThread_` 对应的布尔状态标志。
    int exitCode_; ///< `exitCode_` 对应的数值配置、计数或时间状态。
    // 父进程拒绝统计用于限频诊断，不改变 publish() 的同步返回语义。
    quint64 publishRejectedCount_; ///< `publishRejectedCount_` 对应的数值配置、计数或时间状态。
    quint64 publishDroppedCount_; ///< `publishDroppedCount_` 对应的数值配置、计数或时间状态。
    quint64 publishLocalRejectedCount_; ///< `publishLocalRejectedCount_` 对应的数值配置、计数或时间状态。
    quint64 publishAbandonedCount_; ///< `publishAbandonedCount_` 对应的数值配置、计数或时间状态。
    qint64 lastPublishRejectWarningMs_; ///< `lastPublishRejectWarningMs_` 对应的数值配置、计数或时间状态。
    QHash<QString, TopicSettings> topicConfigs_; ///< 保存 `topicConfigs` 相关值的 Qt 容器。
    // messageId -> 共享内存句柄；直到 publishAck 或故障清理前保持所有权。
    QHash<QString, QSharedMemory*> outgoingSharedSegments_; ///< `outgoingSharedSegments_` 对应的对象指针；所有权和线程归属见本类文件级说明。
};
}
