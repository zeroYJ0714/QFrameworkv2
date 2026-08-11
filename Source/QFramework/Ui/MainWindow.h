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
class QCloseEvent;
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
    /// @brief 创建 MainWindow 对象并初始化其基础状态。
    /// @param modules 模块配置列表。
    /// @param pluginManager 传给该操作的 `pluginManager` 参数；取值应符合声明类型和函数用途。
    /// @param processSupervisor 传给该操作的 `processSupervisor` 参数；取值应符合声明类型和函数用途。
    /// @param styleManager 传给该操作的 `styleManager` 参数；取值应符合声明类型和函数用途。
    /// @param parent Qt 父对象或父控件；用于建立所有权关系，允许为空。
    /// @return 无（构造函数不返回值）。
    MainWindow(const QVector<ModuleConfig>& modules,
               PluginManager* pluginManager,
               ProcessSupervisor* processSupervisor,
               StyleManager* styleManager,
               QWidget* parent = nullptr); ///< `parent` 对应的对象指针；所有权和线程归属见本类文件级说明。
    /// @brief 销毁 MainWindow 对象并释放其拥有的资源。
    /// @return 无（析构函数不返回值）。
    ~MainWindow() override;

    // 这两个属性主要供 QSS 的 qproperty-* 使用；未加载 TechDashboard 时使用
    // 当前 Qt 调色板中的 Mid/Highlight 颜色，分隔线仍然自然可见。
    /// @brief 执行 `dockSeparatorColor` 所定义的类职责。
    /// @return 返回对应 Qt 值或容器的安全副本。
    QColor dockSeparatorColor() const;
    /// @brief 设置 `setDockSeparatorColor` 对应的框架操作。
    /// @param color 目标颜色值。
    /// @return 无。
    void setDockSeparatorColor(const QColor& color);
    /// @brief 执行 `dockSeparatorHoverColor` 所定义的类职责。
    /// @return 返回对应 Qt 值或容器的安全副本。
    QColor dockSeparatorHoverColor() const;
    /// @brief 设置 `setDockSeparatorHoverColor` 对应的框架操作。
    /// @param color 目标颜色值。
    /// @return 无。
    void setDockSeparatorHoverColor(const QColor& color);

    // 插件启动后把 QWidget 放进 Dock；关闭前先解除父子关系再卸载 DLL。
    /// @brief 挂接 `attachInProcessUiModules` 对应的框架操作。
    /// @return 无。
    void attachInProcessUiModules();
    /// @brief 释放 `releaseInProcessUiModules` 对应的框架操作。
    /// @return 无。
    void releaseInProcessUiModules();

    // 由 FrameworkRuntime 在构造完成后注入一次只读布局预设快照。
    /// @brief 设置 `setLayoutPresets` 对应的框架操作。
    /// @param presets 布局预设列表。
    /// @return 无。
    void setLayoutPresets(const QVector<LayoutPresetConfig>& presets);
    // 启动时只尝试 Layout.1；失败不会弹框，也不会建立活动布局。
    /// @brief 加载 `loadInitialLayoutPreset` 对应的框架操作。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool loadInitialLayoutPreset();

    // 委托 LayoutManager 恢复布局，并再次隐藏当前不可用模块的 Dock。
    /// @brief 加载 `loadLayoutFile` 对应的框架操作。
    /// @param filePath 文件路径。
    /// @param errorMessage 可选错误说明输出参数。
    /// @param unavailableModuleIds 传给该操作的 `unavailableModuleIds` 参数；取值应符合声明类型和函数用途。
    /// @param activateLayout 传给该操作的 `activateLayout` 参数；取值应符合声明类型和函数用途。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool loadLayoutFile(const QString& filePath,
                        QString* errorMessage = nullptr,
                        QStringList* unavailableModuleIds = nullptr,
                        bool activateLayout = true); ///< `activateLayout` 对应的布尔状态标志。
    // 返回借用指针，供测试或上层调用布局 API。
    /// @brief 执行 `layoutManager` 所定义的类职责。
    /// @return 返回借用指针；找不到目标时返回 nullptr。
    LayoutManager* layoutManager() const;

