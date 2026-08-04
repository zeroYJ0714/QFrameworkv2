#include "ProcessWindowHost.h"

#include <QLabel>
#include <QStackedLayout>
#include <QWindow>

namespace qframework
{
ProcessWindowHost::ProcessWindowHost(QWidget* parent)
    : QWidget(parent),
      stackedLayout_(new QStackedLayout(this)),
      placeholderLabel_(new QLabel(QString::fromUtf8(u8"等待子进程窗口"), this)),
      windowContainer_(nullptr),
      foreignWindow_(nullptr)
{
    placeholderLabel_->setAlignment(Qt::AlignCenter);
    placeholderLabel_->setWordWrap(true);
    placeholderLabel_->setObjectName(QStringLiteral("ProcessWindowPlaceholder"));
    stackedLayout_->setContentsMargins(0, 0, 0, 0);
    stackedLayout_->addWidget(placeholderLabel_);
    setMinimumSize(320, 180);
}

ProcessWindowHost::~ProcessWindowHost()
{
    clearEmbeddedWindow();
}

bool ProcessWindowHost::attachWindow(quintptr windowId, QString* errorMessage)
{
    if (windowId == 0) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"子进程窗口句柄为空");
        return false;
    }

    clearEmbeddedWindow();
    foreignWindow_ = QWindow::fromWinId(static_cast<WId>(windowId));
    if (foreignWindow_ == nullptr) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"Qt 无法包装子进程 HWND");
        return false;
    }

    foreignWindow_->setFlags(Qt::FramelessWindowHint);
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

void ProcessWindowHost::showPlaceholder(const QString& detail)
{
    clearEmbeddedWindow();
    placeholderLabel_->setText(detail.isEmpty()
        ? QString::fromUtf8(u8"等待子进程窗口") : detail);
    stackedLayout_->setCurrentWidget(placeholderLabel_);
}

bool ProcessWindowHost::hasEmbeddedWindow() const
{
    return windowContainer_ != nullptr;
}

void ProcessWindowHost::clearEmbeddedWindow()
{
    if (windowContainer_ == nullptr) {
        foreignWindow_ = nullptr;
        return;
    }

    stackedLayout_->removeWidget(windowContainer_);
    delete windowContainer_;
    windowContainer_ = nullptr;
    // createWindowContainer 持有 QWindow，容器析构后该指针不再有效。
    foreignWindow_ = nullptr;
}
}
