#include "FrameworkConfig.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace qframework
{
namespace
{
int positiveValue(const QVariant& value, int defaultValue)
{
    bool ok = false;
    const int result = value.toInt(&ok);
    return ok && result > 0 ? result : defaultValue;
}
}

bool FrameworkConfig::load(const QString& iniFilePath, QString* errorMessage)
{
    const QFileInfo fileInfo(iniFilePath);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"配置文件不存在：%1").arg(iniFilePath);
        return false;
    }

    QSettings settings(fileInfo.absoluteFilePath(), QSettings::IniFormat);
    settings.setIniCodec("UTF-8");
    if (settings.status() != QSettings::NoError) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"无法读取配置文件：%1").arg(iniFilePath);
        return false;
    }

    iniFilePath_ = fileInfo.absoluteFilePath();
    configDirectory_ = fileInfo.absoluteDir().absolutePath();
    modules_.clear();

    settings.beginGroup(QStringLiteral("Modules"));
    const QStringList moduleIds = settings.value(QStringLiteral("Names")).toStringList();
    settings.endGroup();

    for (const QString& rawId : moduleIds) {
        const QString moduleId = rawId.trimmed();
        if (moduleId.isEmpty())
            continue;

        settings.beginGroup(QStringLiteral("Module.%1").arg(moduleId));
        ModuleConfig module;
        module.id = moduleId;
        module.enabled = settings.value(QStringLiteral("Enabled"), true).toBool();
        module.displayName = settings.value(QStringLiteral("DisplayName"), moduleId).toString();
        module.filePath = resolvePath(settings.value(QStringLiteral("FilePath")).toString());
        module.waitForDebugger = settings.value(QStringLiteral("WaitForDebugger"), false).toBool();
        module.debuggerWaitTimeoutMs = positiveValue(
            settings.value(QStringLiteral("DebuggerWaitTimeoutMs")), 30000);

        const QString typeValue = settings.value(QStringLiteral("Type")).toString();
        settings.endGroup();
        if (!parseModuleType(typeValue, &module.type)) {
            if (errorMessage != nullptr) {
                *errorMessage = QString::fromUtf8(u8"模块 %1 的 Type 无效：%2")
                    .arg(moduleId, typeValue);
            }
            return false;
        }
        if (module.filePath.isEmpty()) {
            if (errorMessage != nullptr)
                *errorMessage = QString::fromUtf8(u8"模块 %1 缺少 FilePath").arg(moduleId);
            return false;
        }
        modules_.append(module);
    }

    settings.beginGroup(QStringLiteral("MessageBus"));
    messageBus_.defaultQueueCapacity = positiveValue(
        settings.value(QStringLiteral("DefaultQueueCapacity")), 256);
    messageBus_.maxMessageBytes = positiveValue(
        settings.value(QStringLiteral("MaxMessageBytes")), 16 * 1024 * 1024);
    messageBus_.sharedMemoryThresholdBytes = positiveValue(
        settings.value(QStringLiteral("SharedMemoryThresholdBytes")), 1024 * 1024);
    messageBus_.shutdownDrainTimeoutMs = positiveValue(
        settings.value(QStringLiteral("ShutdownDrainTimeoutMs")), 3000);
    const QString defaultPolicy = settings.value(
        QStringLiteral("DefaultPolicy"), QStringLiteral("Reliable")).toString();
    settings.endGroup();
    if (!parseQueuePolicy(defaultPolicy, &messageBus_.defaultPolicy)) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"MessageBus/DefaultPolicy 无效：%1").arg(defaultPolicy);
        return false;
    }

    messageBus_.topics.clear();
    const QStringList groups = settings.childGroups();
    for (const QString& group : groups) {
        if (!group.startsWith(QStringLiteral("Topic.")))
            continue;

        const QString topic = group.mid(6);
        settings.beginGroup(group);
        TopicConfig topicConfig;
        topicConfig.queueCapacity = positiveValue(
            settings.value(QStringLiteral("QueueCapacity")),
            messageBus_.defaultQueueCapacity);
        topicConfig.maxMessageBytes = positiveValue(
            settings.value(QStringLiteral("MaxMessageBytes")),
            messageBus_.maxMessageBytes);
        const QString policy = settings.value(
            QStringLiteral("Policy"), defaultPolicy).toString();
        settings.endGroup();
        if (!parseQueuePolicy(policy, &topicConfig.policy)) {
            if (errorMessage != nullptr)
                *errorMessage = QString::fromUtf8(u8"主题 %1 的 Policy 无效：%2").arg(topic, policy);
            return false;
        }
        messageBus_.topics.insert(topic, topicConfig);
    }

    settings.beginGroup(QStringLiteral("Process"));
    process_.registrationTimeoutMs = positiveValue(settings.value(QStringLiteral("RegistrationTimeoutMs")), 10000);
    process_.heartbeatIntervalMs = positiveValue(settings.value(QStringLiteral("HeartbeatIntervalMs")), 1000);
    process_.heartbeatTimeoutMs = positiveValue(settings.value(QStringLiteral("HeartbeatTimeoutMs")), 5000);
    process_.stopTimeoutMs = positiveValue(settings.value(QStringLiteral("StopTimeoutMs")), 5000);
    process_.restartDelayMs = positiveValue(settings.value(QStringLiteral("RestartDelayMs")), 1000);
    process_.restartWindowMs = positiveValue(settings.value(QStringLiteral("RestartWindowMs")), 60000);
    process_.maxRestartCount = positiveValue(settings.value(QStringLiteral("MaxRestartCount")), 3);
    settings.endGroup();

    settings.beginGroup(QStringLiteral("Layout"));
    layout_.startupFile = resolvePath(settings.value(QStringLiteral("StartupFile")).toString());
    settings.endGroup();
    settings.beginGroup(QStringLiteral("Style"));
    style_.file = resolvePath(settings.value(QStringLiteral("File")).toString());
    settings.endGroup();
    settings.beginGroup(QStringLiteral("Logging"));
    logging_.directory = resolvePath(settings.value(
        QStringLiteral("Directory"), QStringLiteral("../Logs")).toString());
    logging_.maxFileBytes = settings.value(
        QStringLiteral("MaxFileBytes"), 10 * 1024 * 1024).toLongLong();
    if (logging_.maxFileBytes <= 0)
        logging_.maxFileBytes = 10 * 1024 * 1024;
    settings.endGroup();
    return true;
}

