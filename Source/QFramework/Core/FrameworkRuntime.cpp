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

// 主进程启动顺序：配置 -> Logger -> MessageBus -> 插件/子进程 -> UI/布局。
// 关闭顺序与依赖相反，确保日志最后停止，能够记录前面组件的退出诊断。

namespace qframework
{
// 只保存 QApplication 借用指针并初始化所有可选组件为空，实际资源在 initialize 创建。
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
    // shutdown 是幂等的，因此正常退出和异常早退都可以安全调用。
    shutdown();
}

// 按依赖顺序构建整个主进程运行时；基础设施失败立即返回，模块级失败记录为警告。
bool FrameworkRuntime::initialize(const QString& configFilePath,
                                  QString* errorMessage)
{
    // 禁止重复初始化，也不允许在没有 QApplication 的情况下创建 UI。
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
            config_.logging().flushIntervalMs,
            &error)) {
        if (errorMessage != nullptr)
            *errorMessage = error;
            return false;
    }
    // Logger 必须在创建 MessageBus、插件和子进程监督器之前启动，保证这些
    // 对象在初始化阶段产生的诊断也能进入同一个异步日志队列。
    loggerStarted_ = true;
    Logger::instance().installQtMessageHandler();

    messageBus_ = new MessageBus(config_.messageBus());
    // MessageBus 必须先于插件和监督器，因为两类模块都要注册到同一总线。
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
    // 样式更新由主进程广播给已连接的 UI 子进程。

    const QString styleFilePath = config_.style().file;
    if (!styleFilePath.isEmpty() &&
        !styleManager_->loadStyleSheet(styleFilePath, &error)) {
        appendStartupWarnings(
            QStringList() << QString::fromUtf8(u8"启动 QSS 加载失败：%1").arg(error));
    }

    QStringList moduleErrors;
    // 先启动主进程插件并挂接其 QWidget，再启动子进程模块。
    pluginManager_->loadAndStart(config_.modules(), &moduleErrors, false);
    appendStartupWarnings(moduleErrors);
    mainWindow_->attachInProcessUiModules();

    moduleErrors.clear();
    processSupervisor_->startAll(config_.modules(), &moduleErrors);
    appendStartupWarnings(moduleErrors);
    messageBus_->setDeliveryEnabled(true);
    // 所有模块完成注册后才统一放开消息投递，避免启动顺序造成丢消息。

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

// 显示已完成初始化的主窗口，并集中展示启动期间收集的非致命问题。
void FrameworkRuntime::show()
{
    // initialize 失败时不显示一个半初始化窗口。
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

// 按“停止消息生产者 -> 解除 UI -> 删除依赖 -> 最后停日志”的反向顺序关闭。
void FrameworkRuntime::shutdown()
{
    if (shutdownComplete_)
        return;
    shutdownComplete_ = true;

    // 先拒绝新消息并排空输入队列，再停止生产消息的模块。
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

    // 按照“使用者先删、被使用者后删”的顺序释放对象。
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
        // 先显式刷新，再 stop；即使最后一批日志还没到 100 ms 周期，也不会丢。
        Logger::instance().flush();
        Logger::instance().stop();
        loggerStarted_ = false;
    }
    initialized_ = false;
}

// 返回 MainWindow 借用指针，主要供入口和自动化测试访问。
MainWindow* FrameworkRuntime::mainWindow() const
{
    return mainWindow_;
}

// 过滤空警告，保存供 show() 展示，并立即写入集中日志保留诊断上下文。
void FrameworkRuntime::appendStartupWarnings(const QStringList& warnings)
{
    // 同一条警告既保留在日志中，也留给 show() 向用户集中展示。
    for (const QString& warning : warnings) {
        if (warning.isEmpty())
            continue;
        startupWarnings_.append(warning);
        Logger::instance().log(
            LogLevel::Error, QStringLiteral("QFrameworkApp"), warning);
    }
}
}
