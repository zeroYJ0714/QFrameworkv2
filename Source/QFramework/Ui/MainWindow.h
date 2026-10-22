#pragma once

// 文件职责：声明主窗口的 UI 协调层。
// MainWindow 把模块状态映射到菜单、Dock、状态栏和管理对话框，但不实现
// 插件加载、进程监督、布局序列化或 QSS 解析本身。

#include <QColor>
#include <QElapsedTimer>
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
class QTimer;

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
    /// @brief 构造主窗口：建立无边框客户区标题栏、菜单、各模块 Dock 和状态栏，并连接管理器状态信号。
    /// @param modules 模块配置快照，决定菜单项、Dock 和状态栏统计的范围；构造后不再从配置刷新。
    /// @param pluginManager 主进程插件管理器，借用指针不参与释放；可为空，为空时插件界面挂接与退出保存跳过。
    /// @param processSupervisor 子进程监督器，借用指针；可为空，为空时子进程窗口嵌入、显示和重启不可用。
    /// @param styleManager 全局 QSS 管理器，借用指针；可为空，为空时“样式”菜单各项点击后直接返回。
    /// @param parent Qt 父控件，允许为空；本窗口由 FrameworkRuntime 管理，且先于三个借用管理器销毁。
    /// @return 无（构造函数不返回值）。
    MainWindow(const QVector<ModuleConfig>& modules,
               PluginManager* pluginManager,
               ProcessSupervisor* processSupervisor,
               StyleManager* styleManager,
               QWidget* parent = nullptr);
    /// @brief 析构主窗口；标题栏、Dock、对话框等子对象交给 Qt 父子关系回收，三个借用管理器不在此释放。
    /// @return 无（析构函数不返回值）。
    ~MainWindow() override;

    // 这两个属性主要供 QSS 的 qproperty-* 使用；未加载 TechDashboard 时使用
    // 当前 Qt 调色板中的 Mid/Highlight 颜色，分隔线仍然自然可见。
    /// @brief 读取 Dock 分隔条的常规颜色，主要供 QSS 的 qproperty-dockSeparatorColor 取值。
    /// @return 当前常规颜色；构造时取调色板 Mid 色，未加载 QSS 时也自然可见。
    QColor dockSeparatorColor() const;
    /// @brief 设置 Dock 分隔条常规颜色；颜色变化时请求整个主窗口重绘，因为分隔条不是独立控件。
    /// @param color 新的常规颜色，通常来自 QSS 的 #RRGGBB；与当前值相同时不做任何事。
    /// @return 无。
    void setDockSeparatorColor(const QColor& color);
    /// @brief 读取 Dock 分隔条鼠标悬停时的颜色，主要供 QSS 的 qproperty-dockSeparatorHoverColor 取值。
    /// @return 当前悬停颜色；构造时取调色板 Highlight 色。
    QColor dockSeparatorHoverColor() const;
    /// @brief 设置 Dock 分隔条悬停颜色；颜色变化时请求整个主窗口重绘。
    /// @param color 新的悬停颜色；与当前值相同时不触发重绘。
    /// @return 无。
    void setDockSeparatorHoverColor(const QColor& color);

    /// @brief 把已加载的主进程插件界面放进对应 Dock 替换占位页，并登记该模块界面可用。
    /// @return 无；未注入 PluginManager、模块被禁用或插件界面尚未加载时跳过该模块。
    void attachInProcessUiModules();
    /// @brief 在插件 DLL 卸载前把界面控件移出 Dock、解除 Qt 父子关系，并换回“模块已停止”占位页。
    /// @return 无；未注入 PluginManager 时直接返回，Dock 中当前不是插件界面时跳过该模块。
    void releaseInProcessUiModules();

    // 由 FrameworkRuntime 在构造完成后注入一次只读布局预设快照。
    /// @brief 注入布局预设快照并重建“布局”菜单里的可勾选预设项，同时逐个校验预设文件是否可用。
    /// @param presets 预设配置列表，按其稳定 index 建立菜单项；重复调用会先清空上一批菜单项，运行期不监听 INI 变化。
    /// @return 无；布局菜单尚未创建时只保存快照，校验失败的预设项被置灰并带上中文错误提示。
    void setLayoutPresets(const QVector<LayoutPresetConfig>& presets);
    // 启动时只尝试 Layout.1；失败不会弹框，也不会建立活动布局。
    /// @brief 启动时尝试加载编号 1 的布局预设，失败只写日志并禁用菜单项，不弹启动警告对话框。
    /// @return 预设 1 存在、菜单项可用且文件恢复成功返回 true；缺失、被禁用或加载失败返回 false。
    bool loadInitialLayoutPreset();

    // 委托 LayoutManager 恢复布局，并再次隐藏当前不可用模块的 Dock。
    /// @brief 委托 LayoutManager 恢复布局文件，成功后将文件里的显示意图整体写回各 Dock，并隐藏当前不可用模块。
    /// @param filePath 要恢复的 .qflayout 文件路径。
    /// @param errorMessage 可选出参，失败时写入中文原因；为空则不写。
    /// @param unavailableModuleIds 可选出参，写入布局中记录但当前不可用的模块 ID；为空则不写。
    /// @param activateLayout true 把该文件记为当前活动布局；false 只应用布局，不改变活动预设与勾选。
    /// @return geometry/state 与全部元数据都恢复成功返回 true；文件缺失、格式不合法或读取失败返回 false。
    bool loadLayoutFile(const QString& filePath,
                        QString* errorMessage = nullptr,
                        QStringList* unavailableModuleIds = nullptr,
                        bool activateLayout = true);
    /// @brief 返回本窗口拥有的布局管理器，供上层或测试直接调用布局 API。
    /// @return LayoutManager 借用指针，随 MainWindow 一起销毁，调用方不得删除；本类中始终非空。
    LayoutManager* layoutManager() const;