QString FrameworkConfig::iniFilePath() const { return iniFilePath_; }
QString FrameworkConfig::configDirectory() const { return configDirectory_; }
QVector<ModuleConfig> FrameworkConfig::modules() const { return modules_; }
MessageBusConfig FrameworkConfig::messageBus() const { return messageBus_; }
ProcessConfig FrameworkConfig::process() const { return process_; }
LayoutConfig FrameworkConfig::layout() const { return layout_; }
StyleConfig FrameworkConfig::style() const { return style_; }
LoggingConfig FrameworkConfig::logging() const { return logging_; }

QString FrameworkConfig::resolvePath(const QString& path) const
{
    if (path.trimmed().isEmpty())
        return QString();
    const QFileInfo fileInfo(path);
    if (fileInfo.isAbsolute())
        return QDir::cleanPath(fileInfo.absoluteFilePath());
    return QDir::cleanPath(QDir(configDirectory_).absoluteFilePath(path));
}

bool FrameworkConfig::parseModuleType(const QString& value, ModuleType* type)
{
    if (type == nullptr)
        return false;
    if (value == QStringLiteral("InProcessUi")) *type = ModuleType::InProcessUi;
    else if (value == QStringLiteral("InProcessNonUi")) *type = ModuleType::InProcessNonUi;
    else if (value == QStringLiteral("ProcessUi")) *type = ModuleType::ProcessUi;
    else if (value == QStringLiteral("ProcessNonUi")) *type = ModuleType::ProcessNonUi;
    else return false;
    return true;
}

bool FrameworkConfig::parseQueuePolicy(const QString& value, QueuePolicy* policy)
{
    if (policy == nullptr)
        return false;
    if (value.compare(QStringLiteral("Reliable"), Qt::CaseInsensitive) == 0)
        *policy = QueuePolicy::Reliable;
    else if (value.compare(QStringLiteral("Latest"), Qt::CaseInsensitive) == 0)
        *policy = QueuePolicy::Latest;
    else
        return false;
    return true;
}
}
