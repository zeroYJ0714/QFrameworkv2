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
    /// @brief 读取监督器参数、启动模块并进入 Qt 事件循环。
    /// @param application 子进程的 QCoreApplication 借用指针。
    /// @param module 模块端点指针。
    /// @return 子进程退出码。
    static int run(QCoreApplication* application, ModuleEndpoint* module);

private slots:
    // Socket 连接成功后发送 register 帧并开始等待 registerAck。
    /// @brief 发送 register 帧并开始等待 registerAck。
    /// @return 无。
    void onSocketConnected();
    // 收到父进程数据时累积半帧并逐帧分派协议控制/业务消息。
    /// @brief 累积 Socket 字节并逐帧分派协议控制与业务消息。
    /// @return 无。
    void onSocketReadyRead();
    // 断线时停止新发布、唤醒队列并以故障码退出事件循环。
    /// @brief 断线时停止新发布、唤醒队列并以故障码退出事件循环。
    /// @return 无。
    void onSocketDisconnected();
    // 记录连接错误；注册前错误直接结束，注册后由断线路径统一清理。
    /// @brief 记录连接错误；注册前直接结束，注册后交由断线路径统一清理。
    /// @param error Socket 报告的错误码。
    /// @return 无。
    void onSocketError(QLocalSocket::LocalSocketError error);
    // 从子到父有界发送队列取有限批次，避免每条 publish 产生一个事件。
    /// @brief 从有界发送队列取出有限批次写入 Socket，避免每条 publish 触发一次事件。
    /// @return 无。
    void drainPublishQueue();
    // 把模块日志切回运行时线程，再编码成控制帧写入 Socket。
    /// @brief 把模块日志切回运行时线程并编码成控制帧发送。
    /// @param level 日志严重级别。
    /// @param text 日志正文。
    /// @return 无。
    void onSendLog(int level, const QString& text);