private slots:
    /// @brief 轮询插件退出就绪状态，全部保存完成后再次请求关闭窗口。
    /// @return 无；未处于退出准备阶段或插件尚未就绪时等待下一次定时器触发。
    void pollExitReadiness();
    /// @brief 显示并前置唯一的非模态模块管理对话框，不创建第二个实例。
    /// @return 无。
    void showModuleManager();
    /// @brief 响应模块菜单项勾选变化，从 QAction 的 moduleId 动态属性取出模块并提交显示意图。
    /// @param checked 菜单项勾选后的状态：true 请求显示，false 请求隐藏。
    /// @return 无；发送者不是 QAction 时忽略。
    void onModuleActionTriggered(bool checked);
    /// @brief 响应 Dock 自身的关闭按钮，把该模块的显示意图置为隐藏。
    /// @return 无；发送者不是 ManagedDockWidget 时忽略；模块拒绝隐藏时保持原显示状态。
    void onDockCloseRequested();
    /// @brief 响应模块管理对话框的“显示界面”请求，把该模块的显示意图置为显示。
    /// @param moduleId 目标模块的稳定 ID。
    /// @return 无；该模块没有登记 Dock 时忽略。
    void onShowModuleRequested(const QString& moduleId);
    /// @brief 响应模块管理对话框的“重启子进程”请求，把重启交给 ProcessSupervisor 并统一提示失败。
    /// @param moduleId 目标模块的稳定 ID。
    /// @return 无；未注入 ProcessSupervisor 时直接返回；此处只提交请求，真正结果由 restartFinished 通知。
    void onRestartModuleRequested(const QString& moduleId);
    /// @brief 接收子进程重启的最终结果：成功只在状态栏提示，失败弹出错误对话框。
    /// @param moduleId 目标模块的稳定 ID。
    /// @param success 重启是否成功。
    /// @param detail 失败时的具体原因，成功时忽略。
    /// @return 无。
    void onRestartFinished(const QString& moduleId,
                           bool success,
                           const QString& detail);
    /// @brief 接收模块状态变化，同步内部状态表、模块管理表格、子进程占位页和状态栏统计。
    /// @param moduleId 目标模块的稳定 ID。
    /// @param state 新的生命周期状态文本，例如 Running、Stopped。
    /// @param detail 状态附带说明，可为空。
    /// @return 无。
    void onModuleStateChanged(const QString& moduleId,
                              const QString& state,
                              const QString& detail);
    /// @brief 接收监督器耗尽自动重启后的最终故障，弹出用户可见的错误提示。
    /// @param moduleId 目标模块的稳定 ID。
    /// @param detail 具体故障原因。
    /// @return 无。
    void onModuleFault(const QString& moduleId, const QString& detail);
    /// @brief 接收子进程上报的原生窗口句柄，嵌入宿主控件后把该模块界面标记为可用。
    /// @param moduleId 目标模块的稳定 ID。
    /// @param windowId 子进程上报的原生窗口句柄，Windows 下即 HWND。
    /// @return 无；找不到宿主时忽略；嵌入失败弹出错误提示且不标记可用。
    void onWindowHandleReady(const QString& moduleId, quintptr windowId);
    /// @brief 把宿主合并后的最终客户区尺寸转发给仍在运行的子进程窗口。
    /// @param moduleId 目标模块的稳定 ID。
    /// @param size 宿主控件最终的客户区尺寸，单位为逻辑像素。
    /// @return 无；未嵌入窗口、用户未要求显示或子进程不在 Running 时静默忽略；发送失败只写日志。
    void onProcessWindowSizeChanged(const QString& moduleId, const QSize& size);
    /// @brief 响应布局预设菜单项触发，按 layoutIndex 动态属性激活对应预设。
    /// @param checked 勾选状态，由 QActionGroup 维护，本槽忽略。
    /// @return 无；发送者不是 QAction 时忽略。
    void onLayoutPresetTriggered(bool checked);
    /// @brief 弹出文件选择器加载 .qflayout，并把其中当前不可用的模块写进日志。
    /// @return 无；用户取消时不改变窗口状态，加载失败弹出错误提示。
    void loadLayoutFromDialog();
    /// @brief 把当前显示意图覆盖保存到当前活动预设文件，并在状态栏显示实际覆盖的路径。
    /// @return 无；没有活动预设时置灰“保存当前布局”，保存失败弹出错误提示。
    void saveCurrentLayout();
    /// @brief 弹出保存对话框把当前布局另存为新文件，缺少 .qflayout 后缀时自动补齐。
    /// @return 无；用户取消或保存失败都不改变当前活动布局。
    void saveLayoutAs();
    /// @brief 弹出文件选择器加载新的全局 QSS，交给 StyleManager 替换；加载失败保留旧样式。
    /// @return 无；未注入 StyleManager 或用户取消时直接返回。
    void selectStyleSheet();
    /// @brief 重新读取最近一次成功加载的 QSS 文件，便于改完样式立刻看效果。
    /// @return 无；未注入 StyleManager 或重载失败时只提示，不改变当前样式。
    void reloadStyleSheet();
    /// @brief 更新模块界面可用标志，并同步模块管理表格、菜单勾选和 Dock 实际可见性。
    /// @param moduleId 目标模块的稳定 ID。
    /// @param available 界面此刻是否可用（插件已加载或子进程窗口已嵌入）。
    /// @return 无。
    void setUiAvailable(const QString& moduleId, bool available);
    /// @brief 响应标题栏空白区拖动请求：普通状态交给系统窗口管理器，最大化时先恢复普通尺寸并按比例定位。
    /// @param globalPosition 按下时的全局屏幕坐标，用于按横向比例决定恢复后窗口的左边界。
    /// @param titleBarPosition 按下点在标题栏内的坐标，用于让鼠标在恢复后仍停在标题栏同一高度。
    /// @return 无；拿不到原生 QWindow 时直接返回。
    void startWindowMove(const QPoint& globalPosition,
                         const QPoint& titleBarPosition);
    /// @brief 响应标题栏最大化/还原按钮，在最大化与普通窗口状态之间切换。
    /// @return 无。
    void toggleMaximizedState();

