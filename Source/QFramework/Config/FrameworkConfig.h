#pragma once

// 文件职责：把只读 QFramework.ini 转成有类型的内存配置。
// 其他模块只读取这些结构，不直接操作 QSettings，也不会把配置写回磁盘。

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include "QFrameworkGlobal.h"

namespace qframework
{
enum class ModuleType
{
    // DLL + QWidget，加载到主进程并嵌入 Dock。
    InProcessUi,
    // DLL + QObject，加载到主进程但没有界面。
    InProcessNonUi,
    // EXE + QWidget，由监督器启动并嵌入原生窗口。
    ProcessUi,
    // EXE + QObject，由监督器启动且没有界面。
    ProcessNonUi
};

enum class QueuePolicy
{
    // 可靠消息：容量满时拒绝新消息，绝不替换已经排队的旧消息。
    Reliable,
    // 帧流消息：容量满时只丢弃同主题最旧的等待帧，保留最新值。
    Latest
};

struct ModuleConfig
{
    // id 是全局唯一键，同时用于插件文件名、日志来源和消息注册。
    QString id; ///< 模块唯一标识，同时用作插件文件名、日志来源和消息注册键。
    bool enabled = true; ///< 是否启用该模块，false 时跳过加载。
    ModuleType type = ModuleType::InProcessNonUi; ///< 模块形态，决定进程内/独立进程以及是否带界面。
    // displayName 只影响 UI；filePath 是以 INI 目录解析后的绝对/规范路径。
    QString displayName; ///< 界面显示名称。
    QString filePath; ///< 模块 DLL 或 EXE 的路径，已规范化为绝对路径。
    // 调试等待只对 Debug 子进程模块生效，并始终受超时限制。
    bool waitForDebugger = false; ///< 是否等待调试器附加后再启动子进程。
    int debuggerWaitTimeoutMs = 30000; ///< 等待调试器附加的超时时间，单位毫秒。
};

struct TopicConfig
{
    // 这是“每个订阅者、每个主题”的等待/在途上限，不是全局总条数。
    int queueCapacity = 0; ///< 该主题的队列容量；0 表示沿用默认值。
    // 单条 QByteArray 的上限；超过后在进入队列前拒绝。
    int maxMessageBytes = 0; ///< 该主题的单条消息字节上限；0 表示沿用默认值。
    QueuePolicy policy = QueuePolicy::Reliable; ///< 该主题队列满时的处理策略。
};

struct MessageBusConfig
{
    // 没有 [Topic.<name>] 专门配置时使用这些默认值。
    int defaultQueueCapacity = 256; ///< 未单独配置的主题默认队列容量。
    // 全局单条消息上限；主题可以用更小或更大的正值覆盖。
    int maxMessageBytes = 16 * 1024 * 1024; ///< 全局单条消息字节上限。
    // 大于等于此字节数时，跨进程传输改用 QSharedMemory。
    int sharedMemoryThresholdBytes = 1024 * 1024; ///< 改用共享内存传输的字节阈值。
    // 关闭时等待队列排空的总时间预算。
    int shutdownDrainTimeoutMs = 3000; ///< 排空队列的时间预算，单位毫秒。
    QueuePolicy defaultPolicy = QueuePolicy::Reliable; ///< 未单独配置的主题默认队列策略。
    // 主题名 -> 专用规则；不存在时使用上面的默认值。
    QHash<QString, TopicConfig> topics; ///< 主题名到专用队列规则的映射。
};

struct ProcessConfig
{
    // 子进程必须在该时间内连接、注册并启动。
    int registrationTimeoutMs = 10000; ///< 子进程连接注册并启动完成的超时时间，单位毫秒。
    // 监督器发送 ping 的周期及判定 pong 超时的上限。
    int heartbeatIntervalMs = 1000; ///< 心跳 ping 的发送周期，单位毫秒。
    int heartbeatTimeoutMs = 5000; ///< 判定心跳 pong 超时的时间上限，单位毫秒。
    // 正常 stopAck 的等待上限。
    int stopTimeoutMs = 5000; ///< 等待正常 stopAck 的时间上限，单位毫秒。
    // 自动重启的延迟、计数窗口和窗口内最大次数。
    int restartDelayMs = 1000; ///< 子进程退出后自动重启的延迟，单位毫秒。
    int restartWindowMs = 60000; ///< 自动重启计数的统计窗口，单位毫秒。
    int maxRestartCount = 3; ///< 统计窗口内允许的最大重启次数。
};

struct LayoutPresetConfig
{
    // index 对应 [Layout.n] 中的 n；filePath 已经由 resolvePath 规范化。
    int index = 0; ///< 布局序号，对应 [Layout.n] 中的 n。
    QString name; ///< 布局预设名称。
    QString filePath; ///< 布局文件路径，已规范化为绝对路径。
};

struct LayoutConfig
{
    // 保留 INI 中从 Layout.1 开始的连续顺序，运行期间不重新读取。
    QVector<LayoutPresetConfig> presets; ///< 按 INI 中 Layout.1 起的连续顺序保存的布局预设。
};

struct StyleConfig
{
    // 主进程和子进程 UI 共用的 QSS 文件。
    QString file; ///< 主进程与子进程 UI 共用的 QSS 文件路径。
};

struct LoggingConfig
{
    // 日志目录相对 INI 目录解析，运行时会按需创建。
    QString directory; ///< 日志目录，相对 INI 目录解析。
    // 单个日志文件的滚动阈值。
    qint64 maxFileBytes = 10 * 1024 * 1024; ///< 单个日志文件的滚动阈值，单位字节。
    // 普通日志的批量刷新周期；显式 flush、Fatal、滚动和停止不受此值延迟。
    int flushIntervalMs = 100; ///< 普通日志的批量刷新周期，单位毫秒。
};

class QFRAMEWORK_EXPORT FrameworkConfig
{
public:
    // 完整解析并校验 INI。失败返回 false，并尽量写出可读错误。
    /// @brief 解析并校验 INI 配置文件，成功后保存只读内存快照。
    /// @param iniFilePath INI 配置文件的路径。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 解析成功返回 true，否则返回 false。
    bool load(const QString& iniFilePath, QString* errorMessage = nullptr);

