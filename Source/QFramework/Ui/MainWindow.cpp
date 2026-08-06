#include "MainWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QEvent>
#include <QFileDialog>
#include <QGuiApplication>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QScreen>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QWindow>

#include "InProcessUiModule.h"
#include "LayoutManager.h"
#include "Logger.h"
#include "ManagedDockWidget.h"
#include "ModuleManagerDialog.h"
#include "PluginManager.h"
#include "../Process/ProcessSupervisor.h"
#include "ProcessWindowHost.h"
#include "StyleManager.h"
#include "WindowTitleBar.h"

#ifdef Q_OS_WIN
// 这里只在顶层 MainWindow 处理 Windows 无边框所需的消息。
// WIN32_LEAN_AND_MEAN/NOMINMAX 已由工程预处理宏提供，避免把 Win32 类型扩散到 Qt UI 类。
#include <windows.h>
#include <windowsx.h>
#endif

// MainWindow 只做 UI 编排：从管理器接收状态、更新 Dock/菜单，并把用户操作
// 委托回对应管理器。任何耗时队列或进程等待都不在 UI 控件代码里实现。

namespace qframework
{
namespace
{
// UI 与进程类型判断集中在此，菜单、Dock 和重启按钮共用同一规则。
bool isUiType(ModuleType type)
{
    // 两种 UI 模块都会拥有菜单项和 Dock。
    return type == ModuleType::InProcessUi || type == ModuleType::ProcessUi;
}

bool isProcessType(ModuleType type)
{
    // 只有子进程类型能响应“重新启动”操作。
    return type == ModuleType::ProcessUi || type == ModuleType::ProcessNonUi;
}

// 从系统主题取图标，找不到时使用 Qt 标准图标作为跨机器回退。
QIcon themedIcon(QWidget* widget,
                 const QString& themeName,
                 QStyle::StandardPixmap fallback)
{
    // 优先使用系统主题图标，找不到时回退 Qt 标准图标，保证按钮始终可见。
    QIcon icon = QIcon::fromTheme(themeName);
    if (icon.isNull())
        icon = widget->style()->standardIcon(fallback);
    return icon;
}

#ifdef Q_OS_WIN
// 以 Windows 屏幕像素构造窗口矩形。WM_NCHITTEST 的坐标也是屏幕坐标，先统一成
// QRect 后再做边角判断，比直接在 LPARAM 上做位运算更容易审查和维护。
QRect nativeWindowRect(HWND windowHandle)
{
    RECT nativeRect = {};
    if (windowHandle == nullptr || !GetWindowRect(windowHandle, &nativeRect))
        return QRect();
    return QRect(QPoint(nativeRect.left, nativeRect.top),
                 QSize(nativeRect.right - nativeRect.left,
                       nativeRect.bottom - nativeRect.top));
}

// 返回八方向缩放命中值。角必须先于边返回，否则左上角会被误判成单独的上边或左边。
// Windows 收到这些 HT* 值后会接管鼠标缩放，不需要 Qt 自己循环 move() 窗口。
long resizeHitTest(const QPoint& screenPoint,
                   const QRect& nativeRect,
                   int nativeBorderWidth)
{
    if (!nativeRect.isValid() || nativeBorderWidth <= 0)
        return HTNOWHERE;

    const bool onLeft = screenPoint.x() >= nativeRect.left() &&
                        screenPoint.x() < nativeRect.left() + nativeBorderWidth;
    const bool onRight = screenPoint.x() <= nativeRect.right() &&
                         screenPoint.x() >= nativeRect.right() - nativeBorderWidth + 1;
    const bool onTop = screenPoint.y() >= nativeRect.top() &&
                       screenPoint.y() < nativeRect.top() + nativeBorderWidth;
    const bool onBottom = screenPoint.y() <= nativeRect.bottom() &&
                          screenPoint.y() >= nativeRect.bottom() - nativeBorderWidth + 1;

    if (onTop && onLeft)
        return HTTOPLEFT;
    if (onTop && onRight)
        return HTTOPRIGHT;
    if (onBottom && onLeft)
        return HTBOTTOMLEFT;
    if (onBottom && onRight)
        return HTBOTTOMRIGHT;
    if (onTop)
        return HTTOP;
    if (onBottom)
        return HTBOTTOM;
    if (onLeft)
        return HTLEFT;
    if (onRight)
        return HTRIGHT;
    return HTNOWHERE;
}
#endif
}

// 创建主窗口静态控件和模块占位页，并连接三类管理器的状态信号。
MainWindow::MainWindow(const QVector<ModuleConfig>& modules,
                       PluginManager* pluginManager,
                       ProcessSupervisor* processSupervisor,
                       StyleManager* styleManager,
                       QWidget* parent)
    : QMainWindow(parent),
      modules_(modules),
      pluginManager_(pluginManager),
      processSupervisor_(processSupervisor),
      styleManager_(styleManager),
      titleBar_(new WindowTitleBar(this)),
      layoutManager_(new LayoutManager(this)),
      moduleManagerDialog_(new ModuleManagerDialog(modules, this)),
      statusSummaryLabel_(new QLabel(this)),
      saveLayoutAction_(nullptr),
      layoutMenu_(nullptr),
      layoutPresetSeparator_(nullptr),
      layoutPresetGroup_(nullptr),
      activeLayoutIndex_(-1),
      dockSeparatorColor_(),
      dockSeparatorHoverColor_()
{
    // 先建立模块状态快照，再创建依赖这些状态的菜单和 Dock。
    setObjectName(QStringLiteral("QFrameworkMainWindow"));
    setWindowTitle(QStringLiteral("QFramework"));

    // Qt 5.15 的 QStyleSheetStyle 会先读取 qproperty-* 的当前值类型，再决定怎样
    // 解析 QSS。先放入有效 QColor，后续的 #RRGGBB 才会走 QColor 解析分支；同时
    // 这两个调色板颜色也是未加载 QSS 时的合理默认值。
    dockSeparatorColor_ = palette().color(QPalette::Mid);
    dockSeparatorHoverColor_ = palette().color(QPalette::Highlight);

    // FramelessWindowHint 删除 Windows 原生标题栏；窗口仍保留系统菜单、最小化、
    // 最大化和关闭 flags，随后由 nativeEvent() 补回拖动、缩放和任务栏工作区行为。
    // 不使用 Qt::Tool，确保任务栏仍把它当作正常应用窗口显示。
    setWindowFlags(windowFlags() |
                   Qt::FramelessWindowHint |
                   Qt::WindowSystemMenuHint |
                   Qt::WindowMinimizeButtonHint |
                   Qt::WindowMaximizeButtonHint |
                   Qt::WindowCloseButtonHint);
    // setMenuWidget() 把标题栏放进 QMainWindow 的客户区顶部；标题栏内部的 QMenuBar
    // 接下来会承载原有 QAction，业务动作仍由 MainWindow 保存和连接。
    setMenuWidget(titleBar_);
    connect(titleBar_,
            &WindowTitleBar::moveRequested,
            this,
            &MainWindow::startWindowMove);
    connect(titleBar_,
            &WindowTitleBar::minimizeRequested,
            this,
            &MainWindow::minimizeWindow);
    connect(titleBar_,
            &WindowTitleBar::maximizeRestoreRequested,
            this,
            &MainWindow::toggleMaximizedState);
    connect(titleBar_,
            &WindowTitleBar::closeRequested,
            this,
            &MainWindow::closeWindow);
    setDockNestingEnabled(true);
    // ProcessWindowHost 内部承载跨进程原生 HWND。QMainWindow 默认会为 Dock 重排启动
    // AnimatedDocks，在 HWND 连续改变父级和尺寸的过程中，Qt 可能暂时保留 pluggingWidget
    // 或鼠标捕获状态，导致动画结束前后分隔条、标题栏拖动和关闭按钮都收不到事件。
    // 关闭的只是视觉过渡，不影响 DockWidgetMovable、上下左右停靠或 QSS 外观；布局会
    // 立即完成，原生子窗口也能在同一轮 Qt 事件循环内收到最终尺寸。
    setAnimated(false);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);
    resize(1200, 760);

