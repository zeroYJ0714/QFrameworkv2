// 【阅读导引】提供受控停靠面板。
// 阅读顺序：Qt 负责基本停靠；本类把关闭操作转成 closeRequested，让主窗口决定隐藏和关闭门禁。
// 需要分清：面板关闭不等于卸载 DLL，也不等于终止设备线程。不要在这里放某个业务插件的保存或录制逻辑。
// 这是执行实现：按入口调用和信号回调追踪，不要按文件行号把所有函数理解成连续执行。

// 文件职责：提供带统一标题栏/可见意图的 QDockWidget 包装。
// 对象归 GUI 主窗口所有；关闭按钮只改变 requestedVisible/隐藏状态，不销毁插件和业务线程。
// Dock ready、标签页切换、布局恢复和模块菜单都通过 MainWindow 的 Qt 信号槽协调，避免在
// QDockWidget 事件中直接访问 MessageBus 或跨线程模块对象。
#include "ManagedDockWidget.h"

#include <QCloseEvent>
#include <QMainWindow>

// 统一构造设置，避免每个模块 Dock 自行选择不同 features。

namespace qframework
{
// 创建一个统一的可停靠模块容器；关闭仅代表隐藏，不会销毁模块对象。
/// @brief 创建一个统一的可停靠模块容器；关闭仅表示隐藏，不销毁模块对象。
/// @param title Dock 标题，显示在标题栏和标签页上。
/// @param mainWindow 所属主窗口，同时作为本 Dock 的 QObject 父对象。
/// @return 无（构造函数不返回值）。
ManagedDockWidget::ManagedDockWidget(const QString& title,
                                     QMainWindow* mainWindow)
    : QDockWidget(title, mainWindow)
{
    // 允许四个停靠区域；Closable 表示“隐藏”，不会停止模块生命周期。
    setAllowedAreas(Qt::AllDockWidgetAreas);
    setFeatures(QDockWidget::DockWidgetClosable |
                 QDockWidget::DockWidgetMovable);
}

// QDockWidget 的 visibilityChanged(false) 也可能来自标签切换，不能代表用户关闭。
/// @brief 拦截用户关闭动作，改为发出 closeRequested 交给主窗口裁决。
/// @param event 关闭事件；默认先拒绝，主窗口已隐藏时才接受。
/// @return 无。
void ManagedDockWidget::closeEvent(QCloseEvent* event)
{
    // 先拒绝 QDockWidget 自己的立即关闭。MainWindow 收到同步信号后会询问
    // 对应 UI 模块；只有模块同意，统一可见性入口才真正 hide 当前 Dock。
    event->ignore();
    emit closeRequested();
    // closeRequested 使用同线程直连。允许关闭时 MainWindow 已经 hide，重新接受
    // 事件可保持 QWidget::close() 的 true 返回契约；veto 时仍可见并保持 ignore。
    if (!isVisible())
        event->accept();
}
}
