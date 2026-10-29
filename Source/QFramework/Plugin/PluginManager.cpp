// 【阅读导引】管理加载到主进程的 DLL 模块。
// 阅读顺序：loadAndStart 分为加载注册和业务启动两轮，保证前面的模块发消息时后面的订阅者已经登记。
// 需要分清：loadOne 核对文件名、元数据、接口类型；uiModule 可能返回 nullptr。关闭时等消息回调结束再卸载 DLL，否则虚函数代码可能已经不存在。
// 这是执行实现：按入口调用和信号回调追踪，不要按文件行号把所有函数理解成连续执行。

// 文件职责：发现、加载、注册和卸载进程内 Qt 插件，并把模块主题交给 MessageBus。
// QPluginLoader 和插件对象由 PluginManager 所在线程拥有；插件 onStart/onStop/onMessage
// 不在这里跨线程直接调用，关闭时先停止发布入口，再等待消息队列按有界流程收尾。
#include "PluginManager.h"

#include <QFileInfo>
#include <QJsonObject>
#include <QPluginLoader>

#include <exception>

#include "InProcessNonUiModule.h"
#include "InProcessUiModule.h"
#include "Logger.h"
#include "MessageBus.h"
#include "QFrameworkPlugin.h"

// 插件启动分为 loadOne 和 startOne：先验证文件、元数据、基类并注册总线，
// 再调用业务 onStart。这样失败原因可以准确落在“加载”或“启动”阶段。