    for (const ModuleConfig& module : modules_) {
        modulesById_.insert(module.id, module);
        moduleStates_.insert(
            module.id,
            module.enabled ? QString::fromUtf8(u8"等待启动") : QString::fromUtf8(u8"已禁用"));
    }

    createActions();
    createModuleDocks();
    statusBar()->addPermanentWidget(statusSummaryLabel_);
    statusBar()->showMessage(QString::fromUtf8(u8"框架已就绪"), 3000);
    updateStatusSummary();

    connect(moduleManagerDialog_,
            &ModuleManagerDialog::showModuleRequested,
            this,
            &MainWindow::onShowModuleRequested);
    connect(moduleManagerDialog_,
            &ModuleManagerDialog::restartModuleRequested,
            this,
            &MainWindow::onRestartModuleRequested);
    // 主进程插件和子进程监督器使用同一个状态槽，UI 表现保持一致。
    if (pluginManager_ != nullptr) {
        connect(pluginManager_,
                &PluginManager::moduleStateChanged,
                this,
                &MainWindow::onModuleStateChanged);
    }
    if (processSupervisor_ != nullptr) {
        connect(processSupervisor_,
                &ProcessSupervisor::moduleStateChanged,
                this,
                &MainWindow::onModuleStateChanged);
        connect(processSupervisor_,
                &ProcessSupervisor::moduleFault,
                this,
                &MainWindow::onModuleFault);
        connect(processSupervisor_,
                &ProcessSupervisor::windowHandleReady,
                this,
                &MainWindow::onWindowHandleReady);
        connect(processSupervisor_,
                &ProcessSupervisor::operationBusyChanged,
                moduleManagerDialog_,
                &ModuleManagerDialog::setRestartBusy);
        connect(processSupervisor_,
                &ProcessSupervisor::restartFinished,
                this,
                &MainWindow::onRestartFinished);
    }
}

// 子控件和借用管理器均由 Qt/FrameworkRuntime 按父子及依赖顺序回收。
MainWindow::~MainWindow() = default;

QColor MainWindow::dockSeparatorColor() const
{
    return dockSeparatorColor_;
}

void MainWindow::setDockSeparatorColor(const QColor& color)
{
    if (dockSeparatorColor_ == color)
        return;
    dockSeparatorColor_ = color;
    // 分隔条不是独立 QWidget，只能请求整个 QMainWindow 重绘；Qt 会按脏区域裁剪。
    update();
}

QColor MainWindow::dockSeparatorHoverColor() const
{
    return dockSeparatorHoverColor_;
}

void MainWindow::setDockSeparatorHoverColor(const QColor& color)
{
    if (dockSeparatorHoverColor_ == color)
        return;
    dockSeparatorHoverColor_ = color;
    update();
}

// 把已成功加载的主进程 QWidget 替换对应占位页并登记为可用。
void MainWindow::attachInProcessUiModules()
{
    // QPluginLoader 拥有模块对象；Dock 只是临时成为 QWidget 父容器。
    if (pluginManager_ == nullptr)
        return;
    for (const ModuleConfig& module : modules_) {
        if (!module.enabled || module.type != ModuleType::InProcessUi)
            continue;
        InProcessUiModule* widget = pluginManager_->uiModule(module.id);
        ManagedDockWidget* dockWidget = moduleDocks_.value(module.id, nullptr);
        if (widget == nullptr || dockWidget == nullptr)
            continue;

        QWidget* previousWidget = dockWidget->widget();
        // 替换加载占位符，旧占位符延迟删除以尊重当前事件循环。
        dockWidget->setWidget(widget);
        if (previousWidget != nullptr && previousWidget != widget)
            previousWidget->deleteLater();
        setUiAvailable(module.id, true);
    }
}

// 插件 DLL 卸载前移除 QWidget 父关系并换回占位页，避免 DLL 中代码悬空。
void MainWindow::releaseInProcessUiModules()
{
    // DLL 卸载前必须把插件 QWidget 从 Dock 取出并清除 Qt parent。
    if (pluginManager_ == nullptr)
        return;
    for (const ModuleConfig& module : modules_) {
        if (module.type != ModuleType::InProcessUi)
            continue;
        setUiAvailable(module.id, false);
        InProcessUiModule* widget = pluginManager_->uiModule(module.id);
        ManagedDockWidget* dockWidget = moduleDocks_.value(module.id, nullptr);
        if (widget == nullptr || dockWidget == nullptr || dockWidget->widget() != widget)
            continue;

        QLabel* placeholder = new QLabel(
            placeholderText(module.id, QString::fromUtf8(u8"模块已停止")), dockWidget);
        placeholder->setAlignment(Qt::AlignCenter);
        placeholder->setWordWrap(true);
        widget->hide();
        widget->setParent(nullptr);
        dockWidget->setWidget(placeholder);
    }
}