private:
    struct TopicSettings
    {
        // 父进程在 registerAck 中下发这些值。queueCapacity 同时限制等待队列
        // 和已发送但未收到 publishAck/deliveryAck 的在途数量。
        int queueCapacity = 256; ///< 该主题的队列容量与在途消息上限。
        int maxMessageBytes = 16 * 1024 * 1024; ///< 该主题单条消息的字节上限。
        // true 表示队列满时丢弃同主题最旧等待项；false 表示拒绝新消息。
        bool latest = false; ///< 队列满时是否丢弃最旧等待项。
    };

    class RuntimeHost;
    class MessageQueue;
    class PublishQueue;

    // 仅由静态 run() 创建，保证 application/module 的所有权边界集中处理。
    /// @brief 构造运行时对象，仅由静态 run() 调用。
    /// @param application 子进程的 QCoreApplication 借用指针。
    /// @param module 模块端点指针。
    /// @return 无（构造函数不返回值）。
    ProcessRuntime(QCoreApplication* application, ModuleEndpoint* module);
    /// @brief 析构并释放运行时拥有的队列、Socket 与共享内存资源。
    /// @return 无（析构函数不返回值）。
    ~ProcessRuntime() override;

    // 执行一次完整子进程生命周期，返回 finish() 保存的退出码。
    /// @brief 跑完一次子进程生命周期：解析参数、建立与主进程的连接、进入事件循环，最后返回退出码。
    /// @return finish() 保存的退出码；未走到 finish() 时返回非零错误码。
    int execute();
    // 以下三个函数读取监督器注入的命令行开关。
    /// @brief 解析监督器注入的命令行参数。
    /// @return 解析成功返回 true，否则返回 false。
    bool parseArguments();
    /// @brief 判断命令行是否包含指定开关。
    /// @param name 命令行开关名。
    /// @return 存在该开关返回 true，否则返回 false。
    bool hasArgument(const QString& name) const;
    /// @brief 读取指定命令行开关的取值。
    /// @param name 命令行开关名。
    /// @return 开关对应的取值；不存在时返回空字符串。
    QString argumentValue(const QString& name) const;
    // 处理一帧父进程协议，必要时调用模块回调或改变生命周期状态。
    /// @brief 处理一帧父进程协议，必要时调用模块回调或改变生命周期状态。
    /// @param frame 已解析的协议帧。
    /// @return 无。
    void handleFrame(const QJsonObject& frame);
    // 发送只写入 QLocalSocket；payload 是否走共享内存由 drainPublishQueue 决定。
    // 只负责控制帧编码和 Socket 写入；业务大负载由调用方先放入共享段。
    /// @brief 编码控制帧并写入 QLocalSocket。
    /// @param frame 已解析的协议帧。
    /// @return 写入成功返回 true，否则返回 false。
    bool sendFrame(const QJsonObject& frame);
    // 父到子：消息进入子进程输入队列后立即确认，不等待 onMessage()。
    /// @brief 向父进程发送消息投递确认。
    /// @param messageId 消息关联 ID。
    /// @param accepted 消息是否被接受。
    /// @return 发送成功返回 true，否则返回 false。
    bool sendDeliveryAck(const QString& messageId, bool accepted);
    // 子到父：父进程 MessageBus 接收/拒绝后确认，释放本地在途槽位。
    /// @brief 处理父进程的 publishAck 并释放本地在途槽位。
    /// @param frame 已解析的协议帧。
    /// @return 无。
    void handlePublishAck(const QJsonObject& frame);
    // 记录第一次退出原因并请求 Qt 事件循环结束，析构负责最终资源回收。
    /// @brief 记录首次退出原因并请求 Qt 事件循环结束。
    /// @param exitCode 子进程退出码。
    /// @return 无。
    void finish(int exitCode);
    // 退出前协作停止消息线程；false 表示必须让整个子进程结束且不能析构模块。
    /// @brief 退出前协作停止消息线程。
    /// @return 可以安全停止消息线程并析构模块时返回 true，否则返回 false。
    bool prepareForExit();
    /// @brief 释放所有尚未确认的共享内存段。
    /// @return 无。
    void clearSharedSegments();
    // 读取主题专用设置，不存在时返回经过正值保护的默认设置。
    /// @brief 读取主题专用传输设置。
    /// @param topic 消息主题名称。
    /// @return 主题设置；未配置时返回经过正值保护的默认值。
    TopicSettings topicConfig(const QString& topic) const;
    // 解析 registerAck 下发的 QJsonArray 主题规则。
    /// @brief 解析 registerAck 下发的主题规则数组。
    /// @param frame 已解析的协议帧。
    /// @return 解析成功返回 true，否则返回 false。
    bool parseTopicConfigs(const QJsonObject& frame);
    // publish()/publishShared() 的非阻塞本地入口；队列持有不可变共享载荷，
    // 返回值只表示本地队列接收，不代表父进程最终 publishAck 结果。
    /// @brief 非阻塞地把一条消息放入本地发送队列。
    /// @param topic 消息主题名称。
    /// @param payload 消息载荷。
    /// @return 本地队列接收成功返回 true；仅表示入队，不代表父进程已接收。
    bool queuePublish(const QString& topic, const MessagePayload& payload);
    // 从任意模块线程安全地排队一条小型日志。
    /// @brief 从任意模块线程安全地排队一条小型日志。
    /// @param level 日志严重级别。
    /// @param text 日志正文。
    /// @return 无。
    void queueLog(LogLevel level, const QString& text);
    // 只允许 Runtime 所属线程访问 QLocalSocket；Debug 测试靠断言守住边界。
    /// @brief 断言当前线程就是 Socket 所属线程（仅 Debug 生效）。
    /// @return 无。
    void assertSocketThread() const;
    // 在指针互斥下关闭发布闸门；可选汇总只从 Runtime 线程发送。
    /// @brief 在指针互斥保护下关闭发布闸门。
    /// @param reason 停止原因，用于日志与诊断。
    /// @param report 是否发布或记录汇总信息。
    /// @return 成功关闭发布队列返回 true，否则返回 false。
    bool stopPublishQueue(const QString& reason, bool report);

    // application_ 和 module_ 由 run() 的调用方创建，ProcessRuntime 负责 module_
    // 的最终销毁；其余对象由当前运行时拥有并在析构时释放。
    QCoreApplication* application_; ///< 由调用方创建并拥有的 QCoreApplication。
    ModuleEndpoint* module_; ///< 由调用方创建、ProcessRuntime 负责销毁的模块端点。
    // host_ 由 ModuleEndpoint 借用；它把 SDK 调用转回当前 ProcessRuntime。
    RuntimeHost* host_; ///< 借用指针，把 SDK 调用转回当前运行时。
    // 父到子输入线程和子到父发送队列，均在停止时显式清空/唤醒。
    MessageQueue* messageQueue_; ///< 父到子输入队列。
    // 与业务线程的 enqueue 同步，防止 stop/delete 与正在进入队列的生产者竞态。
    QMutex publishQueueMutex_; ///< 保护 publishQueue_ 的指针与入队操作，避免与 stop/delete 竞态。
    PublishQueue* publishQueue_; ///< 子到父有界发送队列。
    // Socket 由当前 Qt 事件循环线程拥有，其他线程不直接写它。
    QLocalSocket* socket_; ///< 与主进程通信的 Socket，仅由运行时线程访问。
    // Socket 可能一次读到半帧或多帧，inputBuffer_ 保存未完成的协议字节。
    QByteArray inputBuffer_; ///< 保存尚未拼成完整帧的残留字节。
    // 保存解析后的完整 argv，便于启动阶段重复查询开关和值。
    QStringList arguments_; ///< 解析后的完整命令行参数。
    QString moduleId_; ///< 框架绑定的稳定模块 ID。
    QString moduleType_; ///< 模块形态字符串，来自命令行参数。
    QString serverName_; ///< 父进程 QLocalServer 名称，用于建立连接。
    QString prepareRequestId_; // Correlates prepare/cancel/readiness within this process.
    QString token_; ///< 注册握手 token，随 register 帧上报。
    // 传输策略和队列默认值来自 registerAck；未收到前使用安全初值。
    int sharedMemoryThresholdBytes_; ///< 改用共享内存传输的字节阈值。
    int maxMessageBytes_; ///< 全局单条消息字节上限。
    int defaultQueueCapacity_; ///< 主题默认队列容量。
    int defaultMaxMessageBytes_; ///< 主题默认的单条消息字节上限。
    bool defaultLatest_; ///< 默认队列满时是否丢弃最旧等待项。
    int shutdownDrainTimeoutMs_; ///< 关闭时排空队列的时间预算，单位毫秒。
    bool waitForDebugger_; ///< 是否等待调试器附加后再启动。
    int debuggerWaitTimeoutMs_; ///< 等待调试器附加的超时时间，单位毫秒。
    // 只有收到 accepted 的 registerAck 后才允许 onStart 和业务发布。
    bool registrationAcknowledged_; ///< 是否已收到 accepted 的 registerAck。
    bool running_; ///< 当前生命周期运行门禁。
    bool stopping_; ///< 当前是否正在停止的生命周期门禁。
    bool unsafeMessageThread_; ///< 模块回调线程与消息线程不一致时置位，用于诊断。
    int exitCode_; ///< finish() 记录的进程退出码。
    // 父进程拒绝统计用于限频诊断，不改变 publish() 的同步返回语义。
    quint64 publishRejectedCount_; ///< 被父进程拒绝的发布计数。
    qint64 lastPublishRejectWarningMs_; ///< 上次输出发布拒绝告警的时间戳，单位毫秒，用于限频。
    QHash<QString, TopicSettings> topicConfigs_; ///< 主题名到传输设置的映射。
    // messageId -> 共享内存句柄；直到 publishAck 或故障清理前保持所有权。
    QHash<QString, QSharedMemory*> outgoingSharedSegments_; ///< messageId 到共享内存段的映射，确认或清理前保持所有权。
};
}
