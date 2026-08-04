#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include "QFrameworkGlobal.h"

namespace qframework
{
enum class ModuleType
{
    InProcessUi,
    InProcessNonUi,
    ProcessUi,
    ProcessNonUi
};

enum class QueuePolicy
{
    Reliable,
    Latest
};

struct ModuleConfig
{
    QString id;
    bool enabled = true;
    ModuleType type = ModuleType::InProcessNonUi;
    QString displayName;
    QString filePath;
    bool waitForDebugger = false;
    int debuggerWaitTimeoutMs = 30000;
};

struct TopicConfig
{
    int queueCapacity = 0;
    int maxMessageBytes = 0;
    QueuePolicy policy = QueuePolicy::Reliable;
};

struct MessageBusConfig
{
    int defaultQueueCapacity = 256;
    int maxMessageBytes = 16 * 1024 * 1024;
    int sharedMemoryThresholdBytes = 1024 * 1024;
    int shutdownDrainTimeoutMs = 3000;
    QueuePolicy defaultPolicy = QueuePolicy::Reliable;
    QHash<QString, TopicConfig> topics;
};

struct ProcessConfig
{
    int registrationTimeoutMs = 10000;
    int heartbeatIntervalMs = 1000;
    int heartbeatTimeoutMs = 5000;
    int stopTimeoutMs = 5000;
    int restartDelayMs = 1000;
    int restartWindowMs = 60000;
    int maxRestartCount = 3;
};

struct LayoutConfig
{
    QString startupFile;
};

struct StyleConfig
{
    QString file;
};

struct LoggingConfig
{
    QString directory;
    qint64 maxFileBytes = 10 * 1024 * 1024;
};

class QFRAMEWORK_EXPORT FrameworkConfig
{
public:
    bool load(const QString& iniFilePath, QString* errorMessage = nullptr);

    QString iniFilePath() const;
    QString configDirectory() const;
    QVector<ModuleConfig> modules() const;
    MessageBusConfig messageBus() const;
    ProcessConfig process() const;
    LayoutConfig layout() const;
    StyleConfig style() const;
    LoggingConfig logging() const;

    QString resolvePath(const QString& path) const;
    static bool parseModuleType(const QString& value, ModuleType* type);
    static bool parseQueuePolicy(const QString& value, QueuePolicy* policy);

private:
    QString iniFilePath_;
    QString configDirectory_;
    QVector<ModuleConfig> modules_;
    MessageBusConfig messageBus_;
    ProcessConfig process_;
    LayoutConfig layout_;
    StyleConfig style_;
    LoggingConfig logging_;
};
}
