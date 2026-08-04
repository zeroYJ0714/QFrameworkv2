#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QFileDialog>
#include <QIcon>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>

#include "InProcessUiModule.h"
#include "LayoutManager.h"
#include "Logger.h"
#include "ManagedDockWidget.h"
#include "ModuleManagerDialog.h"
#include "PluginManager.h"
#include "../Process/ProcessSupervisor.h"
#include "ProcessWindowHost.h"
#include "StyleManager.h"

namespace qframework
{
namespace
{
bool isUiType(ModuleType type)
{
    return type == ModuleType::InProcessUi || type == ModuleType::ProcessUi;
}

bool isProcessType(ModuleType type)
{
    return type == ModuleType::ProcessUi || type == ModuleType::ProcessNonUi;
}

QIcon themedIcon(QWidget* widget,
                 const QString& themeName,
                 QStyle::StandardPixmap fallback)
{
    QIcon icon = QIcon::fromTheme(themeName);
    if (icon.isNull())
        icon = widget->style()->standardIcon(fallback);
    return icon;
}
}

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
      layoutManager_(new LayoutManager(this)),
      moduleManagerDialog_(new ModuleManagerDialog(modules, this)),
      statusSummaryLabel_(new QLabel(this)),
      saveLayoutAction_(nullptr)
{
    setObjectName(QStringLiteral("QFrameworkMainWindow"));
    setWindowTitle(QStringLiteral("QFramework"));
    setDockNestingEnabled(true);
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
    }
}

MainWindow::~MainWindow() = default;

void MainWindow::attachInProcessUiModules()
{
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
        dockWidget->setWidget(widget);
        if (previousWidget != nullptr && previousWidget != widget)
            previousWidget->deleteLater();
        setUiAvailable(module.id, true);
    }
}