// 委托布局管理器恢复文件，再按当前 UI 可用性隐藏失效 Dock。
bool MainWindow::loadLayoutFile(const QString& filePath,
                                QString* errorMessage,
                                QStringList* unavailableModuleIds,
                                bool activateLayout)
{
    QHash<QString, bool> loadedRequestedVisibility;
    bool legacyVisibilitySemantics = false;
    const bool loaded = layoutManager_->loadLayout(
        filePath,
        &loadedRequestedVisibility,
        errorMessage,
        unavailableModuleIds,
        &legacyVisibilitySemantics,
        activateLayout
            ? LayoutActivation::Activate
            : LayoutActivation::KeepCurrent);
    if (!loaded)
        return false;

    // 只有 geometry/state 和全部元数据都恢复成功，才整体替换显示意图。
    for (QHash<QString, ManagedDockWidget*>::const_iterator iterator =
             moduleDocks_.constBegin(); iterator != moduleDocks_.constEnd(); ++iterator) {
        requestedDockVisibility_.insert(
            iterator.key(), loadedRequestedVisibility.value(iterator.key(), false));
        syncModuleAction(iterator.key());
        applyRequestedDockVisibility(iterator.key(), VisibilityOrigin::LayoutRestore);
    }
    if (legacyVisibilitySemantics) {
        Logger::instance().log(
            LogLevel::Warning,
            QStringLiteral("QFrameworkApp"),
            QString::fromUtf8(u8"已加载旧版布局；请确认模块显示状态后重新保存"));
        statusBar()->showMessage(
            QString::fromUtf8(u8"旧版布局已加载，请确认显示状态后重新保存"), 8000);
    }
    return true;
}

// 由 FrameworkRuntime 注入一次配置快照；运行期间不监听 INI 文件变化。
void MainWindow::setLayoutPresets(const QVector<LayoutPresetConfig>& presets)
{
    clearLayoutPresetActions();
    layoutPresets_ = presets;
    activeLayoutIndex_ = -1;
    if (saveLayoutAction_ != nullptr)
        saveLayoutAction_->setEnabled(false);
    if (layoutMenu_ == nullptr || layoutPresetGroup_ == nullptr ||
        layoutPresetSeparator_ == nullptr) {
        return;
    }

    layoutMenu_->setToolTipsVisible(true);
    for (const LayoutPresetConfig& preset : layoutPresets_) {
        QAction* action = new QAction(
            preset.name.isEmpty()
                ? QString::fromUtf8(u8"布局%1").arg(preset.index)
                : preset.name,
            layoutMenu_);
        action->setCheckable(true);
        action->setProperty("layoutIndex", preset.index);
        layoutPresetGroup_->addAction(action);
        layoutMenu_->insertAction(layoutPresetSeparator_, action);
        layoutPresetActions_.insert(preset.index, action);
        connect(action,
                &QAction::triggered,
                this,
                &MainWindow::onLayoutPresetTriggered);

        QString validationError;
        const bool valid = layoutManager_->validateLayoutFile(
            preset.filePath, &validationError);
        action->setEnabled(valid);
        if (!valid) {
            action->setToolTip(validationError);
            Logger::instance().log(
                LogLevel::Warning,
                QStringLiteral("QFrameworkApp"),
                QString::fromUtf8(u8"布局预设 %1 不可用：%2")
                    .arg(action->text(), validationError));
        } else {
            action->setToolTip(preset.filePath);
        }
    }
    layoutPresetSeparator_->setVisible(!layoutPresetActions_.isEmpty());
}

// 启动只尝试编号 1；错误写日志并禁用该项，不弹出启动警告对话框。
bool MainWindow::loadInitialLayoutPreset()
{
    return activateLayoutPreset(1, true, nullptr);
}

// 查找配置快照中的稳定编号，避免用菜单文字反向匹配重复 Name。
const LayoutPresetConfig* MainWindow::layoutPreset(int index) const
{
    for (const LayoutPresetConfig& preset : layoutPresets_) {
        if (preset.index == index)
            return &preset;
    }
    return nullptr;
}

// 删除旧 QAction 后再注入新快照，保证重复调用不会产生两套菜单项。
void MainWindow::clearLayoutPresetActions()
{
    for (QAction* action : layoutPresetActions_) {
        if (layoutPresetGroup_ != nullptr)
            layoutPresetGroup_->removeAction(action);
        delete action;
    }
    layoutPresetActions_.clear();
}

// 统一处理启动和用户点击；只有成功恢复后才改变活动编号和勾选状态。
bool MainWindow::activateLayoutPreset(int index,
                                      bool startup,
                                      QString* errorMessage)
{
    const LayoutPresetConfig* preset = layoutPreset(index);
    QAction* action = layoutPresetActions_.value(index, nullptr);
    if (preset == nullptr || action == nullptr || !action->isEnabled()) {
        if (errorMessage != nullptr)
            *errorMessage = action != nullptr
                ? action->toolTip()
                : QString::fromUtf8(u8"布局预设不存在：%1").arg(index);
        return false;
    }

    const int previousIndex = activeLayoutIndex_;
    QAction* previousAction = layoutPresetActions_.value(previousIndex, nullptr);
    QString error;
    QStringList unavailableModules;
    if (!loadLayoutFile(preset->filePath,
                        &error,
                        &unavailableModules,
                        true)) {
        if (startup) {
            action->setEnabled(false);
            action->setToolTip(error);
            Logger::instance().log(
                LogLevel::Warning,
                QStringLiteral("QFrameworkApp"),
                QString::fromUtf8(u8"启动布局预设 %1 加载失败：%2")
                    .arg(action->text(), error));
        } else {
            const QSignalBlocker blocker(layoutPresetGroup_);
            if (previousAction != nullptr)
                previousAction->setChecked(true);
            else
                action->setChecked(false);
            reportStateFailure(QString::fromUtf8(u8"布局加载失败"), error);
        }
        if (errorMessage != nullptr)
            *errorMessage = error;
        return false;
    }

    activeLayoutIndex_ = index;
    {
        const QSignalBlocker blocker(layoutPresetGroup_);
        action->setChecked(true);
    }
    if (saveLayoutAction_ != nullptr)
        saveLayoutAction_->setEnabled(true);
    if (!unavailableModules.isEmpty()) {
        Logger::instance().log(
            LogLevel::Warning,
            QStringLiteral("QFrameworkApp"),
            QString::fromUtf8(u8"布局预设中的模块当前不可用：%1")
                .arg(unavailableModules.join(QStringLiteral(", "))));
    }
    statusBar()->showMessage(
        QString::fromUtf8(u8"布局已加载：%1").arg(action->text()), 5000);
    return true;
}