    /// @brief 返回按配置声明顺序排列的模块列表副本。
    /// @return 模块配置副本列表；未加载时为空。
    QVector<ModuleConfig> modules() const;
    /// @brief 返回消息总线配置副本。
    /// @return 全局默认容量、载荷上限、默认策略与各主题覆盖项。
    MessageBusConfig messageBus() const;
    /// @brief 返回子进程监督配置副本。
    /// @return 注册、心跳、停止与重启相关的时限参数。
    ProcessConfig process() const;
    /// @brief 返回布局预设配置副本。
    /// @return 按编号排列的布局预设列表。
    LayoutConfig layout() const;
    /// @brief 返回全局样式配置副本。
    /// @return 包含 QSS 文件路径的样式配置。
    StyleConfig style() const;
    /// @brief 返回日志配置副本。
    /// @return 日志目录、单文件上限与刷新周期。
    LoggingConfig logging() const;

    // 绝对路径直接规范化；相对路径以 INI 所在目录为基准。
    /// @brief 把配置里的路径统一解析为规范化绝对路径。
    /// @param path 待解析的路径，可为绝对路径或相对路径。
    /// @return 规范化后的绝对路径；path 为空时返回空字符串。
    QString resolvePath(const QString& path) const;
    // 两个解析器把 INI 文本转换为强类型枚举，无法识别时返回 false。
    /// @brief 把 INI 文本解析为模块类型枚举。
    /// @param value INI 中记录的模块类型文本。
    /// @param type 用于接收解析结果的输出指针。
    /// @return 解析成功返回 true，无法识别返回 false。
    static bool parseModuleType(const QString& value, ModuleType* type);
    /// @brief 把 INI 文本解析为队列策略枚举。
    /// @param value INI 中记录的队列策略文本。
    /// @param policy 用于接收解析结果的输出指针。
    /// @return 解析成功返回 true，无法识别返回 false。
    static bool parseQueuePolicy(const QString& value, QueuePolicy* policy);

private:
    // load 成功后保存的只读内存快照。
    QString configDirectory_; ///< INI 文件所在目录。
    QVector<ModuleConfig> modules_; ///< 解析得到的模块配置列表。
    MessageBusConfig messageBus_; ///< 解析得到的消息总线配置。
    ProcessConfig process_; ///< 解析得到的进程监督配置。
    LayoutConfig layout_; ///< 解析得到的布局配置。
    StyleConfig style_; ///< 解析得到的样式配置。
    LoggingConfig logging_; ///< 解析得到的日志配置。
};
}
