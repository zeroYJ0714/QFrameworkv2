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
    using Handler = void (*)(const QString& reason);

    static bool requestForHungModules(const QStringList& moduleIds);
    // 仅供 Qt Test 替换真正的进程终止；传 nullptr 恢复默认处理。
    static void setHandlerForTests(Handler handler);
};

class QFRAMEWORK_EXPORT FrameworkRuntime
{
public:
    // QApplication 由 main 创建且必须比 FrameworkRuntime 活得更久。
    explicit FrameworkRuntime(QApplication* application);
    ~FrameworkRuntime();

    // 完成全部启动阶段；局部模块失败会形成启动警告，基础设施失败才返回 false。
    bool initialize(const QString& configFilePath,
                    QString* errorMessage = nullptr);
    // 显示主窗口，并一次性展示初始化期间收集的非致命警告。
    void show();
    // 幂等关闭：重复调用不会重复停止线程或删除对象。
    void shutdown();

    // 返回运行时拥有的主窗口裸指针，调用方只能借用，不能 delete。
    MainWindow* mainWindow() const;

private:
    void appendStartupWarnings(const QStringList& warnings);
    void showPendingStartupWarnings();

    // application_ 是借用指针；其余带 * 的组件都由 FrameworkRuntime 创建和删除。
    QApplication* application_;
    FrameworkConfig config_;
    MessageBus* messageBus_;
    PluginManager* pluginManager_;
    ProcessSupervisor* processSupervisor_;
    StyleManager* styleManager_;
    MainWindow* mainWindow_;
    // 非致命启动问题同时写日志并在 show() 后集中提示。
    QStringList startupWarnings_;
    int shownStartupWarningCount_;
    bool initialized_;
    bool loggerStarted_;
    bool shutdownComplete_;
};
}
