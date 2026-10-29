// 【阅读导引】把 INI 配置翻译成程序能使用的结构体。
// 阅读顺序：先读 load：解析模块清单，再解析消息容量、进程时限、布局和日志；resolvePath 把相对路径固定到配置文件目录。
// 需要分清：配置是启动时的快照；模块是否存在、是否成功启动由运行时处理，配置读取成功不等于插件已可用。
// 这是执行实现：按入口调用和信号回调追踪，不要按文件行号把所有函数理解成连续执行。

// 文件职责：读取和校验 QFramework 的本地配置文件，并生成模块/进程运行配置。
// 配置对象是值类型，解析过程不创建 UI、QProcess 或数据库连接；调用方在 GUI/框架线程
// 取得副本后再把具体工作交给对应 Worker。错误通过返回值和 QString 输出，不抛出异常。
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
/// @brief 把配置值转成正整数，非法值回退到调用方给定的默认值。
/// @param value QSettings 读出的原始值。
/// @param defaultValue 值非法时使用的安全默认值。
/// @return 大于 0 的整数值，或 defaultValue。
int positiveValue(const QVariant& value, int defaultValue)
{
    // 所有容量和超时都必须为正数；缺失、格式错误、0 和负数使用默认值。
    bool ok = false; ///< 承接 QVariant 到 int 的转换是否成功。
    const int result = value.toInt(&ok); ///< 转换后的整数值，仅在 ok 为真时有意义。
    return ok && result > 0 ? result : defaultValue;
}
}