private slots:
    // 槽把菜单/对话框/监督器信号转换成具体 UI 状态变化。
    /// @brief 显示 `showModuleManager` 对应的框架操作。
    /// @return 无。
    void showModuleManager();
    /// @brief 处理 `onModuleActionTriggered` 对应的框架操作。
    /// @param checked Qt 动作当前是否选中。
    /// @return 无。
    void onModuleActionTriggered(bool checked);
    /// @brief 处理 `onDockCloseRequested` 对应的框架操作。
    /// @return 无。
    void onDockCloseRequested();
    /// @brief 处理 `onShowModuleRequested` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @return 无。
    void onShowModuleRequested(const QString& moduleId);
    /// @brief 处理 `onRestartModuleRequested` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @return 无。
    void onRestartModuleRequested(const QString& moduleId);
    /// @brief 处理 `onRestartFinished` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param success 传给该操作的 `success` 参数；取值应符合声明类型和函数用途。
    /// @param detail 状态或故障详情。
    /// @return 无。
    void onRestartFinished(const QString& moduleId,
                           bool success,
                           const QString& detail);
    /// @brief 处理 `onModuleStateChanged` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param state 状态值。
    /// @param detail 状态或故障详情。
    /// @return 无。
    void onModuleStateChanged(const QString& moduleId,
                              const QString& state,
                              const QString& detail);
    /// @brief 处理 `onModuleFault` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param detail 状态或故障详情。
    /// @return 无。
    void onModuleFault(const QString& moduleId, const QString& detail);
    /// @brief 处理 `onWindowHandleReady` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param windowId 传给该操作的 `windowId` 参数；取值应符合声明类型和函数用途。
    /// @return 无。
    void onWindowHandleReady(const QString& moduleId, quintptr windowId);
    /// @brief 处理 `onProcessWindowSizeChanged` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param size 传给该操作的 `size` 参数；取值应符合声明类型和函数用途。
    /// @return 无。
    void onProcessWindowSizeChanged(const QString& moduleId, const QSize& size);
    /// @brief 处理 `onLayoutPresetTriggered` 对应的框架操作。
    /// @param checked Qt 动作当前是否选中。
    /// @return 无。
    void onLayoutPresetTriggered(bool checked);
    /// @brief 加载 `loadLayoutFromDialog` 对应的框架操作。
    /// @return 无。
    void loadLayoutFromDialog();
    /// @brief 保存 `saveCurrentLayout` 对应的框架操作。
    /// @return 无。
    void saveCurrentLayout();
    /// @brief 保存 `saveLayoutAs` 对应的框架操作。
    /// @return 无。
    void saveLayoutAs();
    /// @brief 执行 `selectStyleSheet` 所定义的类职责。
    /// @return 无。
    void selectStyleSheet();
    /// @brief 执行 `reloadStyleSheet` 所定义的类职责。
    /// @return 无。
    void reloadStyleSheet();
    /// @brief 设置 `setUiAvailable` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param available 目标可用状态。
    /// @return 无。
    void setUiAvailable(const QString& moduleId, bool available);
    // 标题栏空白区请求由 Qt 原生窗口管理器开始移动；最大化时先恢复正常尺寸。
    /// @brief 启动 `startWindowMove` 对应的框架操作。
    /// @param globalPosition 鼠标全局坐标。
    /// @param titleBarPosition 鼠标在标题栏内的坐标。
    /// @return 无。
    void startWindowMove(const QPoint& globalPosition,
                         const QPoint& titleBarPosition);
    // 标题栏按钮请求都回到 MainWindow，由这里统一改变顶层窗口状态。
    /// @brief 执行 `minimizeWindow` 所定义的类职责。
    /// @return 无。
    void minimizeWindow();
    /// @brief 执行 `toggleMaximizedState` 所定义的类职责。
    /// @return 无。
    void toggleMaximizedState();
    /// @brief 关闭 `closeWindow` 对应的框架操作。
    /// @return 无。
    void closeWindow();