namespace qframework
{
namespace
{
// 把只允许进入本管理器的两种 ModuleType 转成插件元数据文本。
/// @brief 把主进程模块类型转换为插件 JSON 元数据中的类型文本。
/// @param type 待转换的模块类型。
/// @return InProcessUi 或 InProcessNonUi；其他类型返回空字符串。
QString inProcessTypeName(ModuleType type)
{
    // 插件 JSON 使用稳定字符串；非主进程类型返回空串并在校验时失败。
    if (type == ModuleType::InProcessUi)
        return QStringLiteral("InProcessUi");
    if (type == ModuleType::InProcessNonUi)
        return QStringLiteral("InProcessNonUi");
    return QString();
}
}

struct PluginManager::LoadedPlugin
{
    // loader 拥有 instance；endpoint/uiModule 都是指向同一插件对象的不同视图。
    ModuleConfig config; ///< 该插件的原始模块配置。
    QPluginLoader* loader = nullptr; ///< 插件加载器，拥有插件对象并负责卸载。
    QObject* instance = nullptr; ///< 插件根对象，由 loader 拥有。
    ModuleEndpoint* endpoint = nullptr; ///< instance 的端点视图，用于生命周期与发布。
    InProcessUiModule* uiModule = nullptr; ///< instance 的 UI 视图；非 UI 插件为 nullptr。
    bool started = false; ///< 是否已成功执行过 onStart。
    bool quarantined = false; ///< 停止超时需保留，禁止卸载其 DLL。
};

// 保存中央 MessageBus 借用指针；实际 DLL 直到 loadAndStart() 才加载。
/// @brief 构造插件管理器，保存中央消息总线借用指针。
/// @param messageBus 中央消息总线，所有权在 FrameworkRuntime。
/// @param parent 所属 QObject，可为空。
/// @return 无（构造函数不返回值）。
PluginManager::PluginManager(MessageBus* messageBus, QObject* parent)
    : QObject(parent),
      messageBus_(messageBus),
      shutdownComplete_(false)
{
}

/// @brief 向所有已启动插件广播进入退出准备阶段。
/// @return 无；跳过隔离中的插件。
void PluginManager::beginPrepareForExit()
{
    for (LoadedPlugin* plugin : loaded_) ///< plugin：逐个通知的已加载插件。
        if (plugin->started && !plugin->quarantined) plugin->endpoint->beginPrepareForExit();
}

/// @brief 查询是否所有已启动插件都已完成退出准备。
/// @return 全部就绪返回 true；任一插件未就绪返回 false。
bool PluginManager::isReadyForExit() const
{
    for (LoadedPlugin* plugin : loaded_) ///< plugin：逐个查询的已加载插件。
        if (plugin->started && !plugin->quarantined && !plugin->endpoint->isReadyForExit()) return false;
    return true;
}

QStringList PluginManager::pendingExitModules() const
{
    QStringList pending;
    for (LoadedPlugin* plugin : loaded_)
        if (plugin->started && !plugin->quarantined && !plugin->endpoint->isReadyForExit())
            pending.append(plugin->config.id);
    return pending;
}

/// @brief 取消所有已启动插件的退出准备状态。
/// @return 无；跳过隔离中的插件。
void PluginManager::cancelPrepareForExit()
{
    for (LoadedPlugin* plugin : loaded_) ///< plugin：逐个通知的已加载插件。
        if (plugin->started && !plugin->quarantined) plugin->endpoint->cancelPrepareForExit();
}

/// @brief 析构兜底：若尚未显式关闭，则停止总线并回收插件 DLL。
/// @return 无（析构函数不返回值）。
PluginManager::~PluginManager()
{
    // 正常 FrameworkRuntime 会显式 shutdown；析构兜底防止插件 DLL 留在内存中。
    if (!shutdownComplete_) {
        messageBus_->beginShutdown();
        const MessageBusStopReport report = messageBus_->stopQueues(3000); ///< 兜底停止结果，用于排除超时模块。
        shutdown(report.timedOutModuleIds);
    }
}

// 按配置顺序分两阶段加载和启动全部主进程插件；单个失败不会中止后续项。
/// @brief 按配置顺序分两轮加载并启动全部主进程插件。
/// @param modules 模块配置列表，非主进程类型会被跳过。
/// @param errors 可选的错误收集列表，可为空。
/// @param enableDeliveryAfterStart 启动完成后是否统一放行消息投递。
/// @return 所有插件都加载且启动成功返回 true，任一失败返回 false。
bool PluginManager::loadAndStart(const QVector<ModuleConfig>& modules,
                                 QStringList* errors,
                                 bool enableDeliveryAfterStart)
{
    // 第一遍只加载和注册，避免早启动模块向尚未注册的模块发布消息。
    bool allSucceeded = true; ///< 是否所有插件都成功加载并启动。
    for (const ModuleConfig& config : modules) { ///< config：当前待加载的模块配置。
        if (!config.enabled ||
            (config.type != ModuleType::InProcessUi &&
             config.type != ModuleType::InProcessNonUi)) {
            continue;
        }
        QString error; ///< 本模块的加载失败原因。
        if (!loadOne(config, &error)) { ///< 加载或注册失败：记录错误并通知状态变化。
            allSucceeded = false;
            if (errors != nullptr)
                errors->append(error);
            emit moduleStateChanged(config.id, QStringLiteral("Failed"), error);
        }
    }

    // 使用快照，因为 start 失败时 removeFailed 会修改 loaded_ 容器。
    const QVector<LoadedPlugin*> loadedSnapshot = loaded_; ///< 加载阶段的插件快照。
    for (LoadedPlugin* plugin : loadedSnapshot) { ///< plugin：当前待启动的插件。
        QString error; ///< 本模块的启动失败原因。
        if (!startOne(plugin, &error)) { ///< onStart 失败：记录错误、通知并回收。
            allSucceeded = false;
            if (errors != nullptr)
                errors->append(error);
            emit moduleStateChanged(plugin->config.id, QStringLiteral("Failed"), error);
            removeFailed(plugin);
        }
    }
    if (enableDeliveryAfterStart)
        messageBus_->setDeliveryEnabled(true);
    return allSucceeded;
}

// 只回收消息线程已经停止的插件；超时插件保留 loader/endpoint，不并发 onStop。
/// @brief 按逆序回收消息线程已停止的插件，超时者标记为隔离保留。
/// @param timedOutModuleIds 上游判定停止超时的模块 ID。
/// @return 关闭后仍需隔离保留的模块 ID 列表。
QStringList PluginManager::shutdown(const QStringList& timedOutModuleIds)
{
    if (shutdownComplete_)
        return quarantinedModuleIds();

    QVector<LoadedPlugin*> quarantinedPlugins; ///< 需要保留的隔离插件，作为新的 loaded_。
    for (int i = loaded_.size() - 1; i >= 0; --i) { ///< i：从后往前回收，保证依赖逆序。
        LoadedPlugin* plugin = loaded_.at(i); ///< 当前处理的插件。
        if (timedOutModuleIds.contains(plugin->config.id) ||
            !messageBus_->isModuleQueueStopped(plugin->config.id) ||
            !releasePlugin(plugin)) { ///< 超时或线程未停：标记隔离并保留。
            plugin->quarantined = true;
            quarantinedPlugins.prepend(plugin);
            emit moduleStateChanged(
                plugin->config.id,
                QStringLiteral("Quarantined"),
                QString::fromUtf8(u8"消息回调未在退出 deadline 内结束，保留 DLL"));
        }
    }
    loaded_ = quarantinedPlugins;
    shutdownComplete_ = true;
    return quarantinedModuleIds();
}

/// @brief 重试回收仍处于隔离状态的插件。
/// @return 本轮之后仍需隔离保留的模块 ID 列表。
QStringList PluginManager::retryQuarantinedShutdown()
{
    QVector<LoadedPlugin*> remaining; ///< 仍未成功回收的隔离插件。
    for (int i = loaded_.size() - 1; i >= 0; --i) { ///< i：从后往前重试回收。
        LoadedPlugin* plugin = loaded_.at(i); ///< 当前重试的插件。
        if (!messageBus_->isModuleQueueStopped(plugin->config.id) ||
            !releasePlugin(plugin)) { ///< 线程仍未停或卸载失败：继续保留。
            remaining.prepend(plugin);
        }
    }
    loaded_ = remaining;
    return quarantinedModuleIds();
}

// 按 loaded_ 顺序返回已成功执行 onStart 的模块 ID。
/// @brief 列出已成功执行 onStart 的模块 ID。
/// @return 按加载顺序排列的运行中模块 ID 列表。
QStringList PluginManager::runningModuleIds() const
{
    QStringList result; ///< 运行中模块 ID 收集结果。
    for (const LoadedPlugin* plugin : loaded_) { ///< plugin：逐个筛选的已加载插件。
        if (plugin->started)
            result.append(plugin->config.id);
    }
    return result;
}

/// @brief 列出因停止超时被隔离保留的模块 ID。
/// @return 处于隔离状态的模块 ID 列表。
QStringList PluginManager::quarantinedModuleIds() const
{
    QStringList result; ///< 隔离模块 ID 收集结果。
    for (const LoadedPlugin* plugin : loaded_) { ///< plugin：逐个筛选的已加载插件。
        if (plugin->quarantined)
            result.append(plugin->config.id);
    }
    return result;
}

// 返回 UI 插件对象借用指针；非 UI、未知或失败模块返回 nullptr。
/// @brief 查询指定模块对应的 UI 插件对象。
/// @param moduleId 目标模块 ID。
/// @return UI 插件借用指针；非 UI、未知或已失败时返回 nullptr。
InProcessUiModule* PluginManager::uiModule(const QString& moduleId) const
{
    for (const LoadedPlugin* plugin : loaded_) { ///< plugin：逐个匹配的已加载插件。
        if (plugin->config.id == moduleId)
            return plugin->uiModule;
    }
    return nullptr;
}

// 加载一个 DLL：先验证路径和 JSON 元数据，再构造实例、校验基类并注册总线。
/// @brief 加载并注册单个进程内插件 DLL，逐步校验路径、元数据和基类。
/// @param config 该模块的配置。
/// @param errorMessage 失败原因输出参数，不能为空。
/// @return 加载并注册成功返回 true，任一步骤失败返回 false。
bool PluginManager::loadOne(const ModuleConfig& config, QString* errorMessage)
{
    // 文件名必须等于 ModuleId，降低配置指向错误 DLL 但仍能加载的风险。
    const QFileInfo fileInfo(config.filePath); ///< 插件 DLL 的文件信息。
    if (!fileInfo.exists() || !fileInfo.isFile()) { ///< 文件不存在：拒绝加载。
        *errorMessage = QString::fromUtf8(u8"模块文件不存在：%1").arg(config.filePath);
        return false;
    }
    if (fileInfo.completeBaseName().compare(config.id, Qt::CaseInsensitive) != 0) { ///< 文件名与模块 ID 不一致：拒绝加载。
        *errorMessage = QString::fromUtf8(u8"模块 ID 必须等于 DLL 文件名：%1")
            .arg(config.id);
        return false;
    }

    LoadedPlugin* plugin = new LoadedPlugin; ///< 本次加载的所有权记录。
    plugin->config = config;
    plugin->loader = new QPluginLoader(config.filePath);

    const QJsonObject rootMetadata = plugin->loader->metaData(); ///< 插件根元数据。
    // 在 instance() 执行插件代码前先校验 IID、ModuleId 和 ModuleType。
    const QJsonObject moduleMetadata = rootMetadata.value(QStringLiteral("MetaData")).toObject(); ///< 插件自定义元数据对象。
    const QString iid = rootMetadata.value(QStringLiteral("IID")).toString(); ///< 插件接口标识。
    const QString metadataId = moduleMetadata.value(QStringLiteral("ModuleId")).toString(); ///< 元数据声明的模块 ID。
    const QString metadataType = moduleMetadata.value(QStringLiteral("ModuleType")).toString(); ///< 元数据声明的模块类型文本。
    if (iid != QLatin1String(QFRAMEWORK_PLUGIN_IID) ||
        metadataId != config.id ||
        metadataType != inProcessTypeName(config.type)) { ///< 元数据与配置不符：拒绝加载。
        *errorMessage = QString::fromUtf8(u8"模块 %1 的插件元数据与配置不匹配")
            .arg(config.id);
        delete plugin->loader;
        delete plugin;
        return false;
    }

    plugin->instance = plugin->loader->instance();
    // instance() 会实际载入 DLL 并构造 Qt 插件对象。
    if (plugin->instance == nullptr) { ///< 实例化失败：上报加载器错误。
        *errorMessage = QString::fromUtf8(u8"加载模块 %1 失败：%2")
            .arg(config.id, plugin->loader->errorString());
        delete plugin->loader;
        delete plugin;
        return false;
    }

    plugin->endpoint = dynamic_cast<ModuleEndpoint*>(plugin->instance);
    // 既检查统一接口，也检查 UI/非 UI 具体基类与配置类型是否一致。
    plugin->uiModule = dynamic_cast<InProcessUiModule*>(plugin->instance);
    InProcessNonUiModule* nonUi = dynamic_cast<InProcessNonUiModule*>(plugin->instance); ///< 非 UI 基类视图，用于类型一致性判断。
    const bool typeMatches =
        (config.type == ModuleType::InProcessUi && plugin->uiModule != nullptr) ||
        (config.type == ModuleType::InProcessNonUi && nonUi != nullptr); ///< 具体基类是否与配置类型一致。
    if (plugin->endpoint == nullptr || !typeMatches) { ///< 缺少统一接口或类型不匹配：卸载并拒绝。
        *errorMessage = QString::fromUtf8(u8"模块 %1 的基类与 Type 不匹配")
            .arg(config.id);
        plugin->loader->unload();
        delete plugin->loader;
        delete plugin;
        return false;
    }

    QString busError; ///< 总线注册失败原因。
    // 只有元数据和基类均通过后才把主题注册到中央 MessageBus。
    if (!messageBus_->registerModule(config.id, plugin->endpoint, &busError)) { ///< 总线注册失败：卸载并拒绝。
        *errorMessage = QString::fromUtf8(u8"注册模块 %1 失败：%2")
            .arg(config.id, busError);
        plugin->loader->unload();
        delete plugin->loader;
        delete plugin;
        return false;
    }
    loaded_.append(plugin);
    emit moduleStateChanged(config.id, QStringLiteral("Loaded"), QString());
    return true;
}

// 运行一个已加载插件的 onStart；异常或 false 都撤销发布权限并返回错误。
/// @brief 执行插件的 onStart，失败或抛异常时撤销发布权限。
/// @param plugin 已加载的插件记录。
/// @param errorMessage 失败原因输出参数，不能为空。
/// @return onStart 返回 true 且无异常时返回 true，否则返回 false。
bool PluginManager::startOne(LoadedPlugin* plugin, QString* errorMessage)
{
    // onStart 期间允许 publish，因此先把模块标记为运行；失败再撤销。
    messageBus_->setModuleRunning(plugin->config.id, true);
    bool started = false; ///< onStart 的返回值，默认视为失败。
    try {
        started = plugin->endpoint->onStart();
    } catch (const std::exception& exception) {
        *errorMessage = QString::fromUtf8(u8"模块 %1 启动异常：%2")
            .arg(plugin->config.id, QString::fromUtf8(exception.what()));
    } catch (...) {
        *errorMessage = QString::fromUtf8(u8"模块 %1 启动发生未知异常")
            .arg(plugin->config.id);
    }
    if (!started) { ///< 启动失败：撤销发布权限并补齐默认错误文本。
        messageBus_->setModuleRunning(plugin->config.id, false);
        if (errorMessage->isEmpty())
            *errorMessage = QString::fromUtf8(u8"模块 %1 的 onStart 返回 false")
                .arg(plugin->config.id);
        return false;
    }

    plugin->started = true;
    emit moduleStateChanged(plugin->config.id, QStringLiteral("Running"), QString());
    return true;
}

// 前置条件是消息队列已经停止；否则绝不能并发调用 onStop 或卸载其 DLL。
/// @brief 停止并卸载单个插件，要求其消息队列已停止。
/// @param plugin 待回收的插件记录。
/// @return 成功卸载返回 true；队列仍在运行导致注销失败时返回 false。
bool PluginManager::releasePlugin(LoadedPlugin* plugin)
{
    if (plugin == nullptr)
        return true;
    if (plugin->started) { ///< 曾成功启动：先调用 onStop 并捕获异常。
        try {
            plugin->endpoint->onStop();
        } catch (const std::exception& exception) {
            Logger::instance().log(
                LogLevel::Error,
                plugin->config.id,
                QString::fromUtf8(u8"onStop 异常：%1")
                    .arg(QString::fromUtf8(exception.what())));
        } catch (...) {
            Logger::instance().log(
                LogLevel::Error,
                plugin->config.id,
                QString::fromUtf8(u8"onStop 未知异常"));
        }
        plugin->started = false;
    }
    if (!messageBus_->unregisterModule(plugin->config.id, false)) ///< 总线注销失败：保留对象，交由隔离逻辑处理。
        return false;
    plugin->loader->unload();
    delete plugin->loader;
    delete plugin;
    return true;
}

// 清理启动失败的单个插件，不影响 loaded_ 中其余有效模块。
/// @brief 回收启动失败的插件，尽力执行 onStop 后注销并卸载。
/// @param plugin 启动失败的插件记录。
/// @return 无。
void PluginManager::removeFailed(LoadedPlugin* plugin)
{
    // 即使 onStart 失败也尝试一次 onStop，让模块释放已创建的局部资源。
    try {
        plugin->endpoint->onStop();
    } catch (...) {
        Logger::instance().log(
            LogLevel::Error,
            plugin->config.id,
            QString::fromUtf8(u8"失败模块清理异常"));
    }
    messageBus_->unregisterModule(plugin->config.id, false);
    loaded_.removeOne(plugin);
    plugin->loader->unload();
    delete plugin->loader;
    delete plugin;
}
}
