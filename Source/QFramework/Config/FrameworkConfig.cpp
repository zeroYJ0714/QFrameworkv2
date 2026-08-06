#include "FrameworkConfig.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

// 解析流程严格按 INI 分组进行：Modules -> MessageBus/Topic -> Process ->
// Layout.n/Style/Logging。任一必需字段无效时立即返回，不保留“部分成功”语义。

namespace qframework
{
namespace
{
// 把 QSettings 的任意 QVariant 转成正整数。
// value 缺失、不是整数或小于等于 0 时使用调用方给出的安全默认值。
int positiveValue(const QVariant& value, int defaultValue)
{
    // 所有容量和超时都必须为正数；缺失、格式错误、0 和负数使用默认值。
    bool ok = false;
    const int result = value.toInt(&ok);
    return ok && result > 0 ? result : defaultValue;
}
}

// 完整读取一份 INI 快照。成功时所有分组均已完成解析；失败时返回 false 并
// 通过可选 errorMessage 告诉调用方具体字段，运行期不会使用半成品配置。
bool FrameworkConfig::load(const QString& iniFilePath, QString* errorMessage)
{
    // 先验证真实文件，避免 QSettings 对不存在路径静默创建空配置的行为。
    const QFileInfo fileInfo(iniFilePath);
    if (!fileInfo.exists() || !fileInfo.isFile()) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"配置文件不存在：%1").arg(iniFilePath);
        return false;
    }

    // QSettings 只负责读取 INI；FrameworkConfig 把结果复制到内存结构后，
    // 运行期间不会反向写回配置文件。
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

    // Names 是唯一加载清单；没有列出的 Module.* 分组不会被自动发现。
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

        // 先结束分组再做错误返回，保持 QSettings 的分组栈成对。
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
    // 读取全局默认值，后面的 Topic.<name> 只覆盖指定主题。
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
    // childGroups 返回所有顶层分组，只处理 Topic. 前缀。
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
    // 所有监督器等待值都有安全默认值，避免配置缺失形成无限等待。
    process_.registrationTimeoutMs = positiveValue(settings.value(QStringLiteral("RegistrationTimeoutMs")), 10000);
    process_.heartbeatIntervalMs = positiveValue(settings.value(QStringLiteral("HeartbeatIntervalMs")), 1000);
    process_.heartbeatTimeoutMs = positiveValue(settings.value(QStringLiteral("HeartbeatTimeoutMs")), 5000);
    process_.stopTimeoutMs = positiveValue(settings.value(QStringLiteral("StopTimeoutMs")), 5000);
    process_.restartDelayMs = positiveValue(settings.value(QStringLiteral("RestartDelayMs")), 1000);
    process_.restartWindowMs = positiveValue(settings.value(QStringLiteral("RestartWindowMs")), 60000);
    process_.maxRestartCount = positiveValue(settings.value(QStringLiteral("MaxRestartCount")), 3);
    settings.endGroup();

    layout_.presets.clear();
    // 布局编号由配置维护者保证从 1 连续递增；遇到第一个缺失分组即结束读取。
    // File 允许为空或指向暂时不存在的文件，MainWindow 会保留菜单项并显示校验原因。
    const QStringList topLevelGroups = settings.childGroups();
    for (int index = 1; ; ++index) {
        const QString groupName = QStringLiteral("Layout.%1").arg(index);
        if (!topLevelGroups.contains(groupName))
            break;

        settings.beginGroup(groupName);
        LayoutPresetConfig preset;
        preset.index = index;
        preset.name = settings.value(QStringLiteral("Name")).toString().trimmed();
        preset.filePath = resolvePath(
            settings.value(QStringLiteral("File")).toString());
        settings.endGroup();
        layout_.presets.append(preset);
    }
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
    logging_.flushIntervalMs = positiveValue(
        settings.value(QStringLiteral("FlushIntervalMs")), 100);
    // 缺失、0 或负数都由 positiveValue 回退到 100 ms，保证日志线程不会
    // 因为配置错误进入忙等，也不会让普通日志无限期停在 QFile 缓冲区。
    settings.endGroup();
    return true;
}

// 以下 getter 都返回值对象而不是可写引用，保证配置快照不会被外部偷偷修改。
// iniFilePath 是实际加载文件的绝对路径。
QString FrameworkConfig::iniFilePath() const { return iniFilePath_; }
// configDirectory 是解析其他相对路径的基准目录。
QString FrameworkConfig::configDirectory() const { return configDirectory_; }
// modules 保留 [Modules]/Names 中声明的顺序。
QVector<ModuleConfig> FrameworkConfig::modules() const { return modules_; }
// 三类配置 getter 返回各自完整结构副本。
MessageBusConfig FrameworkConfig::messageBus() const { return messageBus_; }
ProcessConfig FrameworkConfig::process() const { return process_; }
LayoutConfig FrameworkConfig::layout() const { return layout_; }
StyleConfig FrameworkConfig::style() const { return style_; }
LoggingConfig FrameworkConfig::logging() const { return logging_; }

// 统一把配置路径转换成干净路径；相对值永远以 INI 目录而非进程 cwd 为基准。
QString FrameworkConfig::resolvePath(const QString& path) const
{
    // 空路径代表“未配置可选文件”，不是当前目录。
    if (path.trimmed().isEmpty())
        return QString();
    const QFileInfo fileInfo(path);
    if (fileInfo.isAbsolute())
        return QDir::cleanPath(fileInfo.absoluteFilePath());
    return QDir::cleanPath(QDir(configDirectory_).absoluteFilePath(path));
}

// 把 INI 中的稳定模块类型文本写入 type；输出指针为空或文本未知时失败。
bool FrameworkConfig::parseModuleType(const QString& value, ModuleType* type)
{
    // ModuleType 采用大小写敏感的稳定配置值，及时暴露拼写错误。
    if (type == nullptr)
        return false;
    if (value == QStringLiteral("InProcessUi")) *type = ModuleType::InProcessUi;
    else if (value == QStringLiteral("InProcessNonUi")) *type = ModuleType::InProcessNonUi;
    else if (value == QStringLiteral("ProcessUi")) *type = ModuleType::ProcessUi;
    else if (value == QStringLiteral("ProcessNonUi")) *type = ModuleType::ProcessNonUi;
    else return false;
    return true;
}

// 把策略文本转换为 Reliable/Latest；允许大小写差异但不接受第三种隐含策略。
bool FrameworkConfig::parseQueuePolicy(const QString& value, QueuePolicy* policy)
{
    // 队列策略允许大小写差异，但只接受 Reliable 和 Latest 两个值。
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
