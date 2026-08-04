#include "ProcessWindowHost.h"

#include <QLabel>
#include <QStackedLayout>
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
      foreignWindow_(nullptr)
{
    // 初始状态没有子窗口，先显示居中且可换行的等待说明。
    placeholderLabel_->setAlignment(Qt::AlignCenter);
    placeholderLabel_->setWordWrap(true);
    placeholderLabel_->setObjectName(QStringLiteral("ProcessWindowPlaceholder"));
    stackedLayout_->setContentsMargins(0, 0, 0, 0);
    stackedLayout_->addWidget(placeholderLabel_);
    setMinimumSize(180, 120);
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

    windowContainer_->setFocusPolicy(Qt::StrongFocus);
    stackedLayout_->addWidget(windowContainer_);
    stackedLayout_->setCurrentWidget(windowContainer_);
    return true;
}

// 在进程未启动、断开、失败或重启时清理旧句柄并显示原因。
void ProcessWindowHost::showPlaceholder(const QString& detail)
{
    // 故障/重启时先断开旧窗口，再切回占位页。
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

// 从布局移除并销毁容器；容器拥有的 QWindow 随之失效，指针必须清零。
void ProcessWindowHost::clearEmbeddedWindow()
{
    if (windowContainer_ == nullptr) {
        foreignWindow_ = nullptr;
        return;
    }

    // 先从布局移除再 delete，避免布局保存悬空控件指针。
    stackedLayout_->removeWidget(windowContainer_);
    delete windowContainer_;
    windowContainer_ = nullptr;
    // createWindowContainer 持有 QWindow，容器析构后该指针不再有效。
    foreignWindow_ = nullptr;
}
}
