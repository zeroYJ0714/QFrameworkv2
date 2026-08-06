#include "WindowTitleBar.h"

#include <QAbstractButton>
#include <QHBoxLayout>
#include <QMenuBar>
#include <QMouseEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QToolButton>

namespace qframework
{
// 构造完整标题栏：先建立 Qt 子控件和固定槽位，再把按钮点击转换成上层 signal。
// 所有控件都以 this 为 parent，MainWindow 销毁 WindowTitleBar 时 Qt 会按父子关系
// 自动释放布局、菜单栏、标签和按钮，不需要手工 delete。
WindowTitleBar::WindowTitleBar(QWidget* parent)
    : QWidget(parent),
      menuBar_(new QMenuBar(this)),
      minimizeButton_(new QToolButton(this)),
      maximizeButton_(new QToolButton(this)),
      closeButton_(new QToolButton(this))
{
    setObjectName(QStringLiteral("QFrameworkTitleBar"));
    setProperty("role", QStringLiteral("topbar"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(38);

    menuBar_->setObjectName(QStringLiteral("QFrameworkTitleMenuBar"));
    menuBar_->setNativeMenuBar(false);
    menuBar_->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);

    const QList<QToolButton*> windowButtons =
        QList<QToolButton*>() << minimizeButton_ << maximizeButton_ << closeButton_;
    for (QToolButton* button : windowButtons) {
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::StrongFocus);
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setFixedSize(46, 38);
    }
    minimizeButton_->setObjectName(QStringLiteral("WindowMinimizeButton"));
    maximizeButton_->setObjectName(QStringLiteral("WindowMaximizeButton"));
    closeButton_->setObjectName(QStringLiteral("WindowCloseButton"));

    minimizeButton_->setAccessibleName(QString::fromUtf8(u8"最小化"));
    maximizeButton_->setAccessibleName(QString::fromUtf8(u8"最大化"));
    closeButton_->setAccessibleName(QString::fromUtf8(u8"关闭"));
    minimizeButton_->setToolTip(QString::fromUtf8(u8"最小化"));
    maximizeButton_->setToolTip(QString::fromUtf8(u8"最大化"));
    closeButton_->setToolTip(QString::fromUtf8(u8"关闭"));

    QHBoxLayout* layout = new QHBoxLayout(this);
    // 右侧保留 6 个逻辑像素空白，对应 nativeEvent() 的最小缩放命中区，
    // 避免关闭按钮最右侧几像素同时被 Windows 误判为 HTRIGHT。
    layout->setContentsMargins(8, 0, 6, 0);
    layout->setSpacing(0);
    layout->addWidget(menuBar_);
    layout->addStretch(1);
    layout->addWidget(minimizeButton_);
    layout->addWidget(maximizeButton_);
    layout->addWidget(closeButton_);

    connect(minimizeButton_,
            &QToolButton::clicked,
            this,
            &WindowTitleBar::onMinimizeButtonClicked);
    connect(maximizeButton_,
            &QToolButton::clicked,
            this,
            &WindowTitleBar::onMaximizeButtonClicked);
    connect(closeButton_,
            &QToolButton::clicked,
            this,
            &WindowTitleBar::onCloseButtonClicked);

    updateWindowControlState(false);
}

QMenuBar* WindowTitleBar::menuBar() const
{
    return menuBar_;
}

// WM_NCHITTEST 会先把屏幕坐标换成标题栏客户区坐标，再调用此函数。
// 只有菜单和按钮需要保留 HTCLIENT；品牌文字等不可点击区域仍按“空白”处理，
// 让用户可以从视觉上连续的标题栏区域拖动窗口。
bool WindowTitleBar::isInteractiveAt(const QPoint& titleBarPoint) const
{
    if (!rect().contains(titleBarPoint))
        return false;
    return isInteractiveTitleBarChild(childAt(titleBarPoint));
}

// 标题栏是 Qt 客户区 QWidget。空白处按下后把位置交给 MainWindow，由它调用
// QWindow::startSystemMove()，从而保留 Windows 贴边、跨屏和系统拖动行为。
void WindowTitleBar::mousePressEvent(QMouseEvent* event)
{
    if (event != nullptr && event->button() == Qt::LeftButton &&
        !isInteractiveAt(event->pos())) {
        emit moveRequested(event->globalPos(), event->pos());
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

// Windows 原生标题栏通常支持双击最大化/还原；无边框后由这个 Qt 事件补回。
// 菜单和三个按钮属于交互控件，不会触发窗口状态切换。
void WindowTitleBar::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event != nullptr && event->button() == Qt::LeftButton &&
        !isInteractiveAt(event->pos())) {
        emit maximizeRestoreRequested();
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

// 最大化时使用 NormalButton 图标，普通状态使用 MaxButton 图标；窗口状态变化由
// MainWindow 的 QEvent::WindowStateChange 驱动，标题栏只负责更新自己的视觉状态。
void WindowTitleBar::updateWindowControlState(bool maximized)
{
    // 三个按钮都从当前 Qt style 取标准图标，避免依赖外部资源；只有中间按钮会
    // 根据窗口状态在“最大化”和“还原”之间切换，另外两个图标始终固定。
    minimizeButton_->setIcon(style()->standardIcon(QStyle::SP_TitleBarMinButton));
    closeButton_->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    const QStyle::StandardPixmap pixmap = maximized
        ? QStyle::SP_TitleBarNormalButton
        : QStyle::SP_TitleBarMaxButton;
    const QString accessibleName = maximized
        ? QString::fromUtf8(u8"还原")
        : QString::fromUtf8(u8"最大化");
    maximizeButton_->setIcon(style()->standardIcon(pixmap));
    maximizeButton_->setToolTip(accessibleName);
    maximizeButton_->setAccessibleName(accessibleName);
}

// 判断点击控件是否真的会处理鼠标。QMenuBar 和 QAbstractButton 的子控件返回
// true，普通 QLabel/标题栏本身返回 false，后者会被 MainWindow 交给 HTCAPTION。
bool WindowTitleBar::isInteractiveTitleBarChild(QWidget* child) const
{
    QWidget* current = child;
    while (current != nullptr && current != this) {
        if (qobject_cast<QMenuBar*>(current) != nullptr ||
            qobject_cast<QAbstractButton*>(current) != nullptr) {
            return true;
        }
        current = current->parentWidget();
    }
    return false;
}

void WindowTitleBar::onMinimizeButtonClicked()
{
    emit minimizeRequested();
}

void WindowTitleBar::onMaximizeButtonClicked()
{
    emit maximizeRestoreRequested();
}

void WindowTitleBar::onCloseButtonClicked()
{
    emit closeRequested();
}
}
