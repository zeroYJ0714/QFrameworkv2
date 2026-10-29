// 【阅读导引】让业务模块通过稳定接口与宿主交互。
// 阅读顺序：先读 ModuleHost 和 ModuleEndpoint；前者由宿主实现，后者由模块继承。bindHost/setRunning 是宿主管理的状态。
// 需要分清：publish/log 借用宿主，不拥有宿主。onMessage 的线程与 QObject 所在线程可能不同；UI 更新应转回 GUI 线程。canClose 返回 false 表示本次关闭不能继续。
// 这是声明/类型入口：先看 public 接口和字段注释；复杂执行过程再到同名 .cpp 或指定实现文件阅读。

#pragma once

// 文件职责：声明插件和 QFramework 宿主之间的最小公共契约。
// 插件实现 onStart/onStop/onMessage，通过 publish/publishShared/log* 发送数据；宿主负责线程、
// 队列、插件所有权和卸载。canClose 用于保存/放弃/取消关闭门禁，返回 false 时 MainWindow 必须
// 保持界面打开。接口不暴露 SQLite、ADB Socket 或 QWidget，避免业务插件越过所属线程边界。

// 文件职责：定义模块和框架之间唯一的公开业务接口。
// 初学者可以先看 ModuleEndpoint，再看四个具体模块基类；模块业务代码
// 只需要实现 onStart/onStop/onMessage，并通过 publish/log* 与框架交互。

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QStringList>

#include "LogLevel.h"
#include "MessagePayload.h"
#include "QFrameworkGlobal.h"

namespace qframework
{
class ModuleEndpoint;

// 框架只区分“隐藏一个 Dock”和“退出整个应用”两种用户关闭意图。
// 该枚举不携带 QWidget，非 UI 模块仍可继承同一端点而不依赖界面实现。
enum class ModuleCloseReason
{
    Hide, ///< 用户正在隐藏单个模块 Dock，拒绝时仅保持该 Dock 打开。
    ApplicationExit ///< 用户正在退出整个应用，任一模块拒绝时取消本次退出。
};

// ModuleHost 是框架侧接口：主进程由 MessageBus 实现，子进程由 RuntimeHost 实现。
// 模块只借用该对象，不能保存到框架生命周期之外，也不能主动 delete。
class QFRAMEWORK_EXPORT ModuleHost
{
public:
    /// @brief 通过接口指针安全销毁具体宿主实现。
    /// @return 无（析构函数不返回值）。
    virtual ~ModuleHost() = default;

    // 框架收到模块的 publish 请求后实现此函数。返回值表示框架是否接受
    // 当前请求；子进程运行时的返回值只代表本地发送队列是否收下。
    /// @brief 接收模块提交的普通字节发布；纯虚函数，由宿主实现完成投递。
    /// @param moduleId 稳定模块 ID。
    /// @param topic 消息主题名称，必须是模块声明过的发布主题。
    /// @param data 原始字节数据，可包含 NUL。
    /// @return 宿主接受并进入发布流程返回 true，否则返回 false。
    virtual bool publishFromModule(const QString& moduleId,
                                   const QString& topic,
                                   const QByteArray& data) = 0;
    // 共享发布入口只借用 payload 引用并复制智能指针；实现方不得修改其 QByteArray。
    // 空智能指针表示调用错误，合法的零字节消息必须使用非空 QByteArray 对象。
    /// @brief 接收模块提交的不可变共享载荷发布；纯虚函数，由宿主实现完成投递。
    /// @param moduleId 稳定模块 ID。
    /// @param topic 消息主题名称，必须是模块声明过的发布主题。
    /// @param payload 不可变共享载荷；空智能指针表示调用错误。
    /// @return 宿主接受并进入发布流程返回 true，否则返回 false。
    virtual bool publishSharedFromModule(const QString& moduleId,
                                         const QString& topic,
                                         const MessagePayload& payload) = 0;
    /// @brief 把模块日志交给集中 Logger；纯虚函数，由宿主实现决定落地方式。
    /// @param level 日志严重级别。
    /// @param moduleId 稳定模块 ID。
    /// @param text 日志正文。
    /// @return 无。
    virtual void logFromModule(LogLevel level,
                               const QString& moduleId,
                               const QString& text) = 0;
};

// 四类模块共同继承的业务端点。
// 本类不继承 QObject，因而既能与 QObject 非 UI 模块组合，也能与 QWidget
// UI 模块组合；内部互斥锁只保护宿主/ID/运行态快照。
class QFRAMEWORK_EXPORT ModuleEndpoint
{
public:
    /// @brief 创建尚未绑定宿主、也未进入运行态的端点。
    /// @return 无（构造函数不返回值）。
    ModuleEndpoint();
    /// @brief 虚析构端点；宿主指针只是借用，本类不负责释放。
    /// @return 无（析构函数不返回值）。
    virtual ~ModuleEndpoint();

