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
    /// @brief 构造完整标题栏：建立菜单栏和最小化/最大化/关闭三个窗口按钮并连接点击信号，最后把按钮状态同步为「未最大化」。
    /// @param parent 父控件，通常是 MainWindow；本控件和全部子控件都挂在它下面并随它销毁，可为空（为空时标题栏成为顶层窗口）。
    /// @return 无（构造函数不返回值）。
    explicit WindowTitleBar(QWidget* parent = nullptr);

    // MainWindow 把已经创建的 QAction 加到这个菜单栏，避免复制一套业务动作。
    /// @brief 返回内嵌菜单栏的借用指针；菜单项由 MainWindow 创建后挂载，标题栏只负责显示与布局。
    /// @return 本控件持有的 QMenuBar 指针，所有权属于标题栏，调用方只借用，不应 delete。
    QMenuBar* menuBar() const;

    // 判断标题栏某个 Qt 客户区坐标是否落在菜单或窗口按钮上。
    // 鼠标处理用它避免把菜单或按钮操作识别为窗口拖动。
    /// @brief 判断标题栏客户区坐标是否落在菜单栏或窗口按钮上，供标题栏鼠标处理保留交互控件的操作。
    /// @param titleBarPoint 已换算到标题栏客户区的坐标，以本控件左上角为原点。
    /// @return 落在菜单栏或按钮上返回 true；落在空白区或本控件矩形之外返回 false，后者按可拖动标题栏处理。
    bool isInteractiveAt(const QPoint& titleBarPoint) const;

    // MainWindow 收到 WindowStateChange 后调用，更新最大化/还原图标和提示文本。
    /// @brief 按窗口是否最大化刷新最大化按钮的图标、提示文本和无障碍名称；最小化与关闭按钮的图标固定不变。
    /// @param maximized 窗口当前是否最大化；true 表示该按钮接下来执行「还原」，false 表示执行「最大化」。
    /// @return 无。
    void updateWindowControlState(bool maximized);

signals:
    // 标题栏空白区按下时请求系统移动窗口。两个坐标供最大化状态恢复窗口时
    // 保持鼠标仍位于原来的标题栏位置，具体窗口操作仍由 MainWindow 完成。
    /// @brief 标题栏空白区按下左键并拖动超过系统阈值时发射，请求 MainWindow 执行系统窗口移动。
    /// @param globalPosition 触发时的鼠标全局坐标，供 MainWindow 发起窗口移动。
    /// @param titleBarPosition 左键按下时鼠标在标题栏内的坐标，最大化状态恢复窗口时用它保住鼠标相对标题栏的位置。
    /// @return 无。
    void moveRequested(const QPoint& globalPosition,
                       const QPoint& titleBarPosition);
    // 信号只描述用户意图，具体窗口状态变化由 MainWindow 的槽完成。
    /// @brief 用户点击最小化按钮时发射，请求 MainWindow 最小化窗口；本控件自身不改变窗口状态。
    /// @return 无。
    void minimizeRequested();
    /// @brief 点击最大化按钮或双击标题栏空白区时发射，请求 MainWindow 在最大化与还原之间切换。
    /// @return 无。
    void maximizeRestoreRequested();
    /// @brief 用户点击关闭按钮时发射，把关闭意图交给 MainWindow，由它走统一的保存/放弃/取消流程。
    /// @return 无。
    void closeRequested();

protected:
    // 菜单和按钮继续按普通 Qt 控件处理；只有标题栏空白区请求移动或双击切换状态。
    /// @brief 空白区按下左键时记录拖动起点（全局坐标与标题栏内坐标）；命中菜单或按钮则交回基类处理。
    /// @param event 鼠标按下事件；event 为空或按下的不是左键时直接交回基类。
    /// @return 无。
    void mousePressEvent(QMouseEvent* event) override;
    /// @brief 拖动距离超过 QApplication::startDragDistance() 时发出一次 moveRequested，同一次拖动不重复发射。
    /// @param event 鼠标移动事件，提供当前全局坐标用于和按下点比较。
    /// @return 无。
    void mouseMoveEvent(QMouseEvent* event) override;
    /// @brief 松开左键时清除「已按下」和「已启动」标记，避免后续移动误触发窗口移动。
    /// @param event 鼠标松开事件；只对左键生效。
    /// @return 无。
    void mouseReleaseEvent(QMouseEvent* event) override;
    /// @brief 空白区双击时发出 maximizeRestoreRequested，补回无边框窗口丢失的原生标题栏行为。
    /// @param event 鼠标双击事件；命中菜单或按钮时不切换窗口状态。
    /// @return 无。
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    // 菜单/按钮以外的标题栏子控件仍可作为拖动空白区。
    /// @brief 沿父链判断命中的控件是否属于菜单栏或按钮，据此决定是否保留鼠标命中。
    /// @param child childAt() 命中的子控件，坐标落在空白处时可为空。
    /// @return 该控件或其任一祖先为 QMenuBar / QAbstractButton 时返回 true；child 为空或沿父链走到本控件仍未命中时返回 false。
    bool isInteractiveTitleBarChild(QWidget* child) const;
    bool movePressed_ = false; ///< 左键是否已在标题栏空白区按下且尚未松开；为 true 才允许后续移动触发窗口拖动。
    bool moveStarted_ = false; ///< 本次按下是否已经发出过 moveRequested，保证一次拖动只请求一次窗口移动。
    QPoint pressGlobalPosition_; ///< 按下左键时的鼠标全局坐标，用于判断移动是否超过系统拖动阈值。
    QPoint pressTitleBarPosition_; ///< 按下左键时鼠标在标题栏内的坐标，随 moveRequested 发出，供恢复窗口时复位鼠标位置。

    QMenuBar* menuBar_; ///< 左侧菜单栏，由 MainWindow 挂载模块 QAction；父对象是 this，随标题栏一起销毁。
    QToolButton* minimizeButton_; ///< 最小化按钮，点击转发 minimizeRequested，图标固定。
    QToolButton* maximizeButton_; ///< 最大化/还原按钮，图标与提示由 updateWindowControlState() 按窗口状态切换。
    QToolButton* closeButton_; ///< 关闭按钮，点击转发 closeRequested，图标固定。
};
}
