#include "ProcessWindowHost.h"

#include <QLabel>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStackedLayout>
#include <QTimer>
#include <QWindow>

// 主进程只包装 HWND，不拥有子进程业务 QWidget；子进程退出时必须先清除
// 容器并显示占位页，不能继续访问旧窗口句柄。

namespace qframework
{
// 初始只显示占位标签；收到有效子进程句柄后再切换到嵌入容器。
ProcessWindowHost::ProcessWindowHost(QWidget* parent)
    : QWidget(parent),
      stackedLayout_(new QStackedLayout(this)),
      placeholderLabel_(new QLabel(QString::fromUtf8(u8"等待子进程窗口"), this)),
      windowContainer_(nullptr),
      foreignWindow_(nullptr),
      resizeTimer_(new QTimer(this))
{
    // 初始状态没有子窗口，先显示居中且可换行的等待说明。
    placeholderLabel_->setAlignment(Qt::AlignCenter);
    placeholderLabel_->setWordWrap(true);
    placeholderLabel_->setObjectName(QStringLiteral("ProcessWindowPlaceholder"));
    stackedLayout_->setContentsMargins(0, 0, 0, 0);
    stackedLayout_->addWidget(placeholderLabel_);
    resizeTimer_->setSingleShot(true);
    resizeTimer_->setInterval(50);
    connect(resizeTimer_, &QTimer::timeout,
            this, &ProcessWindowHost::flushClientSizeNotification);
    setMinimumSize(180, 120);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

// 析构前解除外部窗口容器，避免 Qt 在宿主销毁后保留悬空 HWND。
ProcessWindowHost::~ProcessWindowHost()
{
    // 统一走 clear，避免容器和 foreignWindow_ 所有权处理分叉。
    clearEmbeddedWindow();
}

// 把监督器上报的原生句柄包装成 QWindow/QWidget，并切换堆叠布局当前页。
bool ProcessWindowHost::attachWindow(quintptr windowId, QString* errorMessage)
{
    // 0 从来不是有效窗口句柄，不交给 QWindow::fromWinId。
    if (windowId == 0) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"子进程窗口句柄为空");
        return false;
    }

    // 新窗口到来通常意味着子进程重启，先移除旧容器。
    clearEmbeddedWindow();
    foreignWindow_ = QWindow::fromWinId(static_cast<WId>(windowId));
    if (foreignWindow_ == nullptr) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"Qt 无法包装子进程 HWND");
        return false;
    }

    foreignWindow_->setFlags(Qt::FramelessWindowHint);
    // createWindowContainer 创建 QWidget 外壳，使 QDockWidget 能管理 QWindow。
    windowContainer_ = QWidget::createWindowContainer(foreignWindow_, this);
    if (windowContainer_ == nullptr) {
        delete foreignWindow_;
        foreignWindow_ = nullptr;
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"Qt 无法创建子进程窗口容器");
        return false;
    }

    // 外部 QWindow 的 sizeHint 可能把 QDockWidget 最小尺寸放大到屏幕之外；
    // 宿主尺寸由自己的客户区决定，不能让外部窗口反向污染布局约束。
    windowContainer_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    windowContainer_->setMinimumSize(0, 0);
    windowContainer_->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    windowContainer_->setFocusPolicy(Qt::StrongFocus);
    stackedLayout_->addWidget(windowContainer_);
    stackedLayout_->setCurrentWidget(windowContainer_);
    windowContainer_->setGeometry(rect());
    scheduleClientSizeNotification();
    return true;
}

// 在进程未启动、断开、失败或重启时清理旧句柄并显示原因。
void ProcessWindowHost::showPlaceholder(const QString& detail)
{
    // 故障/重启时先断开旧窗口，再切回占位页。
    resizeTimer_->stop();
    clearEmbeddedWindow();
    placeholderLabel_->setText(detail.isEmpty()
        ? QString::fromUtf8(u8"等待子进程窗口") : detail);
    stackedLayout_->setCurrentWidget(placeholderLabel_);
}

// 只报告容器已经成功创建，不把非空句柄误当成可见窗口。
bool ProcessWindowHost::hasEmbeddedWindow() const
{
    // 以容器是否存在为准；裸 foreignWindow_ 不代表已经成功嵌入。
    return windowContainer_ != nullptr;
}

void ProcessWindowHost::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (windowContainer_ != nullptr)
        windowContainer_->setGeometry(rect());
    scheduleClientSizeNotification();
}

void ProcessWindowHost::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (windowContainer_ != nullptr)
        windowContainer_->setGeometry(rect());
    scheduleClientSizeNotification();
}

QSize ProcessWindowHost::sizeHint() const
{
    // 固定宿主提示尺寸，避免 foreign QWindow 的历史尺寸把整个 Dock 拉长。
    return QSize(320, 240);
}

QSize ProcessWindowHost::minimumSizeHint() const
{
    return QSize(180, 120);
}

void ProcessWindowHost::scheduleClientSizeNotification()
{
    if (windowContainer_ == nullptr || !isVisible())
        return;
    resizeTimer_->start();
}

void ProcessWindowHost::flushClientSizeNotification()
{
    if (windowContainer_ == nullptr || !isVisible())
        return;
    windowContainer_->setGeometry(rect());
    const QSize clientSize = rect().size();
    if (clientSize.width() > 0 && clientSize.height() > 0)
        emit clientSizeChanged(clientSize);
}

// 从布局移除并销毁容器；容器拥有的 QWindow 随之失效，指针必须清零。
void ProcessWindowHost::clearEmbeddedWindow()
{
    if (windowContainer_ == nullptr) {
        foreignWindow_ = nullptr;
        return;
    }

    // 先隐藏并从布局移除，再延迟销毁。旧进程的 HWND 可能仍在处理原生
    // resize/paint 事件，同步 delete 会让 Qt 容器在重启边界访问失效句柄。
    QWidget* retiredContainer = windowContainer_;
    windowContainer_ = nullptr;
    stackedLayout_->removeWidget(retiredContainer);
    retiredContainer->hide();
    retiredContainer->deleteLater();
    // createWindowContainer 持有 QWindow，容器析构后该指针不再有效。
    foreignWindow_ = nullptr;
}
}
