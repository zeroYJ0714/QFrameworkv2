#pragma once

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

class QFRAMEWORK_EXPORT FrameworkRuntime
{
public:
    explicit FrameworkRuntime(QApplication* application);
    ~FrameworkRuntime();

    bool initialize(const QString& configFilePath,
                    QString* errorMessage = nullptr);
    void show();
    void shutdown();

    MainWindow* mainWindow() const;

private:
    void appendStartupWarnings(const QStringList& warnings);

    QApplication* application_;
    FrameworkConfig config_;
    MessageBus* messageBus_;
    PluginManager* pluginManager_;
    ProcessSupervisor* processSupervisor_;
    StyleManager* styleManager_;
    MainWindow* mainWindow_;
    QStringList startupWarnings_;
    bool initialized_;
    bool loggerStarted_;
    bool shutdownComplete_;
};
}
