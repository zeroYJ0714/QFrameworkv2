// 【阅读导引】让业务模块通过稳定接口与宿主交互。
// 阅读顺序：先读 ModuleHost 和 ModuleEndpoint；前者由宿主实现，后者由模块继承。bindHost/setRunning 是宿主管理的状态。
// 需要分清：publish/log 借用宿主，不拥有宿主。onMessage 的线程与 QObject 所在线程可能不同；UI 更新应转回 GUI 线程。canClose 返回 false 表示本次关闭不能继续。
// 这是执行实现：按入口调用和信号回调追踪，不要按文件行号把所有函数理解成连续执行。

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
/// @brief 构造未绑定宿主、未进入运行态的端点对象。
/// @return 无（构造函数不返回值）。
ModuleEndpoint::ModuleEndpoint()
    : host_(nullptr),
      running_(false)
{
}

// 本类只借用 ModuleHost，不负责 delete；具体 QObject/QWidget 由四种模块基类管理。
/// @brief 虚析构；宿主指针为借用，本类不负责释放。
/// @return 无（析构函数不返回值）。
ModuleEndpoint::~ModuleEndpoint() = default;

// 在锁内复制 ID，避免停止/解绑同时修改字符串时发生数据竞争。
/// @brief 返回当前绑定的模块 ID。
/// @return 模块 ID 副本；未绑定时为空字符串。
QString ModuleEndpoint::moduleId() const
{
    QMutexLocker locker(&mutex_);
    return moduleId_;
}

// 默认不声明发布主题；派生模块应按实际 publish() 调用显式覆盖。
/// @brief 返回本模块声明发布的主题列表。
/// @return 默认返回空列表，派生模块应按实际 publish() 覆盖。
QStringList ModuleEndpoint::publishedTopics() const
{
    return QStringList();
}

// 默认不订阅任何主题；派生模块覆盖后由框架在注册阶段读取一次。
/// @brief 返回本模块声明订阅的主题列表。
/// @return 默认返回空列表，派生模块应按实际订阅覆盖。
QStringList ModuleEndpoint::subscribedTopics() const
{
    return QStringList();
}

// 默认启动成功，适合没有初始化工作的简单模块。
/// @brief 模块启动钩子，默认直接成功。
/// @return 默认返回 true；需要初始化工作的模块应覆盖。
bool ModuleEndpoint::onStart()
{
    return true;
}

// 默认没有停止动作；需要释放业务资源的模块应覆盖此函数。
/// @brief 模块停止钩子，默认不做任何事。
/// @return 无。
void ModuleEndpoint::onStop()
{
}

/// @brief 进入退出准备阶段，默认无动作。
/// @return 无。
void ModuleEndpoint::beginPrepareForExit() {}
/// @brief 查询模块是否已完成退出准备，默认始终就绪。
/// @return 默认返回 true。
bool ModuleEndpoint::isReadyForExit() const { return true; }
/// @brief 取消退出准备，默认无动作。
/// @return 无。
void ModuleEndpoint::cancelPrepareForExit() {}

// 普通模块没有界面草稿，因此默认不阻止 Dock 隐藏或应用退出。
// 该回调只由 MainWindow 的 GUI 线程调用，基类不增加跨线程状态。
/// @brief 询问模块是否允许关闭，默认总是允许。
/// @param reason 触发关闭的原因，基类不使用。
/// @return 默认返回 true；有界面草稿的模块可覆盖以阻止关闭。
bool ModuleEndpoint::canClose(ModuleCloseReason reason)
{
    Q_UNUSED(reason)
    return true;
}

// 默认消息处理器明确忽略全部参数；只发布、不订阅的模块无需实现空槽。
/// @brief 默认消息处理器，忽略全部参数。
/// @param topic 消息主题。
/// @param senderModuleId 发布者模块 ID。
/// @param data 消息载荷。
/// @return 无。
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
/// @brief 把字节数组消息包装成共享载荷后发布。
/// @param topic 目标主题。
/// @param data 待发送的字节内容。
/// @return 发布被接受返回 true，否则返回 false。
bool ModuleEndpoint::publish(const QString& topic, const QByteArray& data)
{
    // 旧接口只在入口创建一次共享对象，后续总线和订阅队列不再复制 QByteArray 对象。
    return publishShared(topic, makeMessagePayload(data));
}

// 将调用方提供的不可变载荷交给当前宿主；本层只读取生命周期快照，不缓存载荷。
/// @brief 把共享载荷交给当前宿主发布，锁外调用避免自锁。
/// @param topic 目标主题。
/// @param payload 共享不可变载荷。
/// @return 载荷为空、未绑定宿主或未运行时返回 false，否则返回宿主结果。
bool ModuleEndpoint::publishShared(const QString& topic, const MessagePayload& payload)
{
    // 先检查共享载荷非空，再在锁内复制宿主指针/模块 ID；调用宿主时释放 mutex，
    // 因为宿主可能回调总线或再次进入本端点，持锁会造成自锁。
    if (payload.isNull())
        return false;

    ModuleHost* host = nullptr; ///< 锁内复制的宿主借用指针，锁外使用。
    QString id; ///< 锁内复制的模块 ID，与宿主一起传给发布接口。
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
/// @brief 查询生命周期协调器是否已请求停止本模块。
/// @return 未处于运行态时返回 true。
bool ModuleEndpoint::isStopRequested() const
{
    QMutexLocker locker(&mutex_);
    return !running_;
}

// 四个公开快捷函数只选择级别，公共的宿主转发集中在私有 log() 中。
/// @brief 以 Debug 级别记录一条日志。
/// @param text 日志文本。
/// @return 无。
void ModuleEndpoint::logDebug(const QString& text)
{
    log(LogLevel::Debug, text);
}

// 记录正常运行信息。
/// @brief 以 Info 级别记录一条日志。
/// @param text 日志文本。
/// @return 无。
void ModuleEndpoint::logInfo(const QString& text)
{
    log(LogLevel::Info, text);
}

// 记录可恢复问题或降级行为。
/// @brief 以 Warning 级别记录一条日志。
/// @param text 日志文本。
/// @return 无。
void ModuleEndpoint::logWarning(const QString& text)
{
    log(LogLevel::Warning, text);
}

// 记录需要关注的失败。
/// @brief 以 Error 级别记录一条日志。
/// @param text 日志文本。
/// @return 无。
void ModuleEndpoint::logError(const QString& text)
{
    log(LogLevel::Error, text);
}

// 框架在注册时绑定，在注销时传入空 ID/nullptr 解除绑定。
/// @brief 绑定或解绑宿主，并同步模块 ID。
/// @param moduleId 模块 ID；解绑时传空字符串。
/// @param host 宿主指针；解绑时传 nullptr。
/// @return 无；不转移宿主所有权。
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
/// @brief 设置模块运行态，控制 publish 是否放行。
/// @param running true 表示进入运行态，false 表示停止发布。
/// @return 无。
void ModuleEndpoint::setRunning(bool running)
{
    QMutexLocker locker(&mutex_);
    // running 与 host 分开保存：模块可以已注册但暂时禁止发布。
    running_ = running;
}

// 将日志转发给宿主；未绑定期间安静忽略，避免模块构造/析构日志访问空指针。
/// @brief 把日志转发给宿主，未绑定宿主时静默忽略。
/// @param level 日志级别。
/// @param text 日志文本。
/// @return 无。
void ModuleEndpoint::log(LogLevel level, const QString& text)
{
    ModuleHost* host = nullptr; ///< 锁内复制的宿主借用指针，锁外使用。
    QString id; ///< 锁内复制的模块 ID，与级别一起转发。
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
