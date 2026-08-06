#pragma once

// 文件职责：声明主窗口的 UI 协调层。
// MainWindow 把模块状态映射到菜单、Dock、状态栏和管理对话框，但不实现
// 插件加载、进程监督、布局序列化或 QSS 解析本身。

#include <QColor>
#include <QHash>
#include <QMainWindow>
#include <QPoint>
#include <QSize>
#include <QVector>

#include "FrameworkConfig.h"
#include "QFrameworkGlobal.h"

class QAction;
class QActionGroup;
class QByteArray;
class QEvent;
class QLabel;
class QMenu;

namespace qframework
{
class LayoutManager;
class ManagedDockWidget;
class ModuleManagerDialog;
class PluginManager;
class ProcessSupervisor;
class ProcessWindowHost;
class StyleManager;
class WindowTitleBar;

class QFRAMEWORK_EXPORT MainWindow : public QMainWindow
{
    Q_OBJECT
    // QSS 只负责给出颜色，不再直接绘制 separator 背景。实际绘制由 Qt 样式类完成，
    // 因而可以把 5px 鼠标命中区和中央 1px 可见线分开处理。
    Q_PROPERTY(QColor dockSeparatorColor
               READ dockSeparatorColor
               WRITE setDockSeparatorColor)
    Q_PROPERTY(QColor dockSeparatorHoverColor
               READ dockSeparatorHoverColor
               WRITE setDockSeparatorHoverColor)

public:
    // 三个管理器指针均为借用；MainWindow 由 FrameworkRuntime 先于它们删除。
    MainWindow(const QVector<ModuleConfig>& modules,
               PluginManager* pluginManager,
               ProcessSupervisor* processSupervisor,
               StyleManager* styleManager,
               QWidget* parent = nullptr);
    ~MainWindow() override;

    // 这两个属性主要供 QSS 的 qproperty-* 使用；未加载 TechDashboard 时使用
    // 当前 Qt 调色板中的 Mid/Highlight 颜色，分隔线仍然自然可见。
    QColor dockSeparatorColor() const;
    void setDockSeparatorColor(const QColor& color);
    QColor dockSeparatorHoverColor() const;
    void setDockSeparatorHoverColor(const QColor& color);

    // 插件启动后把 QWidget 放进 Dock；关闭前先解除父子关系再卸载 DLL。
    void attachInProcessUiModules();
    void releaseInProcessUiModules();

    // 由 FrameworkRuntime 在构造完成后注入一次只读布局预设快照。
    void setLayoutPresets(const QVector<LayoutPresetConfig>& presets);
    // 启动时只尝试 Layout.1；失败不会弹框，也不会建立活动布局。
    bool loadInitialLayoutPreset();

    // 委托 LayoutManager 恢复布局，并再次隐藏当前不可用模块的 Dock。
    bool loadLayoutFile(const QString& filePath,
                        QString* errorMessage = nullptr,
                        QStringList* unavailableModuleIds = nullptr,
                        bool activateLayout = true);
    // 返回借用指针，供测试或上层调用布局 API。
    LayoutManager* layoutManager() const;

private slots:
    // 槽把菜单/对话框/监督器信号转换成具体 UI 状态变化。
    void showModuleManager();
    void onModuleActionTriggered(bool checked);
    void onDockCloseRequested();
    void onShowModuleRequested(const QString& moduleId);
    void onRestartModuleRequested(const QString& moduleId);
    void onRestartFinished(const QString& moduleId,
                           bool success,
                           const QString& detail);
    void onModuleStateChanged(const QString& moduleId,
                              const QString& state,
                              const QString& detail);
    void onModuleFault(const QString& moduleId, const QString& detail);
    void onWindowHandleReady(const QString& moduleId, quintptr windowId);
    void onProcessWindowSizeChanged(const QString& moduleId, const QSize& size);
    void onLayoutPresetTriggered(bool checked);
    void loadLayoutFromDialog();
    void saveCurrentLayout();
    void saveLayoutAs();
    void selectStyleSheet();
    void reloadStyleSheet();
    void setUiAvailable(const QString& moduleId, bool available);
    // 标题栏空白区请求由 Qt 原生窗口管理器开始移动；最大化时先恢复正常尺寸。
    void startWindowMove(const QPoint& globalPosition,
                         const QPoint& titleBarPosition);
    // 标题栏按钮请求都回到 MainWindow，由这里统一改变顶层窗口状态。
    void minimizeWindow();
    void toggleMaximizedState();
    void closeWindow();

protected:
    // WindowStateChange 到达后同步标题栏的最大化/还原图标。
    void changeEvent(QEvent* event) override;
    // Windows 无边框窗口的命中测试；非 Windows 平台走 QWidget 默认实现。
    bool nativeEvent(const QByteArray& eventType,
                     void* message,
                     long* result) override;

private:
    enum class VisibilityOrigin
    {
        UserAction,
        LayoutRestore,
        WindowReady,
        RuntimeState,
        CloseButton
    };

    // 构建固定菜单和模块 Dock；运行期间只更新可见性和状态。
    void createActions();
    void createModuleDocks();
    void setRequestedDockVisible(const QString& moduleId,
                                 bool visible,
                                 VisibilityOrigin origin);
    void applyRequestedDockVisibility(const QString& moduleId,
                                      VisibilityOrigin origin);
    void syncModuleAction(const QString& moduleId);
    void updateStatusSummary();
    void reportStateFailure(const QString& title, const QString& detail);
    bool activateLayoutPreset(int index, bool startup, QString* errorMessage = nullptr);
    const LayoutPresetConfig* layoutPreset(int index) const;
    void clearLayoutPresetActions();
    QString displayName(const QString& moduleId) const;
    QString placeholderText(const QString& moduleId, const QString& detail) const;

    // modules_ 保持配置顺序；modulesById_ 用于按 ID 快速查显示名称。
    QVector<ModuleConfig> modules_;
    QHash<QString, ModuleConfig> modulesById_;
    // 三个管理器为借用指针；layout/dialog/Dock/控件由本窗口拥有。
    PluginManager* pluginManager_;
    ProcessSupervisor* processSupervisor_;
    StyleManager* styleManager_;
    // 由 MainWindow 持有的客户区标题栏；setMenuWidget() 只改变它在 QMainWindow 中的位置。
    WindowTitleBar* titleBar_;
    LayoutManager* layoutManager_;
    ModuleManagerDialog* moduleManagerDialog_;
    // 下列映射都以稳定 moduleId 为键，指针所有权仍由 Qt 父子关系管理。
    QHash<QString, ManagedDockWidget*> moduleDocks_;
    QHash<QString, ProcessWindowHost*> processHosts_;
    QHash<QString, QAction*> moduleActions_;
    // 用户显示意图独立于 Qt 当前标签是否正在绘制，也独立于模块是否 ready。
    QHash<QString, bool> requestedDockVisibility_;
    QHash<QString, bool> uiAvailable_;
    QHash<QString, QString> moduleStates_;
    QLabel* statusSummaryLabel_;
    QAction* saveLayoutAction_;
    QMenu* layoutMenu_;
    QAction* layoutPresetSeparator_;
    QActionGroup* layoutPresetGroup_;
    QHash<int, QAction*> layoutPresetActions_;
    QVector<LayoutPresetConfig> layoutPresets_;
    int activeLayoutIndex_;
    QColor dockSeparatorColor_;
    QColor dockSeparatorHoverColor_;
};
}
