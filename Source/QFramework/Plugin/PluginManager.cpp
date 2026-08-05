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
    ModuleConfig config;
    QPluginLoader* loader = nullptr;
    QObject* instance = nullptr;
    ModuleEndpoint* endpoint = nullptr;
    InProcessUiModule* uiModule = nullptr;
    bool started = false;
    bool quarantined = false;
};

// 保存中央 MessageBus 借用指针；实际 DLL 直到 loadAndStart() 才加载。
PluginManager::PluginManager(MessageBus* messageBus, QObject* parent)
    : QObject(parent),
      messageBus_(messageBus),
      shutdownComplete_(false)
{
}

PluginManager::~PluginManager()
{
    // 正常 FrameworkRuntime 会显式 shutdown；析构兜底防止插件 DLL 留在内存中。
    if (!shutdownComplete_) {
        messageBus_->beginShutdown();
        const MessageBusStopReport report = messageBus_->stopQueues(3000);
        shutdown(report.timedOutModuleIds);
    }
}

// 按配置顺序分两阶段加载和启动全部主进程插件；单个失败不会中止后续项。
bool PluginManager::loadAndStart(const QVector<ModuleConfig>& modules,
                                 QStringList* errors,
                                 bool enableDeliveryAfterStart)
{
    // 第一遍只加载和注册，避免早启动模块向尚未注册的模块发布消息。
    bool allSucceeded = true;
    for (const ModuleConfig& config : modules) {
        if (!config.enabled ||
            (config.type != ModuleType::InProcessUi &&
             config.type != ModuleType::InProcessNonUi)) {
            continue;
        }
        QString error;
        if (!loadOne(config, &error)) {
            allSucceeded = false;
            if (errors != nullptr)
                errors->append(error);
            emit moduleStateChanged(config.id, QStringLiteral("Failed"), error);
        }
    }

    // 使用快照，因为 start 失败时 removeFailed 会修改 loaded_ 容器。
    const QVector<LoadedPlugin*> loadedSnapshot = loaded_;
    for (LoadedPlugin* plugin : loadedSnapshot) {
        QString error;
        if (!startOne(plugin, &error)) {
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
QStringList PluginManager::shutdown(const QStringList& timedOutModuleIds)
{
    if (shutdownComplete_)
        return quarantinedModuleIds();

    QVector<LoadedPlugin*> quarantinedPlugins;
    for (int i = loaded_.size() - 1; i >= 0; --i) {
        LoadedPlugin* plugin = loaded_.at(i);
        if (timedOutModuleIds.contains(plugin->config.id) ||
            !messageBus_->isModuleQueueStopped(plugin->config.id) ||
            !releasePlugin(plugin)) {
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

QStringList PluginManager::retryQuarantinedShutdown()
{
    QVector<LoadedPlugin*> remaining;
    for (int i = loaded_.size() - 1; i >= 0; --i) {
        LoadedPlugin* plugin = loaded_.at(i);
        if (!messageBus_->isModuleQueueStopped(plugin->config.id) ||
            !releasePlugin(plugin)) {
            remaining.prepend(plugin);
        }
    }
    loaded_ = remaining;
    return quarantinedModuleIds();
}

// 按 loaded_ 顺序返回已成功执行 onStart 的模块 ID。
QStringList PluginManager::runningModuleIds() const
{
    QStringList result;
    for (const LoadedPlugin* plugin : loaded_) {
        if (plugin->started)
            result.append(plugin->config.id);
    }
    return result;
}

QStringList PluginManager::quarantinedModuleIds() const
{
    QStringList result;
    for (const LoadedPlugin* plugin : loaded_) {
        if (plugin->quarantined)
            result.append(plugin->config.id);
    }
    return result;
}

// 返回 UI 插件对象借用指针；非 UI、未知或失败模块返回 nullptr。
InProcessUiModule* PluginManager::uiModule(const QString& moduleId) const
{
    for (const LoadedPlugin* plugin : loaded_) {
        if (plugin->config.id == moduleId)
            return plugin->uiModule;
    }
    return nullptr;
}

// 加载一个 DLL：先验证路径和 JSON 元数据，再构造实例、校验基类并注册总线。
bool PluginManager::loadOne(const ModuleConfig& config, QString* errorMessage)
{
    // 文件名必须等于 ModuleId，降低配置指向错误 DLL 但仍能加载的风险。
    const QFileInfo fileInfo(config.filePath);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        *errorMessage = QString::fromUtf8(u8"模块文件不存在：%1").arg(config.filePath);
        return false;
    }
    if (fileInfo.completeBaseName().compare(config.id, Qt::CaseInsensitive) != 0) {
        *errorMessage = QString::fromUtf8(u8"模块 ID 必须等于 DLL 文件名：%1")
            .arg(config.id);
        return false;
    }

    LoadedPlugin* plugin = new LoadedPlugin;
    plugin->config = config;
    plugin->loader = new QPluginLoader(config.filePath);

    const QJsonObject rootMetadata = plugin->loader->metaData();
    // 在 instance() 执行插件代码前先校验 IID、ModuleId 和 ModuleType。
    const QJsonObject moduleMetadata = rootMetadata.value(QStringLiteral("MetaData")).toObject();
    const QString iid = rootMetadata.value(QStringLiteral("IID")).toString();
    const QString metadataId = moduleMetadata.value(QStringLiteral("ModuleId")).toString();
    const QString metadataType = moduleMetadata.value(QStringLiteral("ModuleType")).toString();
    if (iid != QLatin1String(QFRAMEWORK_PLUGIN_IID) ||
        metadataId != config.id ||
        metadataType != inProcessTypeName(config.type)) {
        *errorMessage = QString::fromUtf8(u8"模块 %1 的插件元数据与配置不匹配")
            .arg(config.id);
        delete plugin->loader;
        delete plugin;
        return false;
    }

    plugin->instance = plugin->loader->instance();
    // instance() 会实际载入 DLL 并构造 Qt 插件对象。
    if (plugin->instance == nullptr) {
        *errorMessage = QString::fromUtf8(u8"加载模块 %1 失败：%2")
            .arg(config.id, plugin->loader->errorString());
        delete plugin->loader;
        delete plugin;
        return false;
    }

    plugin->endpoint = dynamic_cast<ModuleEndpoint*>(plugin->instance);
    // 既检查统一接口，也检查 UI/非 UI 具体基类与配置类型是否一致。
    plugin->uiModule = dynamic_cast<InProcessUiModule*>(plugin->instance);
    InProcessNonUiModule* nonUi = dynamic_cast<InProcessNonUiModule*>(plugin->instance);
    const bool typeMatches =
        (config.type == ModuleType::InProcessUi && plugin->uiModule != nullptr) ||
        (config.type == ModuleType::InProcessNonUi && nonUi != nullptr);
    if (plugin->endpoint == nullptr || !typeMatches) {
        *errorMessage = QString::fromUtf8(u8"模块 %1 的基类与 Type 不匹配")
            .arg(config.id);
        plugin->loader->unload();
        delete plugin->loader;
        delete plugin;
        return false;
    }

    QString busError;
    // 只有元数据和基类均通过后才把主题注册到中央 MessageBus。
    if (!messageBus_->registerModule(config.id, plugin->endpoint, &busError)) {
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
bool PluginManager::startOne(LoadedPlugin* plugin, QString* errorMessage)
{
    // onStart 期间允许 publish，因此先把模块标记为运行；失败再撤销。
    messageBus_->setModuleRunning(plugin->config.id, true);
    bool started = false;
    try {
        started = plugin->endpoint->onStart();
    } catch (const std::exception& exception) {
        *errorMessage = QString::fromUtf8(u8"模块 %1 启动异常：%2")
            .arg(plugin->config.id, QString::fromUtf8(exception.what()));
    } catch (...) {
        *errorMessage = QString::fromUtf8(u8"模块 %1 启动发生未知异常")
            .arg(plugin->config.id);
    }
    if (!started) {
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
bool PluginManager::releasePlugin(LoadedPlugin* plugin)
{
    if (plugin == nullptr)
        return true;
    if (plugin->started) {
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
    if (!messageBus_->unregisterModule(plugin->config.id, false))
        return false;
    plugin->loader->unload();
    delete plugin->loader;
    delete plugin;
    return true;
}

// 清理启动失败的单个插件，不影响 loaded_ 中其余有效模块。
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
