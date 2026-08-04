#pragma once

#include <QObject>
#include <QVector>

#include "FrameworkConfig.h"
#include "QFrameworkGlobal.h"

namespace qframework
{
class InProcessUiModule;
class MessageBus;

class QFRAMEWORK_EXPORT PluginManager : public QObject
{
    Q_OBJECT

public:
    explicit PluginManager(MessageBus* messageBus, QObject* parent = nullptr);
    ~PluginManager() override;

    bool loadAndStart(const QVector<ModuleConfig>& modules,
                      QStringList* errors = nullptr,
                      bool enableDeliveryAfterStart = true);
    void shutdown(int drainTimeoutMs);

    QStringList runningModuleIds() const;
    InProcessUiModule* uiModule(const QString& moduleId) const;

signals:
    void moduleStateChanged(const QString& moduleId,
                            const QString& state,
                            const QString& detail);

private:
    struct LoadedPlugin;

    bool loadOne(const ModuleConfig& config, QString* errorMessage);
    bool startOne(LoadedPlugin* plugin, QString* errorMessage);
    void removeFailed(LoadedPlugin* plugin);

    MessageBus* messageBus_;
    QVector<LoadedPlugin*> loaded_;
    bool shutdownComplete_;
};
}