// 返回由 MainWindow 拥有的 LayoutManager 借用指针。
LayoutManager* MainWindow::layoutManager() const
{
    // LayoutManager 生命周期跟随 MainWindow，调用方不得删除。
    return layoutManager_;
}

// 显示单例非模态模块管理对话框。
void MainWindow::showModuleManager()
{
    // 非模态对话框可重复 show/raise，不创建多个实例。
    moduleManagerDialog_->show();
    moduleManagerDialog_->raise();
    moduleManagerDialog_->activateWindow();
}

// 从 QAction 动态属性解析 moduleId，复用统一显示入口。
void MainWindow::onModuleActionTriggered(bool checked)
{
    // 每个 QAction 的动态属性保存 moduleId，避免为每个模块创建专用槽。
    QAction* action = qobject_cast<QAction*>(sender());
    if (action != nullptr) {
        setRequestedDockVisible(action->property("moduleId").toString(),
                                checked,
                                VisibilityOrigin::UserAction);
    }
}

// 只有 Dock 关闭按钮才撤销显示意图；标签切换不会经过这里。
void MainWindow::onDockCloseRequested()
{
    ManagedDockWidget* dockWidget = qobject_cast<ManagedDockWidget*>(sender());
    if (dockWidget == nullptr)
        return;
    setRequestedDockVisible(dockWidget->property("moduleId").toString(),
                            false,
                            VisibilityOrigin::CloseButton);
}

// 处理管理对话框的显示请求。
void MainWindow::onShowModuleRequested(const QString& moduleId)
{
    setRequestedDockVisible(moduleId, true, VisibilityOrigin::UserAction);
}

// 处理管理对话框的子进程重启请求并统一呈现错误。
void MainWindow::onRestartModuleRequested(const QString& moduleId)
{
    // 这里只提交状态转换；QProcess/Socket 信号在后续事件循环中完成重启。
    if (processSupervisor_ == nullptr)
        return;
    QString error;
    if (!processSupervisor_->requestRestart(moduleId, &error))
        reportStateFailure(QString::fromUtf8(u8"子进程重启请求失败"), error);
}

// 最终结果只从状态机 signal 进入 UI，避免请求函数返回值被误当成重启完成。
void MainWindow::onRestartFinished(const QString& moduleId,
                                   bool success,
                                   const QString& detail)
{
    if (success) {
        statusBar()->showMessage(
            QString::fromUtf8(u8"模块 %1 已重新启动").arg(displayName(moduleId)),
            5000);
        return;
    }
    reportStateFailure(
        QString::fromUtf8(u8"模块 %1 重启失败").arg(displayName(moduleId)),
        detail);
}

// 接收插件/监督器状态，更新表格、占位页和状态栏快照。
void MainWindow::onModuleStateChanged(const QString& moduleId,
                                      const QString& state,
                                      const QString& detail)
{
    // 同步三处状态：内部统计、模块管理表格和子进程窗口占位页。
    moduleStates_.insert(moduleId, state);
    moduleManagerDialog_->setModuleState(moduleId, state, detail);

    ProcessWindowHost* host = processHosts_.value(moduleId, nullptr);
    if (host != nullptr &&
        (state == QStringLiteral("Starting") ||
         state == QStringLiteral("Restarting") ||
         state == QStringLiteral("Failed") ||
         state == QStringLiteral("Stopped"))) {
        setUiAvailable(moduleId, false);
        host->showPlaceholder(placeholderText(moduleId, detail.isEmpty() ? state : detail));
    }
    updateStatusSummary();
}

// 将监督器最终故障转换成用户可见错误提示。
void MainWindow::onModuleFault(const QString& moduleId, const QString& detail)
{
    // moduleFault 只在自动重启次数耗尽后发出，因此需要用户可见提示。
    reportStateFailure(
        QString::fromUtf8(u8"模块 %1 已停止自动重启").arg(displayName(moduleId)),
        detail);
}

// 接收子进程原生窗口句柄；是否发送显示命令仍由用户/布局意图决定。
void MainWindow::onWindowHandleReady(const QString& moduleId, quintptr windowId)
{
    // 先包装 HWND，再把 ready 状态交给统一显示入口；明确隐藏时不补发 showWindow。
    ProcessWindowHost* host = processHosts_.value(moduleId, nullptr);
    if (host == nullptr)
        return;
    QString error;
    if (!host->attachWindow(windowId, &error)) {
        reportStateFailure(QString::fromUtf8(u8"子进程窗口嵌入失败"), error);
        return;
    }
    setUiAvailable(moduleId, true);
}

// 宿主连续 resize 合并后，只把最终客户区尺寸转发给用户仍要求显示的当前 generation。
void MainWindow::onProcessWindowSizeChanged(const QString& moduleId,
                                            const QSize& size)
{
    ProcessWindowHost* host = processHosts_.value(moduleId, nullptr);
    ManagedDockWidget* dockWidget = moduleDocks_.value(moduleId, nullptr);
    if (host == nullptr || dockWidget == nullptr || processSupervisor_ == nullptr ||
        !host->hasEmbeddedWindow() ||
        !requestedDockVisibility_.value(moduleId, false) ||
        !uiAvailable_.value(moduleId, false) ||
        processSupervisor_->state(moduleId) != QStringLiteral("Running") ||
        size.width() <= 0 || size.height() <= 0) {
        return;
    }

    // 这里不能再检查 dockWidget->isVisible()：QMainWindow 调整 Dock 标签/分隔区域时
    // 会让 Dock 短暂不可见，但用户菜单仍是勾选状态。requestedDockVisibility_ 才是
    // “用户明确要求显示”的稳定依据；真正取消勾选时它为 false，上面的条件仍会拦截。
    QString error;
    if (!processSupervisor_->resizeWindow(moduleId,
                                          size.width(),
                                          size.height(),
                                          &error)) {
        // 发送失败由 ProcessSupervisor 进入故障/重启状态；resize 路径不弹重复对话框。
        Logger::instance().log(LogLevel::Error,
                               QStringLiteral("QFrameworkApp"),
                               QString::fromUtf8(u8"子进程窗口尺寸同步失败：%1").arg(error));
    }
}

