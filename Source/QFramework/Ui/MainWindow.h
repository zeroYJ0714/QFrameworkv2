#pragma once

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
    MainWindow(const QVector<ModuleConfig>& modules,
               PluginManager* pluginManager,
               ProcessSupervisor* processSupervisor,
               StyleManager* styleManager,
               QWidget* parent = nullptr);
    ~MainWindow() override;

    void attachInProcessUiModules();
    void releaseInProcessUiModules();

    bool loadLayoutFile(const QString& filePath,
                        QString* errorMessage = nullptr,
                        QStringList* unavailableModuleIds = nullptr);
    LayoutManager* layoutManager() const;

private slots:
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
    void createActions();
    void createModuleDocks();
    void setUiAvailable(const QString& moduleId, bool available);
    void showModule(const QString& moduleId);
    void updateStatusSummary();
    void reportStateFailure(const QString& title, const QString& detail);
    QString displayName(const QString& moduleId) const;
    QString placeholderText(const QString& moduleId, const QString& detail) const;

    QVector<ModuleConfig> modules_;
    QHash<QString, ModuleConfig> modulesById_;
    PluginManager* pluginManager_;
    ProcessSupervisor* processSupervisor_;
    StyleManager* styleManager_;
    LayoutManager* layoutManager_;
    ModuleManagerDialog* moduleManagerDialog_;
    QHash<QString, ManagedDockWidget*> moduleDocks_;
    QHash<QString, ProcessWindowHost*> processHosts_;
    QHash<QString, QAction*> moduleActions_;
    QHash<QString, bool> uiAvailable_;
    QHash<QString, QString> moduleStates_;
    QLabel* statusSummaryLabel_;
    QAction* saveLayoutAction_;
};
}