// 完整读取一份 INI 快照。成功时所有分组均已完成解析；失败时返回 false 并
// 通过可选 errorMessage 告诉调用方具体字段，运行期不会使用半成品配置。
/// @brief 读取并校验整份 INI 配置，生成模块、消息总线、进程、布局、样式和日志配置。
/// @param iniFilePath 待加载的 INI 文件路径。
/// @param errorMessage 可选的失败原因输出参数，可为空。
/// @return 全部必需字段合法时返回 true；文件缺失或字段无效时返回 false。
bool FrameworkConfig::load(const QString& iniFilePath, QString* errorMessage)
{
    // 先验证真实文件，避免 QSettings 对不存在路径静默创建空配置的行为。
    const QFileInfo fileInfo(iniFilePath); ///< 配置文件信息，用于校验存在性并取绝对路径。
    if (!fileInfo.exists() || !fileInfo.isFile()) { ///< 文件不存在：直接报错，不创建空配置。
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"配置文件不存在：%1").arg(iniFilePath);
        return false;
    }

    // QSettings 只负责读取 INI；FrameworkConfig 把结果复制到内存结构后，
    // 运行期间不会反向写回配置文件。
    QSettings settings(fileInfo.absoluteFilePath(), QSettings::IniFormat); ///< 只读的 INI 读取器。
    settings.setIniCodec("UTF-8");
    if (settings.status() != QSettings::NoError) { ///< 读取器状态异常：上报无法读取。
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"无法读取配置文件：%1").arg(iniFilePath);
        return false;
    }

    iniFilePath_ = fileInfo.absoluteFilePath();
    configDirectory_ = fileInfo.absoluteDir().absolutePath();
    modules_.clear();

    settings.beginGroup(QStringLiteral("Modules"));
    const QStringList moduleIds = settings.value(QStringLiteral("Names")).toStringList(); ///< 唯一加载清单中声明的模块 ID。
    settings.endGroup();

    // Names 是唯一加载清单；没有列出的 Module.* 分组不会被自动发现。
    for (const QString& rawId : moduleIds) { ///< rawId：清单中的原始模块 ID 文本。
        const QString moduleId = rawId.trimmed(); ///< 去除首尾空白后的模块 ID。
        if (moduleId.isEmpty())
            continue;

        settings.beginGroup(QStringLiteral("Module.%1").arg(moduleId));
        ModuleConfig module; ///< 当前模块的配置，逐字段填充后追加到 modules_。
        module.id = moduleId;
        module.enabled = settings.value(QStringLiteral("Enabled"), true).toBool();
        module.displayName = settings.value(QStringLiteral("DisplayName"), moduleId).toString();
        module.filePath = resolvePath(settings.value(QStringLiteral("FilePath")).toString());
        module.waitForDebugger = settings.value(QStringLiteral("WaitForDebugger"), false).toBool();
        module.debuggerWaitTimeoutMs = positiveValue(
            settings.value(QStringLiteral("DebuggerWaitTimeoutMs")), 30000);

        // 先结束分组再做错误返回，保持 QSettings 的分组栈成对。
        const QString typeValue = settings.value(QStringLiteral("Type")).toString(); ///< 模块类型文本，用于解析 ModuleType。
        settings.endGroup();
        if (!parseModuleType(typeValue, &module.type)) { ///< 类型文本无法识别：整体加载失败。
            if (errorMessage != nullptr) {
                *errorMessage = QString::fromUtf8(u8"模块 %1 的 Type 无效：%2")
                    .arg(moduleId, typeValue);
            }
            return false;
        }
        if (module.filePath.isEmpty()) { ///< 缺少必需的文件路径：整体加载失败。
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
        QStringLiteral("DefaultPolicy"), QStringLiteral("Reliable")).toString(); ///< 全局默认队列策略文本。
    settings.endGroup();
    if (!parseQueuePolicy(defaultPolicy, &messageBus_.defaultPolicy)) { ///< 默认策略文本无效：整体加载失败。
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"MessageBus/DefaultPolicy 无效：%1").arg(defaultPolicy);
        return false;
    }

    messageBus_.topics.clear();
    const QStringList groups = settings.childGroups(); ///< 所有顶层分组名，用于筛选 Topic. 前缀项。
    // childGroups 返回所有顶层分组，只处理 Topic. 前缀。
    for (const QString& group : groups) { ///< group：当前待判断的顶层分组名。
        if (!group.startsWith(QStringLiteral("Topic.")))
            continue;

        const QString topic = group.mid(6); ///< 去掉 "Topic." 前缀后的主题名。
        settings.beginGroup(group);
        TopicConfig topicConfig; ///< 当前主题的容量与策略配置。
        topicConfig.queueCapacity = positiveValue(
            settings.value(QStringLiteral("QueueCapacity")),
            messageBus_.defaultQueueCapacity);
        topicConfig.maxMessageBytes = positiveValue(
            settings.value(QStringLiteral("MaxMessageBytes")),
            messageBus_.maxMessageBytes);
        const QString policy = settings.value(
            QStringLiteral("Policy"), defaultPolicy).toString(); ///< 当前主题的策略文本。
        settings.endGroup();
        if (!parseQueuePolicy(policy, &topicConfig.policy)) { ///< 主题策略文本无效：整体加载失败。
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
    const QStringList topLevelGroups = settings.childGroups(); ///< 顶层分组快照，用于判断 Layout.n 是否存在。
    for (int index = 1; ; ++index) { ///< index：从 1 递增的布局编号。
        const QString groupName = QStringLiteral("Layout.%1").arg(index); ///< 当前布局分组名。
        if (!topLevelGroups.contains(groupName))
            break;

        settings.beginGroup(groupName);
        LayoutPresetConfig preset; ///< 当前布局预设配置。
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
/// @brief 返回实际加载的 INI 文件绝对路径。
/// @return 加载时的配置文件绝对路径；未加载时为空字符串。
QString FrameworkConfig::iniFilePath() const { return iniFilePath_; }
// configDirectory 是解析其他相对路径的基准目录。
/// @brief 返回解析相对路径时使用的配置目录。
/// @return 配置文件所在目录的绝对路径；未加载时为空字符串。
QString FrameworkConfig::configDirectory() const { return configDirectory_; }
// modules 保留 [Modules]/Names 中声明的顺序。
/// @brief 返回按配置声明顺序排列的模块列表副本。
/// @return 模块配置副本列表；未加载时为空。
QVector<ModuleConfig> FrameworkConfig::modules() const { return modules_; }
// 三类配置 getter 返回各自完整结构副本。
/// @brief 返回消息总线配置副本。
/// @return 全局默认容量、载荷上限、默认策略与各主题覆盖项。
MessageBusConfig FrameworkConfig::messageBus() const { return messageBus_; }
/// @brief 返回子进程监督配置副本。
/// @return 注册、心跳、停止与重启相关的时限参数。
ProcessConfig FrameworkConfig::process() const { return process_; }
/// @brief 返回布局预设配置副本。
/// @return 按编号排列的布局预设列表。
LayoutConfig FrameworkConfig::layout() const { return layout_; }
/// @brief 返回全局样式配置副本。
/// @return 包含 QSS 文件路径的样式配置。
StyleConfig FrameworkConfig::style() const { return style_; }
/// @brief 返回日志配置副本。
/// @return 日志目录、单文件上限与刷新周期。
LoggingConfig FrameworkConfig::logging() const { return logging_; }

// 统一把配置路径转换成干净路径；相对值永远以 INI 目录而非进程 cwd 为基准。
/// @brief 把配置中的路径规范化为绝对干净路径，相对值以 INI 目录为基准。
/// @param path 配置中读出的原始路径，允许为空。
/// @return 规范化后的绝对路径；输入为空时返回空字符串。
QString FrameworkConfig::resolvePath(const QString& path) const
{
    // 空路径代表“未配置可选文件”，不是当前目录。
    if (path.trimmed().isEmpty())
        return QString();
    const QFileInfo fileInfo(path); ///< 待判断相对/绝对并规范化的路径信息。
    if (fileInfo.isAbsolute())
        return QDir::cleanPath(fileInfo.absoluteFilePath());
    return QDir::cleanPath(QDir(configDirectory_).absoluteFilePath(path));
}

// 把 INI 中的稳定模块类型文本写入 type；输出指针为空或文本未知时失败。
/// @brief 把配置文本解析为 ModuleType 枚举，大小写敏感。
/// @param value INI 中的类型文本。
/// @param type 解析结果的输出参数，可为空表示仅校验。
/// @return 文本匹配已知类型且输出指针有效时返回 true，否则返回 false。
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
/// @brief 把配置文本解析为 QueuePolicy 枚举，允许大小写差异。
/// @param value INI 中的策略文本。
/// @param policy 解析结果的输出参数，可为空表示仅校验。
/// @return 文本为 Reliable 或 Latest 且输出指针有效时返回 true，否则返回 false。
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
