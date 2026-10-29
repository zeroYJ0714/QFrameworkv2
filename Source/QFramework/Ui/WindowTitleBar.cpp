// 【阅读导引】为无边框窗口提供标题栏操作。
// 阅读顺序：本控件产生最小化、最大化、关闭和拖动请求，由 MainWindow 完成实际窗口动作。
// 需要分清：按钮图标必须跟随真实窗口状态更新；点击标题栏和点击子控件要区分，不能把菜单点击误判为拖动。
// 这是执行实现：按入口调用和信号回调追踪，不要按文件行号把所有函数理解成连续执行。

// 文件职责：实现自定义窗口标题栏的拖动、最小化、最大化和关闭按钮。
// 所有 QWidget、鼠标事件和 QAction 都只在 GUI 线程处理；标题栏不拥有业务模块，关闭
// 请求交给 MainWindow 的统一保存/放弃/取消流程，避免直接销毁未保存的插件界面。
#include "WindowTitleBar.h"

#include <QAbstractButton>
#include <QApplication>
#include <QHBoxLayout>
#include <QEvent>
#include <QLabel>
#include <QMenuBar>
#include <QMouseEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QStylePainter>
#include <QStyleOption>
#include <QToolButton>

namespace qframework
{
namespace
{
class ElidedTitleLabel : public QLabel
{
public:
    explicit ElidedTitleLabel(QWidget* parent) : QLabel(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setMinimumWidth(0);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QStylePainter painter(this);
        QStyleOption option;
        option.initFrom(this);
        painter.drawPrimitive(QStyle::PE_Widget, option);
        painter.drawItemText(contentsRect(), Qt::AlignLeft | Qt::AlignVCenter,
                             palette(), isEnabled(),
                             fontMetrics().elidedText(text(), Qt::ElideRight,
                                                      contentsRect().width()),
                             foregroundRole());
    }
};
}

// 构造完整标题栏：先建立 Qt 子控件和固定槽位，再把按钮点击转换成上层 signal。
// 所有控件都以 this 为 parent，MainWindow 销毁 WindowTitleBar 时 Qt 会按父子关系
// 自动释放布局、菜单栏、标签和按钮，不需要手工 delete。
/// @brief 构造完整标题栏：建立菜单栏和三个窗口按钮并连接点击信号。
/// @param parent 父控件，通常是 MainWindow，同时作为全部子控件的父对象。
/// @return 无（构造函数不返回值）。
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
        QList<QToolButton*>() << minimizeButton_ << maximizeButton_ << closeButton_; ///< 三个窗口按钮，用于统一设置公共外观。
    for (QToolButton* button : windowButtons) { ///< 当前被设置外观的窗口按钮。
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

    QHBoxLayout* layout = new QHBoxLayout(this); ///< 标题栏水平布局：菜单靠左，按钮靠右。
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

/// @brief 返回标题栏内嵌的菜单栏，供 MainWindow 挂载模块菜单。
/// @return 本控件持有的 QMenuBar 指针，所有权仍属于标题栏。
QMenuBar* WindowTitleBar::menuBar() const
{
    return menuBar_;
}

void WindowTitleBar::setDialogMode(Qt::WindowFlags originalFlags)
{
    if (dialogMode_)
        return;
    dialogMode_ = true;
    setProperty("role", QStringLiteral("dialogTitleBar"));
    menuBar_->hide();
    titleLabel_ = new ElidedTitleLabel(this);
    titleLabel_->setObjectName(QStringLiteral("DialogTitleLabel"));
    auto* box = qobject_cast<QHBoxLayout*>(layout());
    delete box->takeAt(1); // 用可伸缩标题替代主窗口菜单旁的空白占位。
    box->insertWidget(1, titleLabel_, 1);
    minimizeButton_->setVisible(originalFlags.testFlag(Qt::WindowMinimizeButtonHint));
    maximizeButton_->setVisible(originalFlags.testFlag(Qt::WindowMaximizeButtonHint));
    closeButton_->setVisible(originalFlags.testFlag(Qt::WindowCloseButtonHint));
    for (auto* button : {minimizeButton_, maximizeButton_, closeButton_}) {
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    }
    updateWindowControlState(window()->isMaximized());
}

void WindowTitleBar::setTitle(const QString& title)
{
    if (titleLabel_ == nullptr)
        return;
    titleLabel_->setText(title);
    titleLabel_->setToolTip(title);
    titleLabel_->setAccessibleName(title);
    setToolTip(title); // 标题文字鼠标穿透到标题栏，完整标题提示由标题栏提供。
}

void WindowTitleBar::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::StyleChange)
        updateWindowControlState(window()->isMaximized());
}

// WM_NCHITTEST 会先把屏幕坐标换成标题栏客户区坐标，再调用此函数。
// 只有菜单和按钮需要保留 HTCLIENT；品牌文字等不可点击区域仍按“空白”处理，
// 让用户可以从视觉上连续的标题栏区域拖动窗口。
/// @brief 判断标题栏坐标处是否落在需要保留鼠标命中的交互控件上。
/// @param titleBarPoint 已换算到标题栏客户区的坐标。
/// @return 坐标落在菜单或按钮上返回 true，落在空白区域返回 false。
bool WindowTitleBar::isInteractiveAt(const QPoint& titleBarPoint) const
{
    if (!rect().contains(titleBarPoint))
        return false;
    return isInteractiveTitleBarChild(childAt(titleBarPoint));
}

