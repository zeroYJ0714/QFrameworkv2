#include "FrameworkRuntime.h"

#include <QApplication>
#include <QDebug>
#include <QMessageBox>
#include <QMutex>
#include <QMutexLocker>
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
namespace
{
QMutex failFastMutex;
ProcessFailFast::Handler failFastHandler = nullptr;
}

bool ProcessFailFast::requestForHungModules(const QStringList& moduleIds)
{
    QStringList uniqueIds = moduleIds;
    uniqueIds.removeDuplicates();
    if (uniqueIds.isEmpty())
        return false;
    const QString reason = QString::fromUtf8(
        u8"应用退出时主进程模块消息回调未结束，无法安全卸载 DLL：%1")
        .arg(uniqueIds.join(QStringLiteral(", ")));
    Handler handler = nullptr;
    {
        QMutexLocker locker(&failFastMutex);
        handler = failFastHandler;
    }
    if (handler != nullptr) {
        handler(reason);
        return true;
    }
    // qFatal 是整个主进程的隔离边界；此路径只在应用本来已经退出时使用。
    qFatal("%s", qPrintable(reason));
    return true;
}

void ProcessFailFast::setHandlerForTests(Handler handler)
{
    QMutexLocker locker(&failFastMutex);
    failFastHandler = handler;
}

// 只保存 QApplication 借用指针并初始化所有可选组件为空，实际资源在 initialize 创建。
FrameworkRuntime::FrameworkRuntime(QApplication* application)
    : application_(application),
      messageBus_(nullptr),
      pluginManager_(nullptr),
      processSupervisor_(nullptr),
      styleManager_(nullptr),
      mainWindow_(nullptr),
      shownStartupWarningCount_(0),
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
    mainWindow_->setLayoutPresets(config_.layout().presets);
    QObject::connect(styleManager_,
                     &StyleManager::styleSheetChanged,
                     processSupervisor_,
                     &ProcessSupervisor::applyStyleSheet);
    QObject::connect(
        processSupervisor_,
        &ProcessSupervisor::startupBatchFinished,
        processSupervisor_,
        [this](const QStringList& errors) {
            appendStartupWarnings(errors);
            if (messageBus_ != nullptr)
                messageBus_->setDeliveryEnabled(true);
            showPendingStartupWarnings();
        });
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
    // 子进程启动已经改成异步批次；即时 errors 只说明请求未被接受，最终错误和
    // MessageBus 投递闸门都由 startupBatchFinished 在事件循环中统一处理。

    // 布局预设由 MainWindow 统一处理：启动只尝试 Layout.1，失败写日志并置灰菜单项，
    // 不进入全局启动警告对话框，也不改变当前空白窗口状态。
    mainWindow_->loadInitialLayoutPreset();

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
    showPendingStartupWarnings();
}

// 按“停止消息生产者 -> 解除 UI -> 删除依赖 -> 最后停日志”的反向顺序关闭。
void FrameworkRuntime::shutdown()
{
    if (shutdownComplete_)
        return;
    shutdownComplete_ = true;

    MessageBusStopReport stopReport;
    // 只在这里执行一次全局队列停止；PluginManager 和 MessageBus 析构不重复预算。
    if (messageBus_ != nullptr) {
        messageBus_->beginShutdown();
        stopReport = messageBus_->stopQueues(
            config_.messageBus().shutdownDrainTimeoutMs);
    }
    if (processSupervisor_ != nullptr)
        processSupervisor_->shutdown();
    if (mainWindow_ != nullptr)
        mainWindow_->releaseInProcessUiModules();
    QStringList quarantinedPluginIds;
    if (pluginManager_ != nullptr) {
        quarantinedPluginIds = pluginManager_->shutdown(
            stopReport.timedOutModuleIds);
    }

    QStringList unsafeModuleIds = quarantinedPluginIds;
    if (messageBus_ != nullptr)
        unsafeModuleIds.append(messageBus_->quarantinedModuleIds());
    unsafeModuleIds.removeDuplicates();

    // 按照“使用者先删、被使用者后删”的顺序释放对象。
    delete mainWindow_;
    mainWindow_ = nullptr;
    delete processSupervisor_;
    processSupervisor_ = nullptr;
    delete pluginManager_;
    pluginManager_ = nullptr;
    delete styleManager_;
    styleManager_ = nullptr;
    if (unsafeModuleIds.isEmpty()) {
        delete messageBus_;
        messageBus_ = nullptr;
    } else {
        // 仍在 onMessage 的线程会继续借用 MessageBus/endpoint；进程终止前故意保留。
        messageBus_ = nullptr;
    }

    if (loggerStarted_) {
        Logger::instance().uninstallQtMessageHandler();
        // 先显式刷新，再 stop；即使最后一批日志还没到 100 ms 周期，也不会丢。
        Logger::instance().flush();
        Logger::instance().stop();
        loggerStarted_ = false;
    }
    initialized_ = false;
    ProcessFailFast::requestForHungModules(unsafeModuleIds);
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
        if (startupWarnings_.contains(warning))
            continue;
        startupWarnings_.append(warning);
        Logger::instance().log(
            LogLevel::Error, QStringLiteral("QFrameworkApp"), warning);
    }
}

// 主窗口已经显示时补充呈现异步启动错误；用索引避免同一警告重复弹出。
void FrameworkRuntime::showPendingStartupWarnings()
{
    if (mainWindow_ == nullptr || !mainWindow_->isVisible() ||
        shownStartupWarningCount_ >= startupWarnings_.size()) {
        return;
    }
    const QStringList pending = startupWarnings_.mid(shownStartupWarningCount_);
    shownStartupWarningCount_ = startupWarnings_.size();
    QMessageBox::warning(mainWindow_,
                         QString::fromUtf8(u8"框架启动警告"),
                         pending.join(QLatin1Char('\n')));
}
}
