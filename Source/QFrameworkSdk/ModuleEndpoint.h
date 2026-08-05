#pragma once

// 文件职责：定义模块和框架之间唯一的公开业务接口。
// 初学者可以先看 ModuleEndpoint，再看四个具体模块基类；模块业务代码
// 只需要实现 onStart/onStop/onMessage，并通过 publish/log* 与框架交互。

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QStringList>

#include "LogLevel.h"
#include "QFrameworkGlobal.h"

namespace qframework
{
class ModuleEndpoint;

// ModuleHost 是框架侧接口：主进程由 MessageBus 实现，子进程由 RuntimeHost 实现。
// 模块只借用该对象，不能保存到框架生命周期之外，也不能主动 delete。
class QFRAMEWORK_EXPORT ModuleHost
{
public:
    // 虚析构允许框架通过接口指针安全释放具体宿主实现。
    virtual ~ModuleHost() = default;

    // 框架收到模块的 publish 请求后实现此函数。返回值表示框架是否接受
    // 当前请求；子进程运行时的返回值只代表本地发送队列是否收下。
    virtual bool publishFromModule(const QString& moduleId,
                                   const QString& topic,
                                   const QByteArray& data) = 0;
    // 将模块日志交给集中 Logger；调用者不需要自己打开 QFile。
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
    // 构造时模块尚未绑定到框架，因此 moduleId 为空、running 为 false。
    ModuleEndpoint();
    virtual ~ModuleEndpoint();

    // 返回框架在启动边界绑定的模块 ID；模块业务代码只读，不自行修改。
    QString moduleId() const;
    // 声明本模块允许发布的主题。框架注册时读取一次，运行中不动态改变。
    virtual QStringList publishedTopics() const;
    // 声明本模块需要接收的主题。每个主题都会得到独立的有界输入队列。
    virtual QStringList subscribedTopics() const;
    // 启动回调期间允许 publish；返回 false 会撤销发布权限并让当前模块启动失败。
    virtual bool onStart();
    // 停止回调在框架停止接受新消息后执行；应尽快返回。
    virtual void onStop();
    // 异步消息回调。输入 data 是本次消息的副本，模块不拥有传输层资源。
    virtual void onMessage(const QString& topic,
                           const QString& senderModuleId,
                           const QByteArray& data);

    // 发布消息。返回 false 表示未运行、主题未声明、大小超限或有界队列已满。
    bool publish(const QString& topic, const QByteArray& data);
    // 长循环可定期查询；停止开始后返回 true，便于协作退出 onMessage。
    bool isStopRequested() const;
    // 四个日志快捷方法最终都会进入同一个集中日志线程。
    void logDebug(const QString& text);
    void logInfo(const QString& text);
    void logWarning(const QString& text);
    void logError(const QString& text);

    // 仅供框架在模块启动和停止边界绑定宿主；业务代码不能调用。
    void bindHost(const QString& moduleId, ModuleHost* host);
    // 仅供框架切换生命周期状态；false 后 publish 会立即拒绝。
    void setRunning(bool running);

private:
    // 日志快捷函数的公共实现；未绑定宿主时安全忽略。
    void log(LogLevel level, const QString& text);

    // 下面三个值由 bindHost/setRunning 修改，因此读取 publish 状态时必须加锁。
    QString moduleId_;
    ModuleHost* host_;
    bool running_;
    mutable QMutex mutex_;
};
}
