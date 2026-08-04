#include "FrameworkRuntime.h"

#include <QApplication>
#include <QMessageBox>
#include <QObject>

#include "../Process/ProcessSupervisor.h"
#include "../Ui/MainWindow.h"
#include "../Ui/StyleManager.h"
#include "Logger.h"
#include "MessageBus.h"
#include "PluginManager.h"

namespace qframework
{
FrameworkRuntime::FrameworkRuntime(QApplication* application)
    : application_(application),
      messageBus_(nullptr),
      pluginManager_(nullptr),
      processSupervisor_(nullptr),
      styleManager_(nullptr),
      mainWindow_(nullptr),
      initialized_(false),
      loggerStarted_(false),
      shutdownComplete_(false)
{
}

FrameworkRuntime::~FrameworkRuntime()
{
    shutdown();
}

bool FrameworkRuntime::initialize(const QString& configFilePath,
                                  QString* errorMessage)
{
    if (initialized_ || application_ == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = initialized_
                ? QString::fromUtf8(u8"框架已经初始化")
                : QString::fromUtf8(u8"QApplication 不可用");
        }
        return false;
    }

    QString error;
    if (!config_.load(configFilePath, &error)) {
        if (errorMessage != nullptr)
            *errorMessage = error;
        return false;
    }
    if (!Logger::instance().start(
            config_.logging().directory,
            config_.logging().maxFileBytes,
            &error)) {
        if (errorMessage != nullptr)
            *errorMessage = error;
        return false;
    }
    loggerStarted_ = true;
    Logger::instance().installQtMessageHandler();

    messageBus_ = new MessageBus(config_.messageBus());
    pluginManager_ = new PluginManager(messageBus_);
    processSupervisor_ = new ProcessSupervisor(
        messageBus_, config_.messageBus(), config_.process());
    styleManager_ = new StyleManager;
    mainWindow_ = new MainWindow(
        config_.modules(), pluginManager_, processSupervisor_, styleManager_);
    QObject::connect(styleManager_,
                     &StyleManager::styleSheetChanged,
                     processSupervisor_,
                     &ProcessSupervisor::applyStyleSheet);

    const QString styleFilePath = config_.style().file;
    if (!styleFilePath.isEmpty() &&
        !styleManager_->loadStyleSheet(styleFilePath, &error)) {
        appendStartupWarnings(
            QStringList() << QString::fromUtf8(u8"启动 QSS 加载失败：%1").arg(error));
    }

    QStringList moduleErrors;
    pluginManager_->loadAndStart(config_.modules(), &moduleErrors, false);
    appendStartupWarnings(moduleErrors);
    mainWindow_->attachInProcessUiModules();

    moduleErrors.clear();
    processSupervisor_->startAll(config_.modules(), &moduleErrors);
    appendStartupWarnings(moduleErrors);
    messageBus_->setDeliveryEnabled(true);

    const QString layoutFilePath = config_.layout().startupFile;
    if (!layoutFilePath.isEmpty()) {
        QStringList unavailableModules;
        error.clear();
        if (!mainWindow_->loadLayoutFile(
                layoutFilePath, &error, &unavailableModules)) {
            appendStartupWarnings(
                QStringList() << QString::fromUtf8(u8"启动布局加载失败：%1").arg(error));
        } else if (!unavailableModules.isEmpty()) {
            Logger::instance().log(
                LogLevel::Warning,
                QStringLiteral("QFrameworkApp"),
                QString::fromUtf8(u8"启动布局跳过不可用模块：%1")
                    .arg(unavailableModules.join(QStringLiteral(", "))));
        }
    }

    initialized_ = true;
    return true;
}

void FrameworkRuntime::show()
{
    if (!initialized_ || mainWindow_ == nullptr)
        return;
    mainWindow_->show();
    if (!startupWarnings_.isEmpty()) {
        QMessageBox::warning(
            mainWindow_,
            QString::fromUtf8(u8"框架启动警告"),
            startupWarnings_.join(QLatin1Char('\n')));
    }
}

void FrameworkRuntime::shutdown()
{
    if (shutdownComplete_)
        return;
    shutdownComplete_ = true;

    if (messageBus_ != nullptr) {
        messageBus_->beginShutdown();
        messageBus_->stopQueues(config_.messageBus().shutdownDrainTimeoutMs);
    }
    if (processSupervisor_ != nullptr)
        processSupervisor_->shutdown();
    if (mainWindow_ != nullptr)
        mainWindow_->releaseInProcessUiModules();
    if (pluginManager_ != nullptr)
        pluginManager_->shutdown(config_.messageBus().shutdownDrainTimeoutMs);

    delete mainWindow_;
    mainWindow_ = nullptr;
    delete processSupervisor_;
    processSupervisor_ = nullptr;
    delete pluginManager_;
    pluginManager_ = nullptr;
    delete styleManager_;
    styleManager_ = nullptr;
    delete messageBus_;
    messageBus_ = nullptr;

    if (loggerStarted_) {
        Logger::instance().uninstallQtMessageHandler();
        Logger::instance().flush();
        Logger::instance().stop();
        loggerStarted_ = false;
    }
    initialized_ = false;
}

MainWindow* FrameworkRuntime::mainWindow() const
{
    return mainWindow_;
}

void FrameworkRuntime::appendStartupWarnings(const QStringList& warnings)
{
    for (const QString& warning : warnings) {
        if (warning.isEmpty())
            continue;
        startupWarnings_.append(warning);
        Logger::instance().log(
            LogLevel::Error, QStringLiteral("QFrameworkApp"), warning);
    }
}
}
