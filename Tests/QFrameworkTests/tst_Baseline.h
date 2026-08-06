#pragma once

// 文件职责：QFramework 的 Qt Test 基线。
// 测试既覆盖纯值对象，也会启动真实插件/子进程，验证从配置到 UI/IPC 的完整链路。

#include <QtTest>

class BaselineTest : public QObject
{
    Q_OBJECT

private slots:
    // 检查 Qt、框架和 Protobuf 版本基线。
    void qtAndFrameworkVersions();
    // 检查绑定前、运行中和停止后的模块发布门禁。
    void moduleLifecycleDefaults();
    // 检查 Protobuf 消息的序列化/反序列化往返。
    void protobufRoundTrip();
    // 检查只读 INI、相对路径和强类型配置解析。
    void configIsReadOnlyAndResolvesPaths();
    // 检查日志滚动、Qt 全局消息捕获和输出格式。
    void loggerRollsAndCapturesQtMessages();
    // 检查主进程消息总线的顺序、容量、大小和发布权限。
    void messageBusPoliciesAndOrdering();
    // 检查插件元数据、加载顺序、禁用项和故障隔离。
    void pluginLoaderAndModuleIntegration();
    // 检查长度前缀 JSON 帧的半包、完整包和超限错误。
    void processProtocolFraming();
    // 检查真实子进程注册、心跳、窗口嵌入和正常停止。
    void processIpcAndSupervision();
    // 检查 Latest 只覆盖同主题最旧等待帧。
    void processLatestQueueOverwritesOldFrames();
    // 检查 Reliable 容量满时拒绝新消息且绝不覆盖旧值。
    void processReliableQueueDoesNotOverwrite();
    // 检查父到子、子到父以及 inline/shared 两种传输方式。
    void processIpcInlineSharedBidirectional();
    // 检查错误令牌被认证阶段拒绝。
    void processRejectsInvalidToken();
    // 检查连接后未注册会在有限期限内失败。
    void processRegistrationTimeout();
    // 检查不回应 ping 的进程会触发心跳超时。
    void processHeartbeatTimeout();
    // 检查调试器等待阶段不会被普通心跳误杀。
    void processDebuggerWaitDefersHeartbeat();
    void processRestartRequestDoesNotBlockGui();
    void processRejectsDuplicateRestartRequest();
    void processStopClosesIngressImmediately();
    void processDeadlinesAreIndependentPerEntry();
    void processStopAckAdvancesState();
    void processShutdownUsesOneTotalDeadline();
    void processStartupBatchCompletesAsynchronously();
    void messageBusStopTimeoutIsBounded();
    void timedOutQueueIsNotDeletedOrUnloaded();
    void processHungCallbackEndsWholeChild();
    void shutdownRequestsFailFastForHungInProcessModule();
    void messageQueueNeverUsesThreadTerminate();
    void processPublishGateIsThreadSafeDuringStop();
    void processControlWriteFailureTriggersFaultImmediately();
    void processStopAccountingIncludesDiscardedMessages();
    void processRestartWindowBoundaryIsInclusive();
    void processSocketAccessStaysOnRuntimeThread();
    // 检查同一部署目录只能持有一个实例互斥量。
    void singleInstancePerDirectory();
    // 检查 Dock 布局保存、恢复和不可用模块处理。
    void layoutPersistenceAndDockingRules();
    // 检查加载布局后“保存当前”直接覆盖活动文件，不进入另存为。
    void saveCurrentLayoutOverwritesLoadedFile();
    // 检查没有活动布局时“保存当前”才转入另存为对话框。
    void saveCurrentLayoutFallsBackToSaveAs();
    void moduleMenuTogglesRequestedVisibility();
    void dockCloseClearsRequestedVisibility();
    // 检查无边框主窗口最大化后，主进程 UI Dock 的标题栏关闭按钮仍可点击。
    void inProcessDockTitleBarControlsAfterMaximize();
    void tabSwitchKeepsRequestedVisibility();
    void layoutPreservesTabbedVisibilityIntent();
    void layoutDefersUnavailableDockUntilReady();
    void layoutHiddenDockStaysHiddenAfterReady();
    void processUiLateWindowHonorsLayoutVisibility();
    // 检查上下拆分 Dock 后无需缩放主窗口，子进程客户区就能立即贴合宿主。
    void processUiResizeSurvivesVerticalDockRelayout();
    void legacyLayoutLoadsWithoutCorruptingCurrentState();
    // 检查 QSS 重载和失败时保留旧样式。
    void styleSheetReloadAndFailureRecovery();
    // 检查无边框标题栏复用三组菜单，并把窗口按钮请求交回 MainWindow。
    void framelessTitleBarAndWindowControls();
    // 检查模块管理操作列的图标按钮完整落在单元格内。
    void moduleManagerActionButtonsFit();
    // 检查 TechDashboard 的 QMenuBar 状态和 QMainWindow Dock 分隔条规则。
    void techDashboardMenuAndDockStyle();
    // 检查默认/非法配置回退 100 ms，并由后台线程自动落盘。
    void loggerFlushesAtConfiguredInterval();
    // 检查显式 flush 和 stop 都会写出尚未到周期的尾部日志。
    void loggerExplicitFlushAndStopNoLoss();
};
