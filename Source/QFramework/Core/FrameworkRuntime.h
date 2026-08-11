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
    /// @brief 执行 `void` 所定义的类职责。
    /// @return 返回声明类型的结果值。
    using Handler = void (*)(const QString& reason);

    /// @brief 请求 `requestForHungModules` 对应的框架操作。
    /// @param moduleIds 传给该操作的 `moduleIds` 参数；取值应符合声明类型和函数用途。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    static bool requestForHungModules(const QStringList& moduleIds);
    // 仅供 Qt Test 替换真正的进程终止；传 nullptr 恢复默认处理。
    /// @brief 设置 `setHandlerForTests` 对应的框架操作。
    /// @param handler 传给该操作的 `handler` 参数；取值应符合声明类型和函数用途。
    /// @return 无。
    static void setHandlerForTests(Handler handler);
};

class QFRAMEWORK_EXPORT FrameworkRuntime
{
public:
    // QApplication 由 main 创建且必须比 FrameworkRuntime 活得更久。
    /// @brief 创建 FrameworkRuntime 对象并初始化其基础状态。
    /// @param application 子进程的 QCoreApplication 借用指针。
    /// @return 无（构造函数不返回值）。
    explicit FrameworkRuntime(QApplication* application);
    /// @brief 销毁 FrameworkRuntime 对象并释放其拥有的资源。
    /// @return 无（析构函数不返回值）。
    ~FrameworkRuntime();

    // 完成全部启动阶段；局部模块失败会形成启动警告，基础设施失败才返回 false。
    /// @brief 执行 `initialize` 所定义的类职责。
    /// @param configFilePath 传给该操作的 `configFilePath` 参数；取值应符合声明类型和函数用途。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool initialize(const QString& configFilePath,
                    QString* errorMessage = nullptr); ///< `errorMessage` 对应的对象指针；所有权和线程归属见本类文件级说明。
    // 显示主窗口，并一次性展示初始化期间收集的非致命警告。
    /// @brief 显示 `show` 对应的框架操作。
    /// @return 无。
    void show();
    // 幂等关闭：重复调用不会重复停止线程或删除对象。
    /// @brief 关闭 `shutdown` 对应的框架操作。
    /// @return 无。
    void shutdown();

    // 返回运行时拥有的主窗口裸指针，调用方只能借用，不能 delete。
    /// @brief 执行 `mainWindow` 所定义的类职责。
    /// @return 返回借用指针；找不到目标时返回 nullptr。
    MainWindow* mainWindow() const;

private:
    /// @brief 执行 `appendStartupWarnings` 所定义的类职责。
    /// @param warnings 传给该操作的 `warnings` 参数；取值应符合声明类型和函数用途。
    /// @return 无。
    void appendStartupWarnings(const QStringList& warnings);
    /// @brief 显示 `showPendingStartupWarnings` 对应的框架操作。
    /// @return 无。
    void showPendingStartupWarnings();

    // application_ 是借用指针；其余带 * 的组件都由 FrameworkRuntime 创建和删除。
    QApplication* application_; ///< `application_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    FrameworkConfig config_; ///< 当前对象持有的配置副本。
    MessageBus* messageBus_; ///< `messageBus_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    PluginManager* pluginManager_; ///< `pluginManager_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    ProcessSupervisor* processSupervisor_; ///< `processSupervisor_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    StyleManager* styleManager_; ///< `styleManager_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    MainWindow* mainWindow_; ///< `mainWindow_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    // 非致命启动问题同时写日志并在 show() 后集中提示。
    QStringList startupWarnings_; ///< 保存 `startupWarnings` 相关值的 Qt 容器。
    int shownStartupWarningCount_; ///< `shownStartupWarningCount_` 对应的数值配置、计数或时间状态。
    bool initialized_; ///< `initialized_` 对应的布尔状态标志。
    bool loggerStarted_; ///< `loggerStarted_` 对应的布尔状态标志。
    bool shutdownComplete_; ///< `shutdownComplete_` 对应的布尔状态标志。
};
}