// 预设 QAction 的 checked 变化先由 QActionGroup 完成；槽只提交成功的布局。
void MainWindow::onLayoutPresetTriggered(bool checked)
{
    Q_UNUSED(checked);
    QAction* action = qobject_cast<QAction*>(sender());
    if (action == nullptr)
        return;
    activateLayoutPreset(action->property("layoutIndex").toInt(), false, nullptr);
}

// 打开非原生文件选择器，加载用户选择的 .qflayout 并报告不可用模块。
void MainWindow::loadLayoutFromDialog()
{
    // 使用非原生对话框保证测试和不同开发机行为一致。
    const QString filePath = QFileDialog::getOpenFileName(
        this,
        QString::fromUtf8(u8"加载布局"),
        QString(),
        QString::fromUtf8(u8"QFramework 布局 (*.qflayout)"),
        nullptr,
        QFileDialog::DontUseNativeDialog);
    if (filePath.isEmpty())
        return;

    QString error;
    QStringList unavailable;
    if (!loadLayoutFile(filePath, &error, &unavailable, false)) {
        reportStateFailure(QString::fromUtf8(u8"布局加载失败"), error);
        return;
    }
    if (!unavailable.isEmpty()) {
        Logger::instance().log(
            LogLevel::Warning,
            QStringLiteral("QFrameworkApp"),
            QString::fromUtf8(u8"布局中的模块当前不可用：%1").arg(unavailable.join(QStringLiteral(", "))));
    }
    statusBar()->showMessage(QString::fromUtf8(u8"布局已加载：%1").arg(filePath), 5000);
}

// 保存到当前选中的 INI 预设；没有活动预设时按钮本身保持置灰。
void MainWindow::saveCurrentLayout()
{
    const LayoutPresetConfig* preset = layoutPreset(activeLayoutIndex_);
    if (preset == nullptr || preset->filePath.isEmpty()) {
        if (saveLayoutAction_ != nullptr)
            saveLayoutAction_->setEnabled(false);
        return;
    }
    QString error;
    if (!layoutManager_->saveLayout(preset->filePath,
                                    requestedDockVisibility_,
                                    &error,
                                    LayoutActivation::KeepCurrent)) {
        reportStateFailure(QString::fromUtf8(u8"布局保存失败"), error);
        return;
    }
    // 显示实际覆盖的文件，让用户能够直接确认“保存当前”和“另存为”的区别。
    statusBar()->showMessage(
        QString::fromUtf8(u8"当前布局已保存：%1").arg(preset->filePath), 5000);
}

// 选择新路径并保存布局，必要时自动补齐 .qflayout 后缀。
void MainWindow::saveLayoutAs()
{
    // “另存为”无论当前是否已经加载布局都必须显示路径选择框；当前文件路径只作为
    // 初始建议位置，用户确认的新路径会在保存成功后成为后续“保存当前”的目标。
    const LayoutPresetConfig* preset = layoutPreset(activeLayoutIndex_);
    const QString suggestedPath = preset != nullptr ? preset->filePath : QString();
    QString filePath = QFileDialog::getSaveFileName(
        this,
        QString::fromUtf8(u8"布局另存为"),
        suggestedPath,
        QString::fromUtf8(u8"QFramework 布局 (*.qflayout)"),
        nullptr,
        QFileDialog::DontUseNativeDialog);
    if (filePath.isEmpty())
        return;
    if (!filePath.endsWith(QStringLiteral(".qflayout"), Qt::CaseInsensitive))
        filePath.append(QStringLiteral(".qflayout"));

    QString error;
    if (!layoutManager_->saveLayout(filePath,
                                    requestedDockVisibility_,
                                    &error,
                                    LayoutActivation::KeepCurrent)) {
        reportStateFailure(QString::fromUtf8(u8"布局保存失败"), error);
        return;
    }
    statusBar()->showMessage(QString::fromUtf8(u8"布局已保存：%1").arg(filePath), 5000);
}

// 选择并应用新的全局 QSS，失败时保留旧样式。
void MainWindow::selectStyleSheet()
{
    // StyleManager 采用成功后替换语义，加载失败不会清掉当前 QSS。
    if (styleManager_ == nullptr)
        return;
    const QString filePath = QFileDialog::getOpenFileName(
        this,
        QString::fromUtf8(u8"选择全局 QSS"),
        QString(),
        QString::fromUtf8(u8"Qt 样式表 (*.qss)"),
        nullptr,
        QFileDialog::DontUseNativeDialog);
    if (filePath.isEmpty())
        return;

    QString error;
    if (!styleManager_->loadStyleSheet(filePath, &error)) {
        reportStateFailure(QString::fromUtf8(u8"QSS 加载失败"), error);
        return;
    }
    statusBar()->showMessage(QString::fromUtf8(u8"全局样式已切换：%1").arg(filePath), 5000);
}

// 重新读取最近一次成功的 QSS 文件。
void MainWindow::reloadStyleSheet()
{
    // 重读最近一次成功文件，适合开发时修改样式后快速验证。
    if (styleManager_ == nullptr)
        return;
    QString error;
    if (!styleManager_->reloadStyleSheet(&error)) {
        reportStateFailure(QString::fromUtf8(u8"QSS 重新加载失败"), error);
        return;
    }
    statusBar()->showMessage(QString::fromUtf8(u8"当前 QSS 已重新加载"), 4000);
}

// 标题栏只发出请求，真正的窗口状态变化集中在 MainWindow，避免子控件知道模块或
// 进程实现，也避免绕过 Qt 的 WindowStateChange、closeEvent 和退出清理流程。
void MainWindow::minimizeWindow()
{
    showMinimized();
}

// 普通状态进入最大化，最大化状态恢复普通窗口。Windows 收到标题栏拖动时也可能
// 自动完成“最大化 -> 普通”转换，changeEvent() 会随后更新按钮视觉状态。
void MainWindow::toggleMaximizedState()
{
    if (isMaximized())
        showNormal();
    else
        showMaximized();
}

