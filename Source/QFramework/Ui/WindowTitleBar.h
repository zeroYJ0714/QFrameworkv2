#pragma once

// 文件职责：提供 QFramework 无边框主窗口的客户区标题栏。
//
// WindowTitleBar 只管理可见的 Qt 控件和用户操作信号：左侧是菜单栏，右侧是三个
// 窗口按钮。它不直接操作 MainWindow 的 Dock、模块或进程，原因是这些
// 业务对象仍由 MainWindow 统一拥有和协调；按钮点击只通过 signal 把请求交回上层。

#include <QPoint>
#include <QWidget>

#include "QFrameworkGlobal.h"

class QMenuBar;
class QToolButton;

namespace qframework
{
class QFRAMEWORK_EXPORT WindowTitleBar : public QWidget
{
    Q_OBJECT

public:
    // parent 是 MainWindow。Qt 的父子对象关系会自动释放标题栏及其所有子控件。
    explicit WindowTitleBar(QWidget* parent = nullptr);

    // MainWindow 把已经创建的 QAction 加到这个菜单栏，避免复制一套业务动作。
    QMenuBar* menuBar() const;

    // 判断标题栏某个 Qt 客户区坐标是否落在菜单或窗口按钮上。
    // MainWindow 的 Windows 命中测试用它区分 HTCLIENT 和 HTCAPTION。
    bool isInteractiveAt(const QPoint& titleBarPoint) const;

    // MainWindow 收到 WindowStateChange 后调用，更新最大化/还原图标和提示文本。
    void updateWindowControlState(bool maximized);

signals:
    // 信号只描述用户意图，具体窗口状态变化由 MainWindow 的槽完成。
    void minimizeRequested();
    void maximizeRestoreRequested();
    void closeRequested();

private slots:
    // 把 QToolButton::clicked 转换成无参数的业务请求信号，避免标题栏知道窗口实现。
    void onMinimizeButtonClicked();
    void onMaximizeButtonClicked();
    void onCloseButtonClicked();

private:
    // 菜单/按钮以外的标题栏子控件仍可作为拖动空白区。
    bool isInteractiveTitleBarChild(QWidget* child) const;

    QMenuBar* menuBar_;
    QToolButton* minimizeButton_;
    QToolButton* maximizeButton_;
    QToolButton* closeButton_;
};
}
