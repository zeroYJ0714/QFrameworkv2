#pragma once

#include <QHash>
#include <QMutex>
#include <QStringList>

#include "FrameworkConfig.h"
#include "ModuleEndpoint.h"
#include "QFrameworkGlobal.h"

namespace qframework
{
struct ModuleQueueStats
{
    quint64 delivered = 0;
    quint64 dropped = 0;
    quint64 rejected = 0;
};

class QFRAMEWORK_EXPORT MessageBus : public ModuleHost
{
public:
    explicit MessageBus(const MessageBusConfig& config);
    ~MessageBus() override;

    bool registerModule(const QString& moduleId,
                        ModuleEndpoint* endpoint,
                        QString* errorMessage = nullptr);
    bool setModuleRunning(const QString& moduleId, bool running);
    void setDeliveryEnabled(bool enabled);
    void beginShutdown();
    bool stopQueues(int drainTimeoutMs);
    bool unregisterModule(const QString& moduleId, bool drainPendingMessages);

    QStringList moduleIds() const;
    ModuleQueueStats queueStats(const QString& moduleId) const;

    bool publishFromModule(const QString& moduleId,
                           const QString& topic,
                           const QByteArray& data) override;
    void logFromModule(LogLevel level,
                       const QString& moduleId,
                       const QString& text) override;

private:
    struct Registration;

    TopicConfig topicConfig(const QString& topic) const;
    void logRejected(const QString& moduleId, const QString& reason) const;

    MessageBusConfig config_;
    mutable QMutex mutex_;
    QHash<QString, Registration*> modules_;
    bool accepting_;
    bool deliveryEnabled_;
};
}
