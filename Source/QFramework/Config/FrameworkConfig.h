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
    QString id;
    bool enabled = true;
    ModuleType type = ModuleType::InProcessNonUi;
    // displayName 只影响 UI；filePath 是以 INI 目录解析后的绝对/规范路径。
    QString displayName;
    QString filePath;
    // 调试等待只对 Debug 子进程模块生效，并始终受超时限制。
    bool waitForDebugger = false;
    int debuggerWaitTimeoutMs = 30000;
};

struct TopicConfig
{
    // 这是“每个订阅者、每个主题”的等待/在途上限，不是全局总条数。
    int queueCapacity = 0;
    // 单条 QByteArray 的上限；超过后在进入队列前拒绝。
    int maxMessageBytes = 0;
    QueuePolicy policy = QueuePolicy::Reliable;
};

struct MessageBusConfig
{
    // 没有 [Topic.<name>] 专门配置时使用这些默认值。
    int defaultQueueCapacity = 256;
    // 全局单条消息上限；主题可以用更小或更大的正值覆盖。
    int maxMessageBytes = 16 * 1024 * 1024;
    // 大于等于此字节数时，跨进程传输改用 QSharedMemory。
    int sharedMemoryThresholdBytes = 1024 * 1024;
    // 关闭时等待队列排空的总时间预算。
    int shutdownDrainTimeoutMs = 3000;
    QueuePolicy defaultPolicy = QueuePolicy::Reliable;
    // 主题名 -> 专用规则；不存在时使用上面的默认值。
    QHash<QString, TopicConfig> topics;
};

struct ProcessConfig
{
    // 子进程必须在该时间内连接、注册并启动。
    int registrationTimeoutMs = 10000;
    // 监督器发送 ping 的周期及判定 pong 超时的上限。
    int heartbeatIntervalMs = 1000;
    int heartbeatTimeoutMs = 5000;
    // 正常 stopAck 的等待上限。
    int stopTimeoutMs = 5000;
    // 自动重启的延迟、计数窗口和窗口内最大次数。
    int restartDelayMs = 1000;
    int restartWindowMs = 60000;
    int maxRestartCount = 3;
};

struct LayoutPresetConfig
{
    // index 对应 [Layout.n] 中的 n；filePath 已经由 resolvePath 规范化。
    int index = 0;
    QString name;
    QString filePath;
};

struct LayoutConfig
{
    // 保留 INI 中从 Layout.1 开始的连续顺序，运行期间不重新读取。
    QVector<LayoutPresetConfig> presets;
};

struct StyleConfig
{
    // 主进程和子进程 UI 共用的 QSS 文件。
    QString file;
};

struct LoggingConfig
{
    // 日志目录相对 INI 目录解析，运行时会按需创建。
    QString directory;
    // 单个日志文件的滚动阈值。
    qint64 maxFileBytes = 10 * 1024 * 1024;
    // 普通日志的批量刷新周期；显式 flush、Fatal、滚动和停止不受此值延迟。
    int flushIntervalMs = 100;
};

class QFRAMEWORK_EXPORT FrameworkConfig
{
public:
    // 完整解析并校验 INI。失败返回 false，并尽量写出可读错误。
    bool load(const QString& iniFilePath, QString* errorMessage = nullptr);

    // 以下 getter 返回配置快照，调用方修改副本不会反向改变 FrameworkConfig。
    QString iniFilePath() const;
    QString configDirectory() const;
    QVector<ModuleConfig> modules() const;
    MessageBusConfig messageBus() const;
    ProcessConfig process() const;
    LayoutConfig layout() const;
    StyleConfig style() const;
    LoggingConfig logging() const;

    // 绝对路径直接规范化；相对路径以 INI 所在目录为基准。
    QString resolvePath(const QString& path) const;
    // 两个解析器把 INI 文本转换为强类型枚举，无法识别时返回 false。
    static bool parseModuleType(const QString& value, ModuleType* type);
    static bool parseQueuePolicy(const QString& value, QueuePolicy* policy);

private:
    // load 成功后保存的只读内存快照。
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