protected:
    /// @brief 拦截窗口关闭：首次关闭先让插件保存草稿并启动轮询，就绪后按配置顺序询问各 UI 模块 canClose，全部通过才真正关闭。
    /// @param event 关闭事件；保存未完成或任一模块拒绝时保持 ignore，窗口不关闭。
    /// @return 无。
    void closeEvent(QCloseEvent* event) override;
    /// @brief 处理窗口状态变化，在 WindowStateChange 到达后刷新标题栏的最大化/还原图标。
    /// @param event 变更事件；仅类型为 WindowStateChange 时更新标题栏，可为空。
    /// @return 无。
    void changeEvent(QEvent* event) override;
    /// @brief 处理 Windows 原生消息：接管最大化工作区、自绘标题栏和窗口边框的命中测试，其余交回 Qt 默认实现。
    /// @param eventType 原生事件类型字节串，本实现不使用。
    /// @param message 平台原生消息指针，Windows 下为 MSG*；可为空。
    /// @param result 原生返回值出参，命中标题栏或边框时写入 HT* 值；可为空。
    /// @return 已接管该消息返回 true；否则返回 QMainWindow 默认实现的结果，非 Windows 平台直接走默认实现。
    bool nativeEvent(const QByteArray& eventType,
                     void* message,
                     long* result) override;