// close() 会进入现有 QWidget/QMainWindow closeEvent 链路，而不是直接调用 qApp->quit。
// 这样 FrameworkRuntime 仍能按原顺序停止模块、进程和消息总线。
void MainWindow::closeWindow()
{
    close();
}

// 创建文件、模块和样式菜单，并把动作连接到本类槽函数。
void MainWindow::createActions()
{
    // 菜单按文件、模块、样式三组构造；模块菜单可提前记录晚到窗口的显示意图。
    // 复用原有 QAction 和槽，只更换菜单栏宿主，避免出现两套功能不一致的菜单。
    QMenuBar* titleMenuBar = titleBar_->menuBar();
    layoutMenu_ = titleMenuBar->addMenu(QString::fromUtf8(u8"布局(&L)"));
    layoutMenu_->setToolTipsVisible(true);
    layoutPresetGroup_ = new QActionGroup(layoutMenu_);
    layoutPresetGroup_->setExclusive(true);
    layoutPresetSeparator_ = layoutMenu_->addSeparator();
    QAction* loadLayoutAction = layoutMenu_->addAction(
        themedIcon(this, QStringLiteral("document-open"), QStyle::SP_DialogOpenButton),
        QString::fromUtf8(u8"加载布局..."));
    saveLayoutAction_ = layoutMenu_->addAction(
        themedIcon(this, QStringLiteral("document-save"), QStyle::SP_DialogSaveButton),
        QString::fromUtf8(u8"保存当前布局"));
    saveLayoutAction_->setEnabled(false);
    QAction* saveAsAction = layoutMenu_->addAction(QString::fromUtf8(u8"布局另存为..."));
    connect(loadLayoutAction, &QAction::triggered, this, &MainWindow::loadLayoutFromDialog);
    connect(saveLayoutAction_, &QAction::triggered, this, &MainWindow::saveCurrentLayout);
    connect(saveAsAction, &QAction::triggered, this, &MainWindow::saveLayoutAs);

    QMenu* moduleMenu = titleMenuBar->addMenu(QString::fromUtf8(u8"模块(&M)"));
    QAction* managerAction = moduleMenu->addAction(
        themedIcon(this, QStringLiteral("view-list-details"), QStyle::SP_FileDialogDetailedView),
        QString::fromUtf8(u8"模块管理"));
    connect(managerAction, &QAction::triggered, this, &MainWindow::showModuleManager);
    moduleMenu->addSeparator();

    // 只给启用的 UI 模块创建可勾选菜单项。
    for (const ModuleConfig& module : modules_) {
        if (!module.enabled || !isUiType(module.type))
            continue;
        QAction* action = moduleMenu->addAction(
            module.displayName.isEmpty() ? module.id : module.displayName);
        action->setCheckable(true);
        action->setEnabled(true);
        action->setObjectName(QStringLiteral("ModuleAction.%1").arg(module.id));
        action->setProperty("moduleId", module.id);
        connect(action, &QAction::triggered, this, &MainWindow::onModuleActionTriggered);
        moduleActions_.insert(module.id, action);
    }

    QMenu* styleMenu = titleMenuBar->addMenu(QString::fromUtf8(u8"样式(&S)"));
    QAction* selectStyleAction = styleMenu->addAction(
        themedIcon(this, QStringLiteral("preferences-desktop-theme"), QStyle::SP_DesktopIcon),
        QString::fromUtf8(u8"选择 QSS..."));
    QAction* reloadStyleAction = styleMenu->addAction(
        themedIcon(this, QStringLiteral("view-refresh"), QStyle::SP_BrowserReload),
        QString::fromUtf8(u8"重新加载当前 QSS"));
    connect(selectStyleAction, &QAction::triggered, this, &MainWindow::selectStyleSheet);
    connect(reloadStyleAction, &QAction::triggered, this, &MainWindow::reloadStyleSheet);

}

// 为启用的 UI 模块创建 Dock/占位页，并注册可见性和模块 ID 映射。
void MainWindow::createModuleDocks()
{
    // 每个启用的 UI 模块预先创建 Dock：进程 UI 使用 ProcessWindowHost，
    // 主进程 UI 先放占位标签，插件启动后再替换。
    ManagedDockWidget* previousDock = nullptr;
    for (const ModuleConfig& module : modules_) {
        if (!module.enabled || !isUiType(module.type))
            continue;

        ManagedDockWidget* dockWidget = new ManagedDockWidget(
            module.displayName.isEmpty() ? module.id : module.displayName, this);
        dockWidget->setObjectName(QStringLiteral("ModuleDock.%1").arg(module.id));
        dockWidget->setProperty("moduleId", module.id);
        // 保留足够最小尺寸，使两个模块 Dock 水平拆分后仍可操作。
        dockWidget->setMinimumSize(180, 120);

        if (module.type == ModuleType::ProcessUi) {
            ProcessWindowHost* host = new ProcessWindowHost(dockWidget);
            dockWidget->setWidget(host);
            connect(host,
                    &ProcessWindowHost::clientSizeChanged,
                    this,
                    [this, module](const QSize& size) {
                        onProcessWindowSizeChanged(module.id, size);
                    });
            processHosts_.insert(module.id, host);
        } else {
            QLabel* placeholder = new QLabel(
                placeholderText(module.id, QString::fromUtf8(u8"模块正在加载")), dockWidget);
            placeholder->setAlignment(Qt::AlignCenter);
            placeholder->setWordWrap(true);
            dockWidget->setWidget(placeholder);
        }

        // 初始把多个 Dock 标签化并隐藏，用户或布局文件决定何时显示。
        addDockWidget(Qt::LeftDockWidgetArea, dockWidget);
        if (previousDock != nullptr)
            tabifyDockWidget(previousDock, dockWidget);
        dockWidget->hide();
        connect(dockWidget,
                &ManagedDockWidget::closeRequested,
                this,
                &MainWindow::onDockCloseRequested);
        moduleDocks_.insert(module.id, dockWidget);
        requestedDockVisibility_.insert(module.id, false);
        uiAvailable_.insert(module.id, false);
        layoutManager_->registerModuleDock(module.id, dockWidget);
        previousDock = dockWidget;
    }
}