void MainWindow::releaseInProcessUiModules()
{
    if (pluginManager_ == nullptr)
        return;
    for (const ModuleConfig& module : modules_) {
        if (module.type != ModuleType::InProcessUi)
            continue;
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

bool MainWindow::loadLayoutFile(const QString& filePath,
                                QString* errorMessage,
                                QStringList* unavailableModuleIds)
{
    const bool loaded = layoutManager_->loadLayout(
        filePath, errorMessage, unavailableModuleIds);
    if (!loaded)
        return false;

    for (QHash<QString, ManagedDockWidget*>::const_iterator iterator =
             moduleDocks_.constBegin(); iterator != moduleDocks_.constEnd(); ++iterator) {
        if (!uiAvailable_.value(iterator.key(), false) && iterator.value() != nullptr)
            iterator.value()->hide();
    }
    return true;
}

LayoutManager* MainWindow::layoutManager() const
{
    return layoutManager_;
}

void MainWindow::showModuleManager()
{
    moduleManagerDialog_->show();
    moduleManagerDialog_->raise();
    moduleManagerDialog_->activateWindow();
}

void MainWindow::onModuleActionTriggered()
{
    QAction* action = qobject_cast<QAction*>(sender());
    if (action != nullptr)
        showModule(action->property("moduleId").toString());
}

void MainWindow::onDockVisibilityChanged(bool visible)
{
    ManagedDockWidget* dockWidget = qobject_cast<ManagedDockWidget*>(sender());
    if (dockWidget == nullptr)
        return;
    QAction* action = moduleActions_.value(
        dockWidget->property("moduleId").toString(), nullptr);
    if (action != nullptr) {
        const QSignalBlocker blocker(action);
        action->setChecked(visible);
    }
}

void MainWindow::onShowModuleRequested(const QString& moduleId)
{
    showModule(moduleId);
}

void MainWindow::onRestartModuleRequested(const QString& moduleId)
{
    if (processSupervisor_ == nullptr)
        return;
    QString error;
    if (!processSupervisor_->restart(moduleId, &error))
        reportStateFailure(QString::fromUtf8(u8"子进程重启失败"), error);
}

void MainWindow::onModuleStateChanged(const QString& moduleId,
                                      const QString& state,
                                      const QString& detail)
{
    moduleStates_.insert(moduleId, state);
    moduleManagerDialog_->setModuleState(moduleId, state, detail);

    ProcessWindowHost* host = processHosts_.value(moduleId, nullptr);
    if (host != nullptr &&
        (state == QStringLiteral("Starting") ||
         state == QStringLiteral("Restarting") ||
         state == QStringLiteral("Failed") ||
         state == QStringLiteral("Stopped"))) {
        host->showPlaceholder(placeholderText(moduleId, detail.isEmpty() ? state : detail));
    }
    updateStatusSummary();
}

void MainWindow::onModuleFault(const QString& moduleId, const QString& detail)
{
    reportStateFailure(
        QString::fromUtf8(u8"模块 %1 已停止自动重启").arg(displayName(moduleId)),
        detail);
}

void MainWindow::onWindowHandleReady(const QString& moduleId, quintptr windowId)
{
    ProcessWindowHost* host = processHosts_.value(moduleId, nullptr);
    if (host == nullptr)
        return;
    QString error;
    if (!host->attachWindow(windowId, &error)) {
        reportStateFailure(QString::fromUtf8(u8"子进程窗口嵌入失败"), error);
        return;
    }
    if (processSupervisor_ == nullptr ||
        !processSupervisor_->showWindow(moduleId,
                                        host->width(),
                                        host->height(),
                                        &error)) {
        host->showPlaceholder(placeholderText(moduleId, error));
        reportStateFailure(QString::fromUtf8(u8"子进程窗口显示失败"), error);
        return;
    }
    setUiAvailable(moduleId, true);
}

void MainWindow::loadLayoutFromDialog()
{
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
    if (!loadLayoutFile(filePath, &error, &unavailable)) {
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

void MainWindow::saveCurrentLayout()
{
    if (layoutManager_->activeFilePath().isEmpty()) {
        saveLayoutAs();
        return;
    }
    QString error;
    if (!layoutManager_->saveLayout(layoutManager_->activeFilePath(), &error)) {
        reportStateFailure(QString::fromUtf8(u8"布局保存失败"), error);
        return;
    }
    statusBar()->showMessage(QString::fromUtf8(u8"当前布局已保存"), 4000);
}

void MainWindow::saveLayoutAs()
{
    QString filePath = QFileDialog::getSaveFileName(
        this,
        QString::fromUtf8(u8"布局另存为"),
        QString(),
        QString::fromUtf8(u8"QFramework 布局 (*.qflayout)"),
        nullptr,
        QFileDialog::DontUseNativeDialog);
    if (filePath.isEmpty())
        return;
    if (!filePath.endsWith(QStringLiteral(".qflayout"), Qt::CaseInsensitive))
        filePath.append(QStringLiteral(".qflayout"));

    QString error;
    if (!layoutManager_->saveLayout(filePath, &error)) {
        reportStateFailure(QString::fromUtf8(u8"布局保存失败"), error);
        return;
    }
    statusBar()->showMessage(QString::fromUtf8(u8"布局已保存：%1").arg(filePath), 5000);
}

void MainWindow::selectStyleSheet()
{
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

void MainWindow::reloadStyleSheet()
{
    if (styleManager_ == nullptr)
        return;
    QString error;
    if (!styleManager_->reloadStyleSheet(&error)) {
        reportStateFailure(QString::fromUtf8(u8"QSS 重新加载失败"), error);
        return;
    }
    statusBar()->showMessage(QString::fromUtf8(u8"当前 QSS 已重新加载"), 4000);
}

void MainWindow::createActions()
{
    QMenu* fileMenu = menuBar()->addMenu(QString::fromUtf8(u8"文件(&F)"));
    QAction* loadLayoutAction = fileMenu->addAction(
        themedIcon(this, QStringLiteral("document-open"), QStyle::SP_DialogOpenButton),
        QString::fromUtf8(u8"加载布局..."));
    saveLayoutAction_ = fileMenu->addAction(
        themedIcon(this, QStringLiteral("document-save"), QStyle::SP_DialogSaveButton),
        QString::fromUtf8(u8"保存当前布局"));
    QAction* saveAsAction = fileMenu->addAction(QString::fromUtf8(u8"布局另存为..."));
    fileMenu->addSeparator();
    QAction* exitAction = fileMenu->addAction(QString::fromUtf8(u8"退出"));
    connect(loadLayoutAction, &QAction::triggered, this, &MainWindow::loadLayoutFromDialog);
    connect(saveLayoutAction_, &QAction::triggered, this, &MainWindow::saveCurrentLayout);
    connect(saveAsAction, &QAction::triggered, this, &MainWindow::saveLayoutAs);
    connect(exitAction, &QAction::triggered, qApp, &QApplication::quit);

    QMenu* moduleMenu = menuBar()->addMenu(QString::fromUtf8(u8"模块(&M)"));
    QAction* managerAction = moduleMenu->addAction(
        themedIcon(this, QStringLiteral("view-list-details"), QStyle::SP_FileDialogDetailedView),
        QString::fromUtf8(u8"模块管理"));
    connect(managerAction, &QAction::triggered, this, &MainWindow::showModuleManager);
    moduleMenu->addSeparator();

    for (const ModuleConfig& module : modules_) {
        if (!module.enabled || !isUiType(module.type))
            continue;
        QAction* action = moduleMenu->addAction(
            module.displayName.isEmpty() ? module.id : module.displayName);
        action->setCheckable(true);
        action->setEnabled(false);
        action->setProperty("moduleId", module.id);
        connect(action, &QAction::triggered, this, &MainWindow::onModuleActionTriggered);
        moduleActions_.insert(module.id, action);
    }

    QMenu* styleMenu = menuBar()->addMenu(QString::fromUtf8(u8"样式(&S)"));
    QAction* selectStyleAction = styleMenu->addAction(
        themedIcon(this, QStringLiteral("preferences-desktop-theme"), QStyle::SP_DesktopIcon),
        QString::fromUtf8(u8"选择 QSS..."));
    QAction* reloadStyleAction = styleMenu->addAction(
        themedIcon(this, QStringLiteral("view-refresh"), QStyle::SP_BrowserReload),
        QString::fromUtf8(u8"重新加载当前 QSS"));
    connect(selectStyleAction, &QAction::triggered, this, &MainWindow::selectStyleSheet);
    connect(reloadStyleAction, &QAction::triggered, this, &MainWindow::reloadStyleSheet);

    QToolBar* navigationToolBar = new QToolBar(QString::fromUtf8(u8"框架工具"), this);
    navigationToolBar->setObjectName(QStringLiteral("FrameworkNavigationToolBar"));
    navigationToolBar->setMovable(false);
    navigationToolBar->setFloatable(false);
    navigationToolBar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    navigationToolBar->setIconSize(QSize(22, 22));
    navigationToolBar->addAction(managerAction);
    navigationToolBar->addSeparator();
    navigationToolBar->addAction(loadLayoutAction);
    navigationToolBar->addAction(selectStyleAction);
    addToolBar(Qt::LeftToolBarArea, navigationToolBar);
}

void MainWindow::createModuleDocks()
{
    ManagedDockWidget* previousDock = nullptr;
    for (const ModuleConfig& module : modules_) {
        if (!module.enabled || !isUiType(module.type))
            continue;

        ManagedDockWidget* dockWidget = new ManagedDockWidget(
            module.displayName.isEmpty() ? module.id : module.displayName, this);
        dockWidget->setObjectName(QStringLiteral("ModuleDock.%1").arg(module.id));
        dockWidget->setProperty("moduleId", module.id);
        // Keep enough room for two module docks to be split horizontally.
        dockWidget->setMinimumSize(180, 120);

        if (module.type == ModuleType::ProcessUi) {
            ProcessWindowHost* host = new ProcessWindowHost(dockWidget);
            dockWidget->setWidget(host);
            processHosts_.insert(module.id, host);
        } else {
            QLabel* placeholder = new QLabel(
                placeholderText(module.id, QString::fromUtf8(u8"模块正在加载")), dockWidget);
            placeholder->setAlignment(Qt::AlignCenter);
            placeholder->setWordWrap(true);
            dockWidget->setWidget(placeholder);
        }

        addDockWidget(Qt::LeftDockWidgetArea, dockWidget);
        if (previousDock != nullptr)
            tabifyDockWidget(previousDock, dockWidget);
        dockWidget->hide();
        connect(dockWidget,
                &QDockWidget::visibilityChanged,
                this,
                &MainWindow::onDockVisibilityChanged);
        moduleDocks_.insert(module.id, dockWidget);
        uiAvailable_.insert(module.id, false);
        previousDock = dockWidget;
    }
}

void MainWindow::setUiAvailable(const QString& moduleId, bool available)
{
    uiAvailable_.insert(moduleId, available);
    QAction* action = moduleActions_.value(moduleId, nullptr);
    if (action != nullptr)
        action->setEnabled(available);
    moduleManagerDialog_->setUiAvailable(moduleId, available);
    if (available) {
        ManagedDockWidget* dockWidget = moduleDocks_.value(moduleId, nullptr);
        if (dockWidget != nullptr)
            layoutManager_->registerModuleDock(moduleId, dockWidget);
    }
}

void MainWindow::showModule(const QString& moduleId)
{
    ManagedDockWidget* dockWidget = moduleDocks_.value(moduleId, nullptr);
    if (dockWidget == nullptr || !uiAvailable_.value(moduleId, false))
        return;
    if (dockWidgetArea(dockWidget) == Qt::NoDockWidgetArea)
        addDockWidget(Qt::LeftDockWidgetArea, dockWidget);
    dockWidget->show();
    dockWidget->raise();
}

void MainWindow::updateStatusSummary()
{
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

void MainWindow::reportStateFailure(const QString& title, const QString& detail)
{
    Logger::instance().log(
        LogLevel::Error,
        QStringLiteral("QFrameworkApp"),
        title + QStringLiteral(": ") + detail);
    QMessageBox::warning(this, title, detail);
}

QString MainWindow::displayName(const QString& moduleId) const
{
    const ModuleConfig module = modulesById_.value(moduleId);
    return module.displayName.isEmpty() ? moduleId : module.displayName;
}

QString MainWindow::placeholderText(const QString& moduleId,
                                    const QString& detail) const
{
    return QString::fromUtf8(u8"%1\n%2").arg(displayName(moduleId), detail);
}
}
