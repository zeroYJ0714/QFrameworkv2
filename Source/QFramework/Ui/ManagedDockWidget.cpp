#include "ManagedDockWidget.h"

#include <QCloseEvent>
#include <QMainWindow>

// 统一构造设置，避免每个模块 Dock 自行选择不同 features。

namespace qframework
{
// 创建一个统一的可停靠模块容器；关闭仅代表隐藏，不会销毁模块对象。
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