// 标题栏是 Qt 客户区 QWidget。空白处按下后把位置交给 MainWindow，由它调用
// QWindow::startSystemMove()，从而保留 Windows 贴边、跨屏和系统拖动行为。
/// @brief 在标题栏空白处按下左键时记录拖动起点，准备发起窗口移动。
/// @param event 鼠标按下事件；交互控件上的按下会交回基类处理。
/// @return 无。
void WindowTitleBar::mousePressEvent(QMouseEvent* event)
{
    if (event != nullptr && event->button() == Qt::LeftButton &&
        !isInteractiveAt(event->pos())) {
        movePressed_ = true;
        moveStarted_ = false;
        pressGlobalPosition_ = event->globalPos();
        pressTitleBarPosition_ = event->pos();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

/// @brief 拖动超过系统阈值后发出 moveRequested，由 MainWindow 执行窗口移动。
/// @param event 鼠标移动事件。
/// @return 无。
void WindowTitleBar::mouseMoveEvent(QMouseEvent* event)
{
    if (event != nullptr && movePressed_ && !moveStarted_ &&
        (event->globalPos() - pressGlobalPosition_).manhattanLength() >=
            QApplication::startDragDistance()) {
        moveStarted_ = true;
        emit moveRequested(event->globalPos(), pressTitleBarPosition_);
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

/// @brief 松开左键时清除拖动状态，避免后续移动误触发窗口移动。
/// @param event 鼠标松开事件。
/// @return 无。
void WindowTitleBar::mouseReleaseEvent(QMouseEvent* event)
{
    if (event != nullptr && event->button() == Qt::LeftButton) {
        movePressed_ = false;
        moveStarted_ = false;
    }
    QWidget::mouseReleaseEvent(event);
}

// Windows 原生标题栏通常支持双击最大化/还原；无边框后由这个 Qt 事件补回。
// 菜单和三个按钮属于交互控件，不会触发窗口状态切换。
/// @brief 标题栏空白处双击时发出最大化/还原请求，补回系统标题栏行为。
/// @param event 鼠标双击事件。
/// @return 无。
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
/// @brief 按当前窗口状态刷新最大化按钮的图标、提示和无障碍名称。
/// @param maximized 窗口当前是否处于最大化状态。
/// @return 无。
void WindowTitleBar::updateWindowControlState(bool maximized)
{
    if (dialogMode_) {
        // 文字符号使用 QSS 前景色，避免系统位图图标在深色主题中变成黑色。
        minimizeButton_->setIcon(QIcon());
        maximizeButton_->setIcon(QIcon());
        closeButton_->setIcon(QIcon());
        minimizeButton_->setText(QString::fromUtf8(u8"−"));
        maximizeButton_->setText(maximized ? QString::fromUtf8(u8"❐")
                                          : QString::fromUtf8(u8"□"));
        closeButton_->setText(QString::fromUtf8(u8"×"));
        const QString text = maximized ? QString::fromUtf8(u8"还原")
                                       : QString::fromUtf8(u8"最大化");
        maximizeButton_->setToolTip(text);
        maximizeButton_->setAccessibleName(text);
        return;
    }
    // 三个按钮都从当前 Qt style 取标准图标，避免依赖外部资源；只有中间按钮会
    // 根据窗口状态在“最大化”和“还原”之间切换，另外两个图标始终固定。
    minimizeButton_->setIcon(style()->standardIcon(QStyle::SP_TitleBarMinButton));
    closeButton_->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
    const QStyle::StandardPixmap pixmap = maximized
        ? QStyle::SP_TitleBarNormalButton
        : QStyle::SP_TitleBarMaxButton; ///< 最大化按钮要使用的标准图标枚举。
    const QString accessibleName = maximized
        ? QString::fromUtf8(u8"还原")
        : QString::fromUtf8(u8"最大化"); ///< 最大化按钮对应的提示与无障碍名称。
    maximizeButton_->setIcon(style()->standardIcon(pixmap));
    maximizeButton_->setToolTip(accessibleName);
    maximizeButton_->setAccessibleName(accessibleName);
}

// 判断点击控件是否真的会处理鼠标。QMenuBar 和 QAbstractButton 的子控件返回
// true，普通 QLabel/标题栏本身返回 false，后者会被 MainWindow 交给 HTCAPTION。
/// @brief 沿父链判断子控件是否属于菜单或按钮，用于决定是否保留 HTCLIENT。
/// @param child 命中的子控件，可为空。
/// @return 命中菜单栏或按钮时返回 true，否则返回 false。
bool WindowTitleBar::isInteractiveTitleBarChild(QWidget* child) const
{
    QWidget* current = child; ///< 沿父链向上遍历的游标，从命中控件开始。
    while (current != nullptr && current != this) {
        if (qobject_cast<QMenuBar*>(current) != nullptr ||
            qobject_cast<QAbstractButton*>(current) != nullptr) {
            return true;
        }
        current = current->parentWidget();
    }
    return false;
}

/// @brief 响应最小化按钮点击，转发为 minimizeRequested 信号。
/// @return 无。
void WindowTitleBar::onMinimizeButtonClicked()
{
    emit minimizeRequested();
}

/// @brief 响应最大化按钮点击，转发为最大化/还原请求。
/// @return 无。
void WindowTitleBar::onMaximizeButtonClicked()
{
    emit maximizeRestoreRequested();
}

/// @brief 响应关闭按钮点击，转发为 closeRequested 交给 MainWindow 处理。
/// @return 无。
void WindowTitleBar::onCloseButtonClicked()
{
    emit closeRequested();
}
}