// 同步菜单、管理对话框和布局注册表中的 UI 可用标志。
void MainWindow::setUiAvailable(const QString& moduleId, bool available)
{
    // ready 只决定此刻能否显示；不能覆盖用户或布局保存的显示意图。
    uiAvailable_.insert(moduleId, available);
    moduleManagerDialog_->setUiAvailable(moduleId, available);
    syncModuleAction(moduleId);
    applyRequestedDockVisibility(
        moduleId, available ? VisibilityOrigin::WindowReady : VisibilityOrigin::RuntimeState);
}

// 写入用户/布局意图后再统一计算实际可见性。
void MainWindow::setRequestedDockVisible(const QString& moduleId,
                                         bool visible,
                                         VisibilityOrigin origin)
{
    if (!moduleDocks_.contains(moduleId))
        return;
    requestedDockVisibility_.insert(moduleId, visible);
    syncModuleAction(moduleId);
    applyRequestedDockVisibility(moduleId, origin);
}

// 实际显示必须同时满足“用户希望显示”和“模块界面已经 ready”。
void MainWindow::applyRequestedDockVisibility(const QString& moduleId,
                                              VisibilityOrigin origin)
{
    ManagedDockWidget* dockWidget = moduleDocks_.value(moduleId, nullptr);
    if (dockWidget == nullptr)
        return;
    if (!requestedDockVisibility_.value(moduleId, false) ||
        !uiAvailable_.value(moduleId, false)) {
        dockWidget->hide();
        return;
    }
    if (dockWidgetArea(dockWidget) == Qt::NoDockWidgetArea)
        addDockWidget(Qt::LeftDockWidgetArea, dockWidget);

    // 布局恢复或窗口晚到时，记住当前标签，show 后再抬回，避免抢走用户焦点。
    ManagedDockWidget* activeTab = nullptr;
    if (origin != VisibilityOrigin::UserAction) {
        const QList<QDockWidget*> siblings = tabifiedDockWidgets(dockWidget);
        for (QDockWidget* sibling : siblings) {
            if (sibling != nullptr && sibling->isVisible()) {
                activeTab = qobject_cast<ManagedDockWidget*>(sibling);
                break;
            }
        }
    }
    dockWidget->show();

    // ProcessUi 只有在显示意图和 ready 同时成立时才收到 showWindow；后续
    // resizeEvent 使用独立 resizeWindow 帧，不会重复调用子进程 QWidget::show()。
    ProcessWindowHost* host = processHosts_.value(moduleId, nullptr);
    if (host != nullptr && host->hasEmbeddedWindow()) {
        QString error;
        if (processSupervisor_ == nullptr ||
            !processSupervisor_->showWindow(moduleId, &error)) {
            host->showPlaceholder(placeholderText(moduleId, error));
            reportStateFailure(QString::fromUtf8(u8"子进程窗口显示失败"), error);
            return;
        }
    }
    if (origin == VisibilityOrigin::UserAction)
        dockWidget->raise();
    else if (activeTab != nullptr)
        activeTab->raise();
}

// 程序同步 QAction 时阻塞信号，避免把状态回写误当成一次用户操作。
void MainWindow::syncModuleAction(const QString& moduleId)
{
    QAction* action = moduleActions_.value(moduleId, nullptr);
    if (action == nullptr)
        return;
    const QSignalBlocker blocker(action);
    action->setChecked(requestedDockVisibility_.value(moduleId, false));
}

// 重新计算状态栏“运行中/启用总数”摘要。
void MainWindow::updateStatusSummary()
{
    // 状态栏分母只统计 enabled 模块，分子只统计精确 Running 状态。
    int enabledCount = 0;
    int runningCount = 0;
    for (const ModuleConfig& module : modules_) {
        if (!module.enabled)
            continue;
        ++enabledCount;
        if (moduleStates_.value(module.id) == QStringLiteral("Running"))
            ++runningCount;
    }
    statusSummaryLabel_->setText(
        QString::fromUtf8(u8"运行中 %1 / %2").arg(runningCount).arg(enabledCount));
}

// 同时写错误日志并弹出一次用户提示，保证故障既可见又可追溯。
void MainWindow::reportStateFailure(const QString& title, const QString& detail)
{
    // 故障同时写集中日志和显示一次用户提示，便于事后追踪。
    Logger::instance().log(
        LogLevel::Error,
        QStringLiteral("QFrameworkApp"),
        title + QStringLiteral(": ") + detail);
    QMessageBox::warning(this, title, detail);
}

// 取配置 DisplayName；为空时回退稳定 moduleId。
QString MainWindow::displayName(const QString& moduleId) const
{
    // 未配置 DisplayName 时回退稳定 moduleId。
    const ModuleConfig module = modulesById_.value(moduleId);
    return module.displayName.isEmpty() ? moduleId : module.displayName;
}

// 生成占位页的两行文本：模块显示名 + 当前生命周期原因。
QString MainWindow::placeholderText(const QString& moduleId,
                                    const QString& detail) const
{
    // 两行格式让占位页同时显示模块名和当前原因。
    return QString::fromUtf8(u8"%1\n%2").arg(displayName(moduleId), detail);
}

// Qt 在 showMaximized()/showNormal() 或系统贴边恢复后发送 WindowStateChange。
// 先让 QMainWindow 完成自身状态更新，再让标题栏刷新最大化/还原图标和辅助文本。
void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event != nullptr && event->type() == QEvent::WindowStateChange && titleBar_ != nullptr)
        titleBar_->updateWindowControlState(isMaximized());
}

