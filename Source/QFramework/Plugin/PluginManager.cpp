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

namespace qframework
{
namespace
{
QString inProcessTypeName(ModuleType type)
{
    if (type == ModuleType::InProcessUi)
        return QStringLiteral("InProcessUi");
    if (type == ModuleType::InProcessNonUi)
        return QStringLiteral("InProcessNonUi");
    return QString();
}
}

struct PluginManager::LoadedPlugin
{
    ModuleConfig config;
    QPluginLoader* loader = nullptr;
    QObject* instance = nullptr;
    ModuleEndpoint* endpoint = nullptr;
    InProcessUiModule* uiModule = nullptr;
    bool started = false;
};

PluginManager::PluginManager(MessageBus* messageBus, QObject* parent)
    : QObject(parent),
      messageBus_(messageBus),
      shutdownComplete_(false)
{
}

PluginManager::~PluginManager()
{
    if (!shutdownComplete_)
        shutdown(3000);
}

bool PluginManager::loadAndStart(const QVector<ModuleConfig>& modules,
                                 QStringList* errors,
                                 bool enableDeliveryAfterStart)
{
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

void PluginManager::shutdown(int drainTimeoutMs)
{
    if (shutdownComplete_)
        return;
    messageBus_->beginShutdown();
    messageBus_->stopQueues(drainTimeoutMs);

    for (int i = loaded_.size() - 1; i >= 0; --i) {
        LoadedPlugin* plugin = loaded_.at(i);
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
        messageBus_->unregisterModule(plugin->config.id, false);
        plugin->loader->unload();
        delete plugin->loader;
        delete plugin;
    }
    loaded_.clear();
    shutdownComplete_ = true;
}

QStringList PluginManager::runningModuleIds() const
{
    QStringList result;
    for (const LoadedPlugin* plugin : loaded_) {
        if (plugin->started)
            result.append(plugin->config.id);
    }
    return result;
}

InProcessUiModule* PluginManager::uiModule(const QString& moduleId) const
{
    for (const LoadedPlugin* plugin : loaded_) {
        if (plugin->config.id == moduleId)
            return plugin->uiModule;
    }
    return nullptr;
}

bool PluginManager::loadOne(const ModuleConfig& config, QString* errorMessage)
{
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
    if (plugin->instance == nullptr) {
        *errorMessage = QString::fromUtf8(u8"加载模块 %1 失败：%2")
            .arg(config.id, plugin->loader->errorString());
        delete plugin->loader;
        delete plugin;
        return false;
    }

    plugin->endpoint = dynamic_cast<ModuleEndpoint*>(plugin->instance);
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

bool PluginManager::startOne(LoadedPlugin* plugin, QString* errorMessage)
{
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

void PluginManager::removeFailed(LoadedPlugin* plugin)
{
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
