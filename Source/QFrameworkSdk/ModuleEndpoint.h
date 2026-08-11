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
    /// @brief 通过接口指针安全销毁具体宿主；参数：无；返回值：无。
    /// @return 无。
    virtual ~ModuleHost() = default;

    // 框架收到模块的 publish 请求后实现此函数。返回值表示框架是否接受
    // 当前请求；子进程运行时的返回值只代表本地发送队列是否收下。
    /// @brief 接收模块普通字节发布；参数：moduleId、topic、data；返回值：宿主接受返回 true。
    /// @param moduleId 稳定模块 ID。
    /// @param topic 消息主题名称。
    /// @param data 原始字节数据，可包含 NUL。
    /// @return 宿主接受并进入发布流程返回 true，否则返回 false。
    virtual bool publishFromModule(const QString& moduleId,
                                   const QString& topic,
                                   const QByteArray& data) = 0;
    // 共享发布入口只借用 payload 引用并复制智能指针；实现方不得修改其 QByteArray。
    // 空智能指针表示调用错误，合法的零字节消息必须使用非空 QByteArray 对象。
    /// @brief 接收模块不可变共享载荷发布；参数：moduleId、topic、payload；返回值：宿主接受返回 true。
    /// @param moduleId 稳定模块 ID。
    /// @param topic 消息主题名称。
    /// @param payload 不可变或独立拥有的消息载荷。
    /// @return 宿主接受并进入发布流程返回 true，否则返回 false。
    virtual bool publishSharedFromModule(const QString& moduleId,
                                         const QString& topic,
                                         const MessagePayload& payload) = 0;
    /// @brief 把模块日志交给集中 Logger；参数：level、moduleId、text；返回值：无。
    /// @param level 日志严重级别。
    /// @param moduleId 稳定模块 ID。
    /// @param text 待显示、发送或记录的文本。
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
    /// @brief 创建尚未绑定宿主的端点；参数：无；返回值：无。
    /// @return 无。
    ModuleEndpoint();
    /// @brief 虚析构端点；参数：无；返回值：无。
    /// @return 无。
    virtual ~ModuleEndpoint();

    /// @brief 读取框架绑定的稳定模块 ID；参数：无；返回值：ID 副本，未绑定时为空。
    /// @return ID 副本；未绑定宿主时为空字符串。
    QString moduleId() const;
    /// @brief 声明允许发布的主题；参数：无；返回值：主题列表，默认空。
    /// @return 主题列表，默认为空列表。
    virtual QStringList publishedTopics() const;
    /// @brief 声明订阅主题；参数：无；返回值：主题列表，默认空。
    /// @return 主题列表，默认为空列表。
    virtual QStringList subscribedTopics() const;
    /// @brief 模块启动回调；参数：无；返回值：启动成功返回 true。
    /// @return 启动成功返回 true，失败返回 false。
    virtual bool onStart();
    /// @brief 模块停止回调；参数：无；返回值：无。
    /// @return 无。
    virtual void onStop();
    // GUI 线程在可能丢失界面草稿前调用。默认允许关闭；有未保存内容的
    // UI 模块可以同步显示确认框，并用有界 Qt 事件循环等待一次保存结果。
    /// @brief 询问 UI 模块是否允许隐藏/退出；参数：reason；返回值：允许返回 true。
    /// @param reason 操作原因或关闭原因。
    /// @return 允许关闭返回 true，需要保留界面返回 false。
    virtual bool canClose(ModuleCloseReason reason);
    // 异步消息回调。输入 data 是本次消息的副本，模块不拥有传输层资源。
    /// @brief 异步消息回调；参数：topic、senderModuleId、data；返回值：无。
    /// @param topic 消息主题名称。
    /// @param senderModuleId 发送消息的模块 ID。
    /// @param data 原始字节数据，可包含 NUL。
    /// @return 无。
    virtual void onMessage(const QString& topic,
                           const QString& senderModuleId,
                           const QByteArray& data);

    /// @brief 发布普通 QByteArray；参数：topic、data；返回值：宿主接受返回 true。
    /// @param topic 消息主题名称。
    /// @param data 原始字节数据，可包含 NUL。
    /// @return 宿主接受并进入发布流程返回 true，否则返回 false。
    bool publish(const QString& topic, const QByteArray& data);
    // 发布调用方已经持有的不可变载荷。同进程多个订阅队列共享同一个
    // QByteArray 对象；跨进程宿主仍会在 QSharedMemory 边界复制字节。
    /// @brief 发布不可变共享载荷；参数：topic、payload；返回值：宿主接受返回 true。
    /// @param topic 消息主题名称。
    /// @param payload 不可变或独立拥有的消息载荷。
    /// @return 宿主接受并进入发布流程返回 true，否则返回 false。
    bool publishShared(const QString& topic, const MessagePayload& payload);
    /// @brief 查询停止是否已请求；参数：无；返回值：未运行/停止中返回 true。
    /// @return 已请求停止或尚未运行返回 true，否则返回 false。
    bool isStopRequested() const;
    /// @brief 记录 Debug 日志；参数：text；返回值：无。
    /// @param text 待显示、发送或记录的文本。
    /// @return 无。
    void logDebug(const QString& text);
    /// @brief 记录 Info 日志；参数：text；返回值：无。
    /// @param text 待显示、发送或记录的文本。
    /// @return 无。
    void logInfo(const QString& text);
    /// @brief 记录 Warning 日志；参数：text；返回值：无。
    /// @param text 待显示、发送或记录的文本。
    /// @return 无。
    void logWarning(const QString& text);
    /// @brief 记录 Error 日志；参数：text；返回值：无。
    /// @param text 待显示、发送或记录的文本。
    /// @return 无。
    void logError(const QString& text);

    /// @brief 由框架绑定 ID 和借用宿主；参数：moduleId、host；返回值：无。
    /// @param moduleId 稳定模块 ID。
    /// @param host 框架宿主的借用指针。
    /// @return 无。
    void bindHost(const QString& moduleId, ModuleHost* host);
    /// @brief 由框架设置发布门禁；参数：running；返回值：无。
    /// @param running 目标运行状态。
    /// @return 无。
    void setRunning(bool running);

private:
    /// @brief 日志快捷函数公共出口；参数：level、text；返回值：无。
    /// @param level 日志严重级别。
    /// @param text 待显示、发送或记录的文本。
    /// @return 无。
    void log(LogLevel level, const QString& text);

    QString moduleId_; ///< 框架绑定的稳定 ID，由 mutex_ 保护。
    ModuleHost* host_; ///< 借用宿主指针，不拥有；由 mutex_ 保护并受框架生命周期保证。
    bool running_; ///< 发布门禁；由 mutex_ 保护。
    mutable QMutex mutex_; ///< 保护 moduleId_、host_ 和 running_ 的跨线程快照。
};
}
