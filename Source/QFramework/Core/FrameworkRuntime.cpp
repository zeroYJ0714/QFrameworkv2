// 【阅读导引】主程序的装配和收尾入口。
// 阅读顺序：从 initialize 看配置、Logger、MessageBus、PluginManager、MainWindow 的创建顺序，再看 show 和 shutdown。
// 需要分清：模块故障可以作为启动警告；配置等基础设施失败才让初始化返回 false。退出要先停消息再释放对象，防止回调访问已销毁插件。
// 这是执行实现：按入口调用和信号回调追踪，不要按文件行号把所有函数理解成连续执行。

// 文件职责：组装 QFramework 主进程的核心对象，包括配置、日志、MessageBus、插件管理
// 和子进程监督器，并推进启动/停止状态机。FrameworkRuntime 通常由 GUI 主线程拥有；
// 它不越过对象线程边界直接访问插件 QWidget，关闭时按固定顺序停止消息入口、插件和进程。
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
QMutex failFastMutex; ///< 保护 failFastHandler 的互斥锁，供测试替换与崩溃路径读取。
ProcessFailFast::Handler failFastHandler = nullptr; ///< 可被测试替换的 fail-fast 处理器；为空时直接调用 qFatal。
}

/// @brief 在模块消息回调无法结束时触发 fail-fast，阻止继续卸载仍被使用的 DLL。
/// @param moduleIds 退出时仍未结束回调的模块 ID 列表。
/// @return 始终返回 true；正常路径不会返回，处理器或 qFatal 会终止进程。
bool ProcessFailFast::requestForHungModules(const QStringList& moduleIds)
{
    QStringList uniqueIds = moduleIds; ///< 去重后的模块 ID 列表，用于生成可读原因。
    uniqueIds.removeDuplicates();
    if (uniqueIds.isEmpty())
        return false;
    const QString reason = QString::fromUtf8(
        u8"应用退出时主进程模块消息回调未结束，无法安全卸载 DLL：%1")
        .arg(uniqueIds.join(QStringLiteral(", "))); ///< 面向用户的终止原因说明。
    Handler handler = nullptr; ///< 锁内取得的处理器副本，锁外再调用以免自锁。
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

/// @brief 替换 fail-fast 处理器，仅供测试注入可控的终止行为。
/// @param handler 新的处理器；传空表示恢复为直接 qFatal。
/// @return 无。
void ProcessFailFast::setHandlerForTests(Handler handler)
{
    QMutexLocker locker(&failFastMutex);
    failFastHandler = handler;
}

// 只保存 QApplication 借用指针并初始化所有可选组件为空，实际资源在 initialize 创建。
/// @brief 构造运行时，仅借用 QApplication 并把各可选组件置空。
/// @param application 主 GUI 线程的 QApplication，所有权仍在调用方。
/// @return 无（构造函数不返回值）。
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

/// @brief 析构时执行一次幂等的 shutdown，保证异常早退也能回收资源。
/// @return 无（析构函数不返回值）。
FrameworkRuntime::~FrameworkRuntime()
{
    // shutdown 是幂等的，因此正常退出和异常早退都可以安全调用。
    shutdown();
}

// 按依赖顺序构建整个主进程运行时；基础设施失败立即返回，模块级失败记录为警告。
/// @brief 按依赖顺序创建配置、日志、消息总线、插件、子进程监督器和主窗口。
/// @param configFilePath 只读 INI 配置文件的绝对路径。
/// @param errorMessage 可选的失败原因输出参数，可为空。
/// @return 基础设施初始化成功返回 true；配置、日志或前置状态无效时返回 false。
bool FrameworkRuntime::initialize(const QString& configFilePath,
                                  QString* errorMessage)
{
    // 禁止重复初始化，也不允许在没有 QApplication 的情况下创建 UI。
    if (initialized_ || application_ == nullptr) { ///< 重复初始化或缺少 QApplication：直接失败。
        if (errorMessage != nullptr) {
            *errorMessage = initialized_
                ? QString::fromUtf8(u8"框架已经初始化")
                : QString::fromUtf8(u8"QApplication 不可用");
        }
        return false;
    }

    QString error; ///< 承接各初始化步骤的失败原因。
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
        [this](const QStringList& errors) { ///< 子进程启动批次完成回调：先收警告再放行消息投递。
            appendStartupWarnings(errors);
            if (messageBus_ != nullptr)
                messageBus_->setDeliveryEnabled(true);
            showPendingStartupWarnings();
        });
    // 样式更新由主进程广播给已连接的 UI 子进程。

    const QString styleFilePath = config_.style().file; ///< 全局 QSS 样式表路径，允许为空表示不加载。
    if (!styleFilePath.isEmpty() &&
        !styleManager_->loadStyleSheet(styleFilePath, &error)) { ///< 样式加载失败只记为启动警告，不阻断启动。
        appendStartupWarnings(
            QStringList() << QString::fromUtf8(u8"启动 QSS 加载失败：%1").arg(error));
    }

    QStringList moduleErrors; ///< 承接插件加载/启动阶段的模块级错误。
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
/// @brief 显示主窗口并弹出此前收集的启动警告。
/// @return 无；未完成初始化或主窗口为空时安全忽略。
void FrameworkRuntime::show()
{
    // initialize 失败时不显示一个半初始化窗口。
    if (!initialized_ || mainWindow_ == nullptr)
        return;
    mainWindow_->show();
    showPendingStartupWarnings();
}

// 按“停止消息生产者 -> 解除 UI -> 删除依赖 -> 最后停日志”的反向顺序关闭。
/// @brief 按依赖逆序停止消息、插件、子进程与日志，并释放全部运行时对象。
/// @return 无；重复调用因 shutdownComplete_ 门禁而直接返回。
void FrameworkRuntime::shutdown()
{
    if (shutdownComplete_)
        return;
    shutdownComplete_ = true;

    MessageBusStopReport stopReport; ///< 全局队列停止结果，用于识别超时模块。
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
    QStringList quarantinedPluginIds; ///< PluginManager 判定需隔离保留的模块 ID。
    if (pluginManager_ != nullptr) {
        quarantinedPluginIds = pluginManager_->shutdown(
            stopReport.timedOutModuleIds);
    }

    QStringList unsafeModuleIds = quarantinedPluginIds; ///< 合并后的不安全模块 ID，决定是否保留 MessageBus。
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
    if (unsafeModuleIds.isEmpty()) { ///< 无隔离模块：可以安全销毁消息总线。
        delete messageBus_;
        messageBus_ = nullptr;
    } else {
        // 仍在 onMessage 的线程会继续借用 MessageBus/endpoint；进程终止前故意保留。
        messageBus_ = nullptr;
    }

    if (loggerStarted_) { ///< 日志已启动：先恢复 Qt 处理器再冲刷并停止 Worker。
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
/// @brief 返回主窗口借用指针，主要供入口和自动化测试访问。
/// @return 已创建的主窗口指针；未初始化时可能为 nullptr。
MainWindow* FrameworkRuntime::mainWindow() const
{
    return mainWindow_;
}

// 过滤空警告，保存供 show() 展示，并立即写入集中日志保留诊断上下文。
/// @brief 去重并收集启动警告，同时写入集中日志。
/// @param warnings 待处理的启动警告文本列表，允许含空项。
/// @return 无。
void FrameworkRuntime::appendStartupWarnings(const QStringList& warnings)
{
    // 同一条警告既保留在日志中，也留给 show() 向用户集中展示。
    for (const QString& warning : warnings) { ///< warning：当前待过滤的单条警告文本。
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
/// @brief 弹出尚未展示过的启动警告，并用计数避免重复提示。
/// @return 无；主窗口未显示或没有新警告时安全忽略。
void FrameworkRuntime::showPendingStartupWarnings()
{
    if (mainWindow_ == nullptr || !mainWindow_->isVisible() ||
        shownStartupWarningCount_ >= startupWarnings_.size()) { ///< 窗口不可见或已全部展示过：无需弹窗。
        return;
    }
    const QStringList pending = startupWarnings_.mid(shownStartupWarningCount_); ///< 本次尚未展示的警告子列表。
    shownStartupWarningCount_ = startupWarnings_.size();
    QMessageBox::warning(mainWindow_,
                         QString::fromUtf8(u8"框架启动警告"),
                         pending.join(QLatin1Char('\n')));
}
}
