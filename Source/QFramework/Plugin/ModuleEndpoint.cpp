// 文件职责：实现插件基类与 ModuleHost 之间的生命周期和发布适配。
// ModuleEndpoint 不拥有宿主，只保存受互斥量保护的指针、running 标志和模块 ID；
// 只有绑定宿主且进入 Running 后才允许 publish/log。停止时先关闭发布入口，再由宿主等待已接收
// 消息结束；所有跨线程调用通过 Qt/宿主提供的安全入口，不直接触碰 MessageBus 内部队列。
#include "ModuleEndpoint.h"

#include <QMutexLocker>

// ModuleEndpoint 是 SDK 的轻量适配层：它只保存宿主指针和生命周期状态，
// 不知道宿主是主进程 MessageBus 还是子进程 RuntimeHost。

namespace qframework
{
// 新模块默认没有宿主，也不处于运行态。只有框架完成注册和生命周期切换后，
// publish() 才会把数据交给 MessageBus 或 ProcessRuntime。
ModuleEndpoint::ModuleEndpoint()
    : host_(nullptr),
      running_(false)
{
}

// 本类只借用 ModuleHost，不负责 delete；具体 QObject/QWidget 由四种模块基类管理。
ModuleEndpoint::~ModuleEndpoint() = default;

// 在锁内复制 ID，避免停止/解绑同时修改字符串时发生数据竞争。
QString ModuleEndpoint::moduleId() const
{
    QMutexLocker locker(&mutex_);
    return moduleId_;
}

// 默认不声明发布主题；派生模块应按实际 publish() 调用显式覆盖。
QStringList ModuleEndpoint::publishedTopics() const
{
    return QStringList();
}

// 默认不订阅任何主题；派生模块覆盖后由框架在注册阶段读取一次。
QStringList ModuleEndpoint::subscribedTopics() const
{
    return QStringList();
}

// 默认启动成功，适合没有初始化工作的简单模块。
bool ModuleEndpoint::onStart()
{
    return true;
}

// 默认没有停止动作；需要释放业务资源的模块应覆盖此函数。
void ModuleEndpoint::onStop()
{
}

// 普通模块没有界面草稿，因此默认不阻止 Dock 隐藏或应用退出。
// 该回调只由 MainWindow 的 GUI 线程调用，基类不增加跨线程状态。
bool ModuleEndpoint::canClose(ModuleCloseReason reason)
{
    Q_UNUSED(reason)
    return true;
}

// 默认消息处理器明确忽略全部参数；只发布、不订阅的模块无需实现空槽。
void ModuleEndpoint::onMessage(const QString& topic,
                               const QString& senderModuleId,
                               const QByteArray& data)
{
    Q_UNUSED(topic)
    Q_UNUSED(senderModuleId)
    Q_UNUSED(data)
}

// 将一条业务消息交给当前宿主。
// 返回 false 只说明宿主未绑定、模块未运行或下游拒绝；本层不缓存消息。
bool ModuleEndpoint::publish(const QString& topic, const QByteArray& data)
{
    // 旧接口只在入口创建一次共享对象，后续总线和订阅队列不再复制 QByteArray 对象。
    return publishShared(topic, makeMessagePayload(data));
}

// 将调用方提供的不可变载荷交给当前宿主；本层只读取生命周期快照，不缓存载荷。
bool ModuleEndpoint::publishShared(const QString& topic, const MessagePayload& payload)
{
    // 先检查共享载荷非空，再在锁内复制宿主指针/模块 ID；调用宿主时释放 mutex，
    // 因为宿主可能回调总线或再次进入本端点，持锁会造成自锁。
    if (payload.isNull())
        return false;

    ModuleHost* host = nullptr;
    QString id;
    {
        QMutexLocker locker(&mutex_);
        // 只在锁内读取状态，离开临界区后再调用宿主，避免宿主回调反向加锁。
        if (host_ == nullptr || !running_)
            return false;
        host = host_;
        id = moduleId_;
    }

    // 宿主调用可能进入总线或本地发送队列，因此不能持有 mutex_。
    return host->publishSharedFromModule(id, topic, payload);
}

// 与 publish 使用同一把 mutex_ 读取运行态，不增加 ABI 数据成员。
bool ModuleEndpoint::isStopRequested() const
{
    QMutexLocker locker(&mutex_);
    return !running_;
}

// 四个公开快捷函数只选择级别，公共的宿主转发集中在私有 log() 中。
void ModuleEndpoint::logDebug(const QString& text)
{
    log(LogLevel::Debug, text);
}

// 记录正常运行信息。
void ModuleEndpoint::logInfo(const QString& text)
{
    log(LogLevel::Info, text);
}

// 记录可恢复问题或降级行为。
void ModuleEndpoint::logWarning(const QString& text)
{
    log(LogLevel::Warning, text);
}

// 记录需要关注的失败。
void ModuleEndpoint::logError(const QString& text)
{
    log(LogLevel::Error, text);
}

// 框架在注册时绑定，在注销时传入空 ID/nullptr 解除绑定。
void ModuleEndpoint::bindHost(const QString& moduleId, ModuleHost* host)
{
    // bindHost 是注册/注销边界，不转移 host 所有权；传入空指针表示解绑，之后 publish
    // 会立即失败，避免调用已经销毁的 MessageBus/RuntimeHost。
    QMutexLocker locker(&mutex_);
    // 绑定和解绑都在生命周期边界发生；解绑后后续 publish 立即返回 false。
    moduleId_ = moduleId;
    host_ = host;
}

// 生命周期协调器通过此开关控制发布入口，避免启动前或停止后产生新消息。
void ModuleEndpoint::setRunning(bool running)
{
    QMutexLocker locker(&mutex_);
    // running 与 host 分开保存：模块可以已注册但暂时禁止发布。
    running_ = running;
}

// 将日志转发给宿主；未绑定期间安静忽略，避免模块构造/析构日志访问空指针。
void ModuleEndpoint::log(LogLevel level, const QString& text)
{
    ModuleHost* host = nullptr;
    QString id;
    {
        QMutexLocker locker(&mutex_);
        // 复制宿主和模块 ID 后立即释放锁，再进入 Logger，避免日志路径自锁。
        host = host_;
        id = moduleId_;
    }
    if (host != nullptr)
        host->logFromModule(level, id, text);
}
}
