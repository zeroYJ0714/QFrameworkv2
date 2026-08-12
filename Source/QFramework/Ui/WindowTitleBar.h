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
class QMouseEvent;
class QToolButton;

namespace qframework
{
class QFRAMEWORK_EXPORT WindowTitleBar : public QWidget
{
    Q_OBJECT

public:
    // parent 是 MainWindow。Qt 的父子对象关系会自动释放标题栏及其所有子控件。
    /// @brief 创建 WindowTitleBar 对象并初始化其基础状态。
    /// @param parent Qt 父对象或父控件；用于建立所有权关系，允许为空。
    /// @return 无（构造函数不返回值）。
    explicit WindowTitleBar(QWidget* parent = nullptr);

    // MainWindow 把已经创建的 QAction 加到这个菜单栏，避免复制一套业务动作。
    /// @brief 执行 `menuBar` 所定义的类职责。
    /// @return 返回借用指针；找不到目标时返回 nullptr。
    QMenuBar* menuBar() const;

    // 判断标题栏某个 Qt 客户区坐标是否落在菜单或窗口按钮上。
    // MainWindow 的 Windows 命中测试用它区分 HTCLIENT 和 HTCAPTION。
    /// @brief 检查 `isInteractiveAt` 对应的框架操作。
    /// @param titleBarPoint 传给该操作的 `titleBarPoint` 参数；取值应符合声明类型和函数用途。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool isInteractiveAt(const QPoint& titleBarPoint) const;

    // MainWindow 收到 WindowStateChange 后调用，更新最大化/还原图标和提示文本。
    /// @brief 更新 `updateWindowControlState` 对应的框架操作。
    /// @param maximized 传给该操作的 `maximized` 参数；取值应符合声明类型和函数用途。
    /// @return 无。
    void updateWindowControlState(bool maximized);

signals:
    // 标题栏空白区按下时请求系统移动窗口。两个坐标供最大化状态恢复窗口时
    // 保持鼠标仍位于原来的标题栏位置，具体窗口操作仍由 MainWindow 完成。
    /// @brief 执行 `moveRequested` 所定义的类职责。
    /// @param globalPosition 鼠标全局坐标。
    /// @param titleBarPosition 鼠标在标题栏内的坐标。
    /// @return 无。
    void moveRequested(const QPoint& globalPosition,
                       const QPoint& titleBarPosition);
    // 信号只描述用户意图，具体窗口状态变化由 MainWindow 的槽完成。
    /// @brief 执行 `minimizeRequested` 所定义的类职责。
    /// @return 无。
    void minimizeRequested();
    /// @brief 执行 `maximizeRestoreRequested` 所定义的类职责。
    /// @return 无。
    void maximizeRestoreRequested();
    /// @brief 关闭 `closeRequested` 对应的框架操作。
    /// @return 无。
    void closeRequested();

private slots:
    // 把 QToolButton::clicked 转换成无参数的业务请求信号，避免标题栏知道窗口实现。
    /// @brief 处理 `onMinimizeButtonClicked` 对应的框架操作。
    /// @return 无。
    void onMinimizeButtonClicked();
    /// @brief 处理 `onMaximizeButtonClicked` 对应的框架操作。
    /// @return 无。
    void onMaximizeButtonClicked();
    /// @brief 处理 `onCloseButtonClicked` 对应的框架操作。
    /// @return 无。
    void onCloseButtonClicked();

protected:
    // 菜单和按钮继续按普通 Qt 控件处理；只有标题栏空白区请求移动或双击切换状态。
    /// @brief 执行 `mousePressEvent` 所定义的类职责。
    /// @param event Qt 事件对象；仅在当前回调期间有效。
    /// @return 无。
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    /// @brief 执行 `mouseDoubleClickEvent` 所定义的类职责。
    /// @param event Qt 事件对象；仅在当前回调期间有效。
    /// @return 无。
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    // 菜单/按钮以外的标题栏子控件仍可作为拖动空白区。
    /// @brief 检查 `isInteractiveTitleBarChild` 对应的框架操作。
    /// @param child 传给该操作的 `child` 参数；取值应符合声明类型和函数用途。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool isInteractiveTitleBarChild(QWidget* child) const;
    bool movePressed_ = false;
    bool moveStarted_ = false;
    QPoint pressGlobalPosition_;
    QPoint pressTitleBarPosition_;

    QMenuBar* menuBar_; ///< `menuBar_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QToolButton* minimizeButton_; ///< `minimizeButton_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QToolButton* maximizeButton_; ///< `maximizeButton_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QToolButton* closeButton_; ///< `closeButton_` 对应的对象指针；所有权和线程归属见本类文件级说明。
};
}
