#pragma once

// 文件职责：声明主应用的总生命周期协调器。
// 它按固定顺序创建配置、日志、消息总线、插件、子进程监督器和主窗口，
// 并在关闭时按依赖关系的反方向释放它们。

#include <QString>
#include <QStringList>

#include "../Config/FrameworkConfig.h"
#include "QFrameworkGlobal.h"

class QApplication;

namespace qframework
{
class MainWindow;
class MessageBus;
class PluginManager;
class ProcessSupervisor;
class StyleManager;

// 应用已经处于退出边界且主进程 DLL 回调仍未结束时，集中执行进程级 fail-fast。
class QFRAMEWORK_EXPORT ProcessFailFast
{
public:
    using Handler = void (*)(const QString& reason); ///< fail-fast 处理函数签名；reason 是可直接展示的中文终止原因。

    /// @brief 应用已越过退出边界却仍有模块消息回调未结束时触发进程级 fail-fast，避免卸载仍被回调借用的 DLL。
    /// @param moduleIds 关闭时仍未结束回调的模块 ID 列表，允许为空或含重复项；内部去重后拼进终止原因。
    /// @return 去重后为空（没有危险模块）返回 false；否则调用处理函数后返回 true——默认走 qFatal，进程不会真的返回。
    static bool requestForHungModules(const QStringList& moduleIds);
    /// @brief 替换 fail-fast 处理函数，仅供 Qt Test 注入可控的终止行为，避免测试进程真的被杀掉。
    /// @param handler 新的处理函数；传 nullptr 表示恢复默认的 qFatal 处理。
    /// @return 无。
    static void setHandlerForTests(Handler handler);
};

class QFRAMEWORK_EXPORT FrameworkRuntime
{
public:
    /// @brief 只记住 QApplication 借用指针：各组件均为空、配置未加载，真正的资源要等 initialize 才创建。
    /// @param application main 创建的 QApplication，生命周期必须长于本对象；本类只借用不删除，为空时 initialize 直接失败。
    /// @return 无（构造函数不返回值）。
    explicit FrameworkRuntime(QApplication* application);
    /// @brief 析构时补做一次幂等的 shutdown，保证 initialize 失败或异常早退也能停线程、释放组件。
    /// @return 无（析构函数不返回值）。
    ~FrameworkRuntime();

    /// @brief 按「配置 → Logger → MessageBus → 插件/子进程 → 主窗口」的顺序装配运行时；模块级故障降级为启动警告，基础设施失败立即返回。
    /// @param configFilePath INI 配置文件路径，须是可读的绝对路径；加载失败直接返回 false。
    /// @param errorMessage 可选的失败原因输出参数，失败时写入中文说明；允许传 nullptr。
    /// @return 配置与日志等基础设施就绪且装配完成返回 true；重复初始化、application 为空、配置或日志启动失败返回 false。
    bool initialize(const QString& configFilePath,
                    QString* errorMessage = nullptr);
    /// @brief 显示主窗口，并把初始化期间收集的非致命警告集中弹窗提示一次。
    /// @return 无；未完成初始化或主窗口为空时直接返回，不显示半初始化的窗口。
    void show();
    /// @brief 按依赖逆序停止消息投递、插件与子进程、冲刷并停掉日志，然后释放全部运行时组件。
    /// @return 无；重复调用由 shutdownComplete_ 门禁拦下，不会二次停机或重复删除对象。
    void shutdown();

    /// @brief 返回运行时持有的主窗口裸指针，供入口和自动化测试使用；调用方只能借用，不能 delete。
    /// @return initialize 成功后的主窗口；初始化之前或 shutdown 之后为 nullptr。
    MainWindow* mainWindow() const;

private:
    /// @brief 过滤空串、跳过重复项，把新的启动警告追加进待展示列表，同时按 Error 级写入集中日志。
    /// @param warnings 本次产生的启动警告文本，允许为空列表或含空字符串；已记录过的条目会被忽略。
    /// @return 无；没有新警告时不做任何事。
    void appendStartupWarnings(const QStringList& warnings);
    /// @brief 在主窗口可见时弹出尚未展示过的启动警告，并推进计数，使同一条警告只提示一次。
    /// @return 无；主窗口为空、尚未显示或没有新警告时静默返回。
    void showPendingStartupWarnings();

    QApplication* application_; ///< main 传入的 QApplication 借用指针，本类不拥有也不 delete；为空时 initialize 直接失败。
    FrameworkConfig config_; ///< 配置文件解析结果的副本，initialize 开头一次性载入，决定日志、总线、模块和布局的初始参数。
    MessageBus* messageBus_; ///< 全局消息总线，initialize 中创建；shutdown 时若仍有隔离模块则只置空、故意不 delete。
    PluginManager* pluginManager_; ///< 主进程内插件管理器，initialize 中创建；shutdown 时在插件停止并判定隔离之后、消息总线之前 delete。
    ProcessSupervisor* processSupervisor_; ///< 子进程模块监督器，initialize 中创建；shutdown 时先停子进程，再于插件管理器之前 delete。
    StyleManager* styleManager_; ///< 全局 QSS 样式管理器，initialize 中创建；MainWindow 只借用，由本类在监督器和插件管理器之后 delete。
    MainWindow* mainWindow_; ///< 主窗口，initialize 中创建并挂接进程内 UI 模块；shutdown 时最先 delete 并置空。
    QStringList startupWarnings_; ///< 累积的非致命启动问题文本，去重后逐条写日志，并在 show() 之后集中弹窗提示。
    int shownStartupWarningCount_; ///< startupWarnings_ 中已经弹窗展示过的条数游标，避免同一条警告重复提示。
    bool initialized_; ///< initialize 全部阶段成功后才置 true，shutdown 结束时复位；用于拦住重复初始化和未初始化时的 show()。
    bool loggerStarted_; ///< Logger 已 start 并安装 Qt 消息处理器的标志，保证 shutdown 只冲刷、停止一次日志。
    bool shutdownComplete_; ///< shutdown 的幂等门禁：首次进入即置 true，之后的调用与析构函数直接返回。
};
}