    /// @brief 返回框架绑定的稳定模块 ID，可在任意线程安全调用。
    /// @return ID 副本；未绑定宿主时为空字符串。
    QString moduleId() const;
    /// @brief 声明模块会发布的主题，框架注册时读取一次并冻结成发布白名单。
    /// @return 主题列表，默认为空列表；派生模块应按实际 publish() 调用覆盖，声明空字符串或重复主题会导致注册失败。
    virtual QStringList publishedTopics() const;
    /// @brief 声明模块要订阅的主题，框架注册时据此建立本模块的投递关系。
    /// @return 主题列表，默认为空列表；派生模块应按实际订阅覆盖，声明空字符串或重复主题会导致注册失败。
    virtual QStringList subscribedTopics() const;
    /// @brief 模块启动钩子；调用期间发布权限已经打开，模块可以直接 publish()。
    /// @return 默认返回 true；覆盖后返回 false 或抛异常都会撤销发布权限并补调一次 onStop()。
    virtual bool onStart();
    /// @brief 模块停止钩子，在消息队列停止之后、总线注销之前调用，用于释放业务资源。
    /// @return 无；基类默认不做任何事，抛出的异常由调用方捕获并记入日志。
    virtual void onStop();
    // GUI 线程在总线仍运行时发起业务收尾。默认无工作；onStop 仅回收资源。
    /// @brief 要求模块在总线仍可用时完成业务收尾，由 GUI 线程在退出流程开始时调用。
    /// @return 无；基类默认不做任何工作，有异步收尾的模块应在此启动收尾并让 isReadyForExit() 反映进度。
    virtual void beginPrepareForExit();
    /// @brief 查询退出收尾是否已经完成，宿主在关闭流程中反复轮询直到就绪。
    /// @return 基类默认返回 true；收尾未完成的模块返回 false 以推迟本次退出。
    virtual bool isReadyForExit() const;
    /// @brief 取消本次退出收尾，把模块恢复到可继续运行的状态。
    /// @return 无；基类默认不做任何工作，用户取消关闭时会与 beginPrepareForExit() 成对出现。
    virtual void cancelPrepareForExit();
    // GUI 线程在可能丢失界面草稿前调用。默认允许关闭；有未保存内容的
    // UI 模块可以同步显示确认框，并用有界 Qt 事件循环等待一次保存结果。
    /// @brief 询问模块是否允许本次关闭，是界面草稿被丢弃前的最后一道门禁。
    /// @param reason 关闭意图：隐藏单个 Dock 还是退出整个应用。
    /// @return 允许关闭返回 true；返回 false 时调用方必须放弃本次关闭，Dock 或应用保持打开。
    virtual bool canClose(ModuleCloseReason reason);
    // 异步消息回调。输入 data 是本次消息的副本，模块不拥有传输层资源。
    /// @brief 异步投递一条已订阅主题的消息，同一模块的回调由专属队列线程串行执行。
    /// @param topic 消息主题名称。
    /// @param senderModuleId 发布方模块的稳定 ID，用于区分同主题的多个来源。
    /// @param data 原始字节数据，可包含 NUL。
    /// @return 无；基类默认丢弃全部消息，需要处理消息的模块应覆盖。
    virtual void onMessage(const QString& topic,
                           const QString& senderModuleId,
                           const QByteArray& data);

