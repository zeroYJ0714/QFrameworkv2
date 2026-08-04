#pragma once

// 文件职责：声明主窗口的 UI 协调层。
// MainWindow 把模块状态映射到菜单、Dock、状态栏和管理对话框，但不实现
// 插件加载、进程监督、布局序列化或 QSS 解析本身。

#include <QHash>
#include <QMainWindow>
#include <QVector>

#include "FrameworkConfig.h"
#include "QFrameworkGlobal.h"

class QAction;
class QLabel;

namespace qframework
{
class LayoutManager;
class ManagedDockWidget;
class ModuleManagerDialog;
class PluginManager;
class ProcessSupervisor;
class ProcessWindowHost;
class StyleManager;

class QFRAMEWORK_EXPORT MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    // 三个管理器指针均为借用；MainWindow 由 FrameworkRuntime 先于它们删除。
    MainWindow(const QVector<ModuleConfig>& modules,
               PluginManager* pluginManager,
               ProcessSupervisor* processSupervisor,
               StyleManager* styleManager,
               QWidget* parent = nullptr);
    ~MainWindow() override;

    // 插件启动后把 QWidget 放进 Dock；关闭前先解除父子关系再卸载 DLL。
    void attachInProcessUiModules();
    void releaseInProcessUiModules();

    // 委托 LayoutManager 恢复布局，并再次隐藏当前不可用模块的 Dock。
    bool loadLayoutFile(const QString& filePath,
                        QString* errorMessage = nullptr,
                        QStringList* unavailableModuleIds = nullptr);
    // 返回借用指针，供测试或上层调用布局 API。
    LayoutManager* layoutManager() const;

private slots:
    // 槽把菜单/对话框/监督器信号转换成具体 UI 状态变化。
    void showModuleManager();
    void onModuleActionTriggered();
    void onDockVisibilityChanged(bool visible);
    void onShowModuleRequested(const QString& moduleId);
    void onRestartModuleRequested(const QString& moduleId);
    void onModuleStateChanged(const QString& moduleId,
                              const QString& state,
                              const QString& detail);
    void onModuleFault(const QString& moduleId, const QString& detail);
    void onWindowHandleReady(const QString& moduleId, quintptr windowId);
    void loadLayoutFromDialog();
    void saveCurrentLayout();
    void saveLayoutAs();
    void selectStyleSheet();
    void reloadStyleSheet();

private:
    // 构建固定菜单和模块 Dock；运行期间只更新可见性和状态。
    void createActions();
    void createModuleDocks();
    void setUiAvailable(const QString& moduleId, bool available);
    void showModule(const QString& moduleId);
    void updateStatusSummary();
    void reportStateFailure(const QString& title, const QString& detail);
    QString displayName(const QString& moduleId) const;
    QString placeholderText(const QString& moduleId, const QString& detail) const;

    // modules_ 保持配置顺序；modulesById_ 用于按 ID 快速查显示名称。
    QVector<ModuleConfig> modules_;
    QHash<QString, ModuleConfig> modulesById_;
    // 三个管理器为借用指针；layout/dialog/Dock/控件由本窗口拥有。
    PluginManager* pluginManager_;
    ProcessSupervisor* processSupervisor_;
    StyleManager* styleManager_;
    LayoutManager* layoutManager_;
    ModuleManagerDialog* moduleManagerDialog_;
    // 下列映射都以稳定 moduleId 为键，指针所有权仍由 Qt 父子关系管理。
    QHash<QString, ManagedDockWidget*> moduleDocks_;
    QHash<QString, ProcessWindowHost*> processHosts_;
    QHash<QString, QAction*> moduleActions_;
    QHash<QString, bool> uiAvailable_;
    QHash<QString, QString> moduleStates_;
    QLabel* statusSummaryLabel_;
    QAction* saveLayoutAction_;
};
}