// 使用 Qt 官方系统移动接口启动拖动。普通状态直接交给窗口管理器；最大化状态先
// 恢复到 normalGeometry()，并按鼠标在最大化窗口中的横向比例放置恢复窗口，避免
// 窗口突然跳到屏幕一侧。下一轮事件循环开始系统拖动时，鼠标左键仍保持按下。
void MainWindow::startWindowMove(const QPoint& globalPosition,
                                 const QPoint& titleBarPosition)
{
    QWindow* nativeWindow = windowHandle();
    if (nativeWindow == nullptr)
        return;

    if (!isMaximized()) {
        nativeWindow->startSystemMove();
        return;
    }

    QRect restoredGeometry = normalGeometry();
    if (!restoredGeometry.isValid() || restoredGeometry.width() <= 0 ||
        restoredGeometry.height() <= 0) {
        restoredGeometry = QRect(QPoint(), QSize(1200, 760));
    }

    const int currentWidth = qMax(1, width());
    const qreal horizontalRatio = qBound(
        0.0,
        static_cast<qreal>(globalPosition.x() - frameGeometry().left()) /
            static_cast<qreal>(currentWidth),
        1.0);
    QPoint restoredTopLeft(
        globalPosition.x() - qRound(horizontalRatio * restoredGeometry.width()),
        globalPosition.y() - titleBarPosition.y());

    QScreen* targetScreen = QGuiApplication::screenAt(globalPosition);
    if (targetScreen == nullptr)
        targetScreen = nativeWindow->screen();
    if (targetScreen != nullptr) {
        const QRect available = targetScreen->availableGeometry();
        const int maximumX = qMax(
            available.left(), available.right() - restoredGeometry.width() + 1);
        const int maximumY = qMax(
            available.top(), available.bottom() - restoredGeometry.height() + 1);
        restoredTopLeft.setX(qBound(available.left(),
                                    restoredTopLeft.x(),
                                    maximumX));
        restoredTopLeft.setY(qBound(available.top(),
                                    restoredTopLeft.y(),
                                    maximumY));
    }

    showNormal();
    setGeometry(QRect(restoredTopLeft, restoredGeometry.size()));
    QTimer::singleShot(0, this, [this]() {
        QWindow* restoredWindow = windowHandle();
        if (restoredWindow != nullptr)
            restoredWindow->startSystemMove();
    });
}

// 无边框窗口不再有 Windows 原生非客户区，所以这里把“边缘/标题栏/客户区”的命中
// 结果交给 Windows。Windows 收到 HT* 返回值后负责移动、八方向缩放和贴边，不需要
// 自己调用 QWidget::move() 写一个持续运行的鼠标循环。
bool MainWindow::nativeEvent(const QByteArray& eventType,
                             void* message,
                             long* result)
{
#ifdef Q_OS_WIN
    Q_UNUSED(eventType)
    if (message != nullptr && result != nullptr) {
        MSG* nativeMessage = static_cast<MSG*>(message);

        if (nativeMessage->message == WM_GETMINMAXINFO) {
            // 最大化尺寸必须使用 monitor 工作区 rcWork，而不是完整 rcMonitor；
            // 这样任务栏位于上下左右任意一侧时都不会被无边框窗口覆盖。
            MINMAXINFO* minMaxInfo = reinterpret_cast<MINMAXINFO*>(nativeMessage->lParam);
            MONITORINFO monitorInfo = {};
            monitorInfo.cbSize = sizeof(MONITORINFO);
            HMONITOR monitor = MonitorFromWindow(
                nativeMessage->hwnd, MONITOR_DEFAULTTONEAREST);
            if (minMaxInfo != nullptr && monitor != nullptr &&
                GetMonitorInfo(monitor, &monitorInfo)) {
                const RECT& monitorRect = monitorInfo.rcMonitor;
                const RECT& workRect = monitorInfo.rcWork;
                minMaxInfo->ptMaxPosition.x = workRect.left - monitorRect.left;
                minMaxInfo->ptMaxPosition.y = workRect.top - monitorRect.top;
                minMaxInfo->ptMaxSize.x = workRect.right - workRect.left;
                minMaxInfo->ptMaxSize.y = workRect.bottom - workRect.top;
                *result = 0;
                return true;
            }
        }

        if (nativeMessage->message == WM_NCHITTEST) {
            const QPoint screenPoint(GET_X_LPARAM(nativeMessage->lParam),
                                     GET_Y_LPARAM(nativeMessage->lParam));
            const QRect windowRect = nativeWindowRect(nativeMessage->hwnd);

            // 最大化状态下 Windows 已经把窗口放到工作区，边缘不再代表缩放区域。
            // 普通状态先判断角，再判断边，保证角落获得正确的斜向调整光标。
            if (!isMaximized() && windowRect.isValid()) {
                const qreal deviceScale = devicePixelRatioF() > 0.0
                    ? devicePixelRatioF() : 1.0;
                const int nativeBorderWidth = qMax(1, qRound(6.0 * deviceScale));
                const long resizeResult = resizeHitTest(
                    screenPoint, windowRect, nativeBorderWidth);
                if (resizeResult != HTNOWHERE) {
                    *result = resizeResult;
                    return true;
                }
            }

            // WM_NCHITTEST 给的是 Windows 屏幕物理像素，而 QWidget::childAt 使用 Qt
            // 逻辑像素。先转到客户区，再按当前窗口 DPI 换算，避免 125%/150% 下错位。
            POINT nativeClientPoint = {screenPoint.x(), screenPoint.y()};
            if (nativeMessage->hwnd != nullptr &&
                ScreenToClient(nativeMessage->hwnd, &nativeClientPoint)) {
                const qreal deviceScale = devicePixelRatioF() > 0.0
                    ? devicePixelRatioF() : 1.0;
                const QPoint logicalClientPoint(
                    qRound(static_cast<qreal>(nativeClientPoint.x) / deviceScale),
                    qRound(static_cast<qreal>(nativeClientPoint.y) / deviceScale));
                if (titleBar_ != nullptr) {
                    const QPoint titleBarPoint = titleBar_->mapFrom(
                        this, logicalClientPoint);
                    if (titleBar_->rect().contains(titleBarPoint)) {
                        // 整个自绘标题栏保持 Qt 客户区。菜单和按钮由各自控件处理；空白区
                        // 由 WindowTitleBar 的 mousePressEvent 调用 QWindow::startSystemMove()。
                        // 不再返回 HTCAPTION，避免无边框+最大化+原生子窗口组合下 Windows
                        // 非客户区拖动链失效，同时保留 Qt 双击最大化/还原处理。
                        *result = HTCLIENT;
                        return true;
                    }
                }
            }

            // Dock 标题栏、关闭按钮和 QMainWindow::separator 都属于普通 Qt 客户区。
            // 这里不能提前返回“已经处理”：那会截断 Qt/Windows 后续的客户区命中链，
            // 无边框窗口最大化后尤其容易表现为 Dock 看得见但收不到拖动或点击。
            // 只让本函数接管上面的窗口边缘和自绘顶部标题栏，其余区域回到
            // QMainWindow/QWidget 的默认 nativeEvent 路径，由 Qt 把鼠标交给实际子控件。
            return QMainWindow::nativeEvent(eventType, message, result);
        }
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
    Q_UNUSED(result)
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}
}