protected:
    // 应用退出前按配置顺序询问已加载的主进程 UI 模块；任一拒绝都保持窗口。
    /// @brief 关闭 `closeEvent` 对应的框架操作。
    /// @param event Qt 事件对象；仅在当前回调期间有效。
    /// @return 无。
    void closeEvent(QCloseEvent* event) override;
    // WindowStateChange 到达后同步标题栏的最大化/还原图标。
    /// @brief 执行 `changeEvent` 所定义的类职责。
    /// @param event Qt 事件对象；仅在当前回调期间有效。
    /// @return 无。
    void changeEvent(QEvent* event) override;
    // Windows 无边框窗口的命中测试；非 Windows 平台走 QWidget 默认实现。
    /// @brief 执行 `nativeEvent` 所定义的类职责。
    /// @param eventType 原生事件类型字节串。
    /// @param message 平台原生消息指针。
    /// @param result 用于接收处理结果的输出指针或结果值。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool nativeEvent(const QByteArray& eventType,
                     void* message,
                     long* result) override; ///< `override` 对应的对象指针；所有权和线程归属见本类文件级说明。

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
    /// @brief 创建 `createActions` 对应的框架操作。
    /// @return 无。
    void createActions();
    /// @brief 创建 `createModuleDocks` 对应的框架操作。
    /// @return 无。
    void createModuleDocks();
    /// @brief 设置 `setRequestedDockVisible` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param visible 传给该操作的 `visible` 参数；取值应符合声明类型和函数用途。
    /// @param origin 传给该操作的 `origin` 参数；取值应符合声明类型和函数用途。
    /// @return 无。
    void setRequestedDockVisible(const QString& moduleId,
                                 bool visible,
                                 VisibilityOrigin origin);
    /// @brief 应用 `applyRequestedDockVisibility` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @param origin 传给该操作的 `origin` 参数；取值应符合声明类型和函数用途。
    /// @return 无。
    void applyRequestedDockVisibility(const QString& moduleId,
                                      VisibilityOrigin origin);
    /// @brief 检查 `canHideModule` 对应的框架操作。
    /// @param moduleId 稳定模块 ID。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool canHideModule(const QString& moduleId) const;
    /// @brief 执行 `syncModuleAction` 所定义的类职责。
    /// @param moduleId 稳定模块 ID。
    /// @return 无。
    void syncModuleAction(const QString& moduleId);
    /// @brief 更新 `updateStatusSummary` 对应的框架操作。
    /// @return 无。
    void updateStatusSummary();
    /// @brief 执行 `reportStateFailure` 所定义的类职责。
    /// @param title 界面显示标题。
    /// @param detail 状态或故障详情。
    /// @return 无。
    void reportStateFailure(const QString& title, const QString& detail);
    /// @brief 执行 `activateLayoutPreset` 所定义的类职责。
    /// @param index 列表或配置索引。
    /// @param startup 是否处于启动阶段。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool activateLayoutPreset(int index, bool startup, QString* errorMessage = nullptr);
    /// @brief 执行 `layoutPreset` 所定义的类职责。
    /// @param index 列表或配置索引。
    /// @return 返回借用指针；找不到目标时返回 nullptr。
    const LayoutPresetConfig* layoutPreset(int index) const;
    /// @brief 清理 `clearLayoutPresetActions` 对应的框架操作。
    /// @return 无。
    void clearLayoutPresetActions();
    /// @brief 执行 `displayName` 所定义的类职责。
    /// @param moduleId 稳定模块 ID。
    /// @return 返回对应 Qt 值或容器的安全副本。
    QString displayName(const QString& moduleId) const;
    /// @brief 执行 `placeholderText` 所定义的类职责。
    /// @param moduleId 稳定模块 ID。
    /// @param detail 状态或故障详情。
    /// @return 返回对应 Qt 值或容器的安全副本。
    QString placeholderText(const QString& moduleId, const QString& detail) const;

    // modules_ 保持配置顺序；modulesById_ 用于按 ID 快速查显示名称。
    QVector<ModuleConfig> modules_; ///< 保存 `modules` 相关值的 Qt 容器。
    QHash<QString, ModuleConfig> modulesById_; ///< 保存 `modulesById` 相关值的 Qt 容器。
    // 三个管理器为借用指针；layout/dialog/Dock/控件由本窗口拥有。
    PluginManager* pluginManager_; ///< `pluginManager_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    ProcessSupervisor* processSupervisor_; ///< `processSupervisor_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    StyleManager* styleManager_; ///< `styleManager_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    // 由 MainWindow 持有的客户区标题栏；setMenuWidget() 只改变它在 QMainWindow 中的位置。
    WindowTitleBar* titleBar_; ///< `titleBar_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    LayoutManager* layoutManager_; ///< `layoutManager_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    ModuleManagerDialog* moduleManagerDialog_; ///< `moduleManagerDialog_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    // 下列映射都以稳定 moduleId 为键，指针所有权仍由 Qt 父子关系管理。
    QHash<QString, ManagedDockWidget*> moduleDocks_; ///< `moduleDocks_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QHash<QString, ProcessWindowHost*> processHosts_; ///< `processHosts_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QHash<QString, QAction*> moduleActions_; ///< `moduleActions_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    // 用户显示意图独立于 Qt 当前标签是否正在绘制，也独立于模块是否 ready。
    QHash<QString, bool> requestedDockVisibility_; ///< `requestedDockVisibility_` 对应的布尔状态标志。
    QHash<QString, bool> uiAvailable_; ///< `uiAvailable_` 对应的布尔状态标志。
    QHash<QString, QString> moduleStates_; ///< 保存 `moduleStates` 相关值的 Qt 容器。
    QLabel* statusSummaryLabel_; ///< `statusSummaryLabel_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QAction* saveLayoutAction_; ///< `saveLayoutAction_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QMenu* layoutMenu_; ///< `layoutMenu_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QAction* layoutPresetSeparator_; ///< `layoutPresetSeparator_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QActionGroup* layoutPresetGroup_; ///< `layoutPresetGroup_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QHash<int, QAction*> layoutPresetActions_; ///< `layoutPresetActions_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QVector<LayoutPresetConfig> layoutPresets_; ///< 保存 `layoutPresets` 相关值的 Qt 容器。
    int activeLayoutIndex_; ///< `activeLayoutIndex_` 对应的数值配置、计数或时间状态。
    QColor dockSeparatorColor_; ///< 保存 `dockSeparatorColor` 对应的对象状态或配置值。
    QColor dockSeparatorHoverColor_; ///< 保存 `dockSeparatorHoverColor` 对应的对象状态或配置值。
};
}