    /// @brief 发布普通 QByteArray 消息，内部包装成不可变共享载荷后交给宿主。
    /// @param topic 消息主题名称，必须是 publishedTopics() 声明过的主题。
    /// @param data 原始字节数据，可包含 NUL。
    /// @return 宿主接受并进入发布流程返回 true；未绑定宿主、未进入运行态或宿主拒绝时返回 false，本层不缓存消息。
    bool publish(const QString& topic, const QByteArray& data);
    // 发布调用方已经持有的不可变载荷。同进程多个订阅队列共享同一个
    // QByteArray 对象；跨进程宿主仍会在 QSharedMemory 边界复制字节。
    /// @brief 发布调用方已持有的不可变共享载荷，同进程订阅方共享同一份字节。
    /// @param topic 消息主题名称，必须是 publishedTopics() 声明过的主题。
    /// @param payload 不可变共享载荷；空智能指针会被直接拒绝。
    /// @return 宿主接受并进入发布流程返回 true；载荷为空、未绑定宿主或未进入运行态时返回 false。
    bool publishShared(const QString& topic, const MessagePayload& payload);
    /// @brief 查询框架是否已请求停止本模块，供长耗时的 onMessage 自查并及时返回。
    /// @return 尚未进入运行态或已请求停止时返回 true，运行期间返回 false。
    bool isStopRequested() const;
    /// @brief 以 Debug 级别记录一条日志，未绑定宿主时静默丢弃。
    /// @param text 日志正文。
    /// @return 无。
    void logDebug(const QString& text);
    /// @brief 以 Info 级别记录一条日志，未绑定宿主时静默丢弃。
    /// @param text 日志正文。
    /// @return 无。
    void logInfo(const QString& text);
    /// @brief 以 Warning 级别记录一条日志，未绑定宿主时静默丢弃。
    /// @param text 日志正文。
    /// @return 无。
    void logWarning(const QString& text);
    /// @brief 以 Error 级别记录一条日志，未绑定宿主时静默丢弃。
    /// @param text 日志正文。
    /// @return 无。
    void logError(const QString& text);

    /// @brief 由框架在注册时写入模块 ID 与宿主，注销时用空值解绑。
    /// @param moduleId 模块 ID；解绑时传空字符串。
    /// @param host 框架宿主的借用指针，本类不拥有；解绑时传 nullptr。
    /// @return 无；解绑后 publish/publishShared/log* 立即转为静默失败。
    void bindHost(const QString& moduleId, ModuleHost* host);
    /// @brief 由框架切换发布门禁，决定 publish/publishShared 是否放行。
    /// @param running true 表示进入运行态并放行发布，false 表示停止接受新消息。
    /// @return 无；总线正在停止时即使传入 true 也不会放行。
    void setRunning(bool running);

private:
    /// @brief 四个日志快捷函数的公共出口，复制宿主与模块 ID 后在锁外转发。
    /// @param level 日志严重级别。
    /// @param text 日志正文。
    /// @return 无；未绑定宿主时静默忽略。
    void log(LogLevel level, const QString& text);

    QString moduleId_; ///< 框架绑定的稳定 ID，由 mutex_ 保护。
    ModuleHost* host_; ///< 借用宿主指针，不拥有；由 mutex_ 保护并受框架生命周期保证。
    bool running_; ///< 发布门禁；由 mutex_ 保护。
    mutable QMutex mutex_; ///< 保护 moduleId_、host_ 和 running_ 的跨线程快照。
};
}