private:
    // 显示意图的来源。它决定两件事：能否先询问插件 canClose，以及显示 Dock 后是否重新置顶原标签。
    enum class VisibilityOrigin
    {
        UserAction, ///< 用户在模块菜单或管理对话框里直接切换显示。
        LayoutRestore, ///< 来自布局文件恢复，不触发插件的业务关闭确认。
        WindowReady, ///< 界面就绪（插件加载完成或子进程窗口嵌入成功）后重新计算。
        RuntimeState, ///< 模块状态变化导致界面暂不可用，属于框架内部状态而非用户意图。
        CloseButton ///< 用户点击 Dock 自身的关闭按钮。
    };

    /// @brief 在标题栏内嵌的菜单栏上创建“布局/模块/样式”三组菜单，并把动作连接到本类槽函数。
    /// @return 无；模块菜单只为启用的 UI 模块建可勾选项。
    void createActions();
    /// @brief 为每个启用的 UI 模块创建 Dock 与占位内容：ProcessUi 用 ProcessWindowHost，InProcessUi 用加载占位标签。
    /// @return 无；新建 Dock 统一停靠左侧并标签化隐藏，等待用户或布局文件要求显示。
    void createModuleDocks();
    /// @brief 记录模块的显示意图并立即重算实际可见性；用户取消显示时会先询问模块 canClose。
    /// @param moduleId 目标模块的稳定 ID；没有登记 Dock 的 ID 直接忽略。
    /// @param visible true 表示要求显示，false 表示要求隐藏。
    /// @param origin 该意图的来源，决定被拒绝时是否回滚菜单勾选以及显示后是否置顶。
    /// @return 无。
    void setRequestedDockVisible(const QString& moduleId,
                                 bool visible,
                                 VisibilityOrigin origin);
    /// @brief 按“用户希望显示 + 界面已就绪”两个条件计算 Dock 的实际可见性，并同步子进程窗口的显示。
    /// @param moduleId 目标模块的稳定 ID；没有登记 Dock 的 ID 直接忽略。
    /// @param origin 本次计算的来源；非 UserAction 时显示后把原标签页抬回前台。
    /// @return 无；子进程窗口显示失败时改用占位页并弹出错误提示。
    void applyRequestedDockVisibility(const QString& moduleId,
                                      VisibilityOrigin origin);
    /// @brief 询问主进程插件是否允许隐藏界面，用于拦住会丢弃本地草稿的关闭操作。
    /// @param moduleId 目标模块的稳定 ID。
    /// @return 允许隐藏返回 true；未注入 PluginManager 或该模块界面未加载时同样返回 true。
    bool canHideModule(const QString& moduleId) const;
    /// @brief 把模块菜单项的勾选状态同步为当前显示意图，同步期间阻塞信号避免回环。
    /// @param moduleId 目标模块的稳定 ID。
    /// @return 无；该模块没有菜单项时直接返回。
    void syncModuleAction(const QString& moduleId);
    /// @brief 重新统计已启用模块总数与状态为 Running 的模块数，刷新状态栏摘要。
    /// @return 无。
    void updateStatusSummary();
    /// @brief 同时写集中错误日志并弹出一次用户警告，让故障既可追溯又对用户可见。
    /// @param title 对话框标题，同时作为日志内容前缀。
    /// @param detail 具体失败原因，直接显示给用户。
    /// @return 无。
    void reportStateFailure(const QString& title, const QString& detail);
    /// @brief 激活指定编号的布局预设：成功后才更新活动编号、勾选状态并启用“保存当前布局”。
    /// @param index 预设的稳定编号，用于在配置快照和菜单项中查找。
    /// @param startup 是否为启动阶段调用；启动失败只禁用菜单项并写日志，非启动失败会恢复原勾选并弹窗。
    /// @param errorMessage 可选出参，失败时写入中文原因；为空则不写。
    /// @return 恢复成功返回 true；预设不存在、菜单项被禁用或布局加载失败返回 false。
    bool activateLayoutPreset(int index, bool startup, QString* errorMessage = nullptr);
    /// @brief 在配置快照里按稳定编号查找布局预设，避免用菜单文字反查重名预设。
    /// @param index 预设的稳定编号。
    /// @return 命中配置的指针，指向内部快照，下次注入预设后失效；未找到返回 nullptr。
    const LayoutPresetConfig* layoutPreset(int index) const;
    /// @brief 删除全部已创建的布局预设菜单项并清空映射，保证重复注入不会出现两套菜单。
    /// @return 无。
    void clearLayoutPresetActions();
    /// @brief 取模块的展示名，用于菜单、对话框和占位页文本。
    /// @param moduleId 目标模块的稳定 ID。
    /// @return 配置里的 DisplayName；未配置时回退为 moduleId 本身。
    QString displayName(const QString& moduleId) const;
    /// @brief 生成 Dock 占位页的两行文本：第一行模块展示名，第二行当前状态或原因。
    /// @param moduleId 目标模块的稳定 ID。
    /// @param detail 第二行内容，通常是生命周期状态或具体失败原因。
    /// @return 形如“展示名\n原因”的两行文本。
    QString placeholderText(const QString& moduleId, const QString& detail) const;

    // modules_ 保持配置顺序；modulesById_ 用于按 ID 快速查显示名称。
    QVector<ModuleConfig> modules_; ///< 构造时注入的模块配置快照，保持配置顺序，菜单与 Dock 都按它遍历。
    QHash<QString, ModuleConfig> modulesById_; ///< moduleId 到配置的索引，用于取 DisplayName 等只读字段。
    // 三个管理器为借用指针；layout/dialog/Dock/控件由本窗口拥有。
    PluginManager* pluginManager_; ///< 主进程插件管理器，借用指针不负责释放；可为空，为空时插件相关功能整体跳过。
    QTimer* exitTimer_ = nullptr; ///< 退出准备阶段的 200ms 轮询定时器，首次关闭窗口时按需创建；等待期间消息总线和窗口事件照常处理。
    QElapsedTimer exitElapsed_;
    bool preparingExit_ = false; ///< 是否已进入退出准备阶段：为 true 时再次 close() 只检查插件保存结果，不重新发起保存。
    ProcessSupervisor* processSupervisor_; ///< 子进程监督器，借用指针不负责释放；可为空，为空时子进程窗口嵌入、显示和重启跳过。
    StyleManager* styleManager_; ///< 全局 QSS 管理器，借用指针不负责释放；可为空，为空时“样式”菜单各项直接返回。
    // 由 MainWindow 持有的客户区标题栏；setMenuWidget() 只改变它在 QMainWindow 中的位置。
    WindowTitleBar* titleBar_; ///< 自绘客户区标题栏，由本窗口拥有并承载菜单栏，构造时创建、永不为空。
    LayoutManager* layoutManager_; ///< 布局文件读写与模块 Dock 注册的管理器，由本窗口拥有并随其销毁。
    ModuleManagerDialog* moduleManagerDialog_; ///< 唯一的非模态模块管理对话框，由本窗口拥有，构造时即创建。
    // 下列映射都以稳定 moduleId 为键，指针所有权仍由 Qt 父子关系管理。
    QHash<QString, ManagedDockWidget*> moduleDocks_; ///< moduleId 到 Dock 容器的映射，只包含启用的 UI 模块。
    QHash<QString, ProcessWindowHost*> processHosts_; ///< moduleId 到子进程窗口宿主的映射，只含 ProcessUi 模块。
    QHash<QString, QAction*> moduleActions_; ///< moduleId 到模块菜单可勾选项的映射，用于回写勾选状态。
    // 用户显示意图独立于 Qt 当前标签是否正在绘制，也独立于模块是否 ready。
    QHash<QString, bool> requestedDockVisibility_; ///< 用户或布局要求的显示意图，是判断“该不该显示”的唯一稳定依据。
    QHash<QString, bool> uiAvailable_; ///< 模块界面此刻是否可用：插件已加载或子进程窗口已嵌入。
    QHash<QString, QString> moduleStates_; ///< moduleId 到最近一次生命周期状态文本，用于状态栏 Running 统计。
    QLabel* statusSummaryLabel_; ///< 状态栏常驻的“运行中 x / y”摘要标签，所有权归状态栏。
    QAction* saveLayoutAction_; ///< “保存当前布局”菜单项；没有活动预设时置灰。
    QMenu* layoutMenu_; ///< “布局”菜单，宿主是标题栏内嵌的 QMenuBar。
    QAction* layoutPresetSeparator_; ///< 预设项与加载/保存动作之间的分隔线；没有预设时隐藏。
    QActionGroup* layoutPresetGroup_; ///< 预设菜单项的互斥组，保证同一时刻只有一个预设被勾选。
    QHash<int, QAction*> layoutPresetActions_; ///< 预设编号到可勾选菜单项的映射，注入新快照时先整体清空。
    QVector<LayoutPresetConfig> layoutPresets_; ///< FrameworkRuntime 注入的布局预设只读快照，运行期不再从 INI 刷新。
    int activeLayoutIndex_; ///< 当前活动布局的预设编号，-1 表示尚未建立活动布局。
    QColor dockSeparatorColor_; ///< 分隔条常规颜色，默认取调色板 Mid 色，可被 QSS 的 qproperty-* 改写。
    QColor dockSeparatorHoverColor_; ///< 分隔条鼠标悬停颜色，默认取调色板 Highlight 色，可被 QSS 的 qproperty-* 改写。
};
}
