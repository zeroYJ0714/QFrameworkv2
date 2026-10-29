// 【阅读导引】把另一个进程的原生窗口放进 Qt 面板。
// 阅读顺序：子进程就绪前显示 placeholder；拿到窗口句柄后包装 QWindow 并放入容器；尺寸变化再通知监督器。
// 需要分清：原生窗口属于子进程，包装器属于宿主。窗口句柄失效后不能继续当 QWidget 使用；释放包装不应误当作直接销毁子进程。
// 这是执行实现：按入口调用和信号回调追踪，不要按文件行号把所有函数理解成连续执行。

// 文件职责：把进程 UI 模块提供的原生窗口句柄嵌入 Qt 宿主 Dock，并处理尺寸同步。
// 原生句柄和 QWidget 都由 GUI 线程使用；本类只借用子进程窗口，不拥有子进程本身，进程
// 停止或句柄失效时清空嵌入状态并发出错误，不在窗口事件中同步等待进程退出。
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
/// @brief 构造宿主控件，初始只显示占位标签等待子进程窗口。
/// @param parent 父控件，通常是对应模块的 ManagedDockWidget。
/// @return 无（构造函数不返回值）。
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
/// @brief 析构前解除外部窗口容器，避免宿主销毁后残留悬空 HWND。
/// @return 无（析构函数不返回值）。
ProcessWindowHost::~ProcessWindowHost()
{
    // 统一走 clear，避免容器和 foreignWindow_ 所有权处理分叉。
    clearEmbeddedWindow();
}

// 把监督器上报的原生句柄包装成 QWindow/QWidget，并切换堆叠布局当前页。
/// @brief 把子进程原生窗口句柄包装成 QWindow 容器并切换为当前页。
/// @param windowId 子进程上报的原生窗口句柄，0 视为无效。
/// @param errorMessage 可选输出参数，失败时写入中文原因。
/// @return 成功嵌入返回 true，句柄为空或 Qt 无法包装时返回 false。
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

    // Qt 5.15 默认可能让 QWindowContainer 保持“非原生 QWidget”，然后把外部
    // QWindow 直接挂到 QMainWindow 的顶层 HWND。这样画面虽然通常仍落在当前
    // Dock 内，但外部窗口没有经过 ProcessWindowHost 的原生裁剪，最大化后可能
    // 继续覆盖下方 Dock 的分隔条和关闭按钮。
    //
    // 强制容器在第一次显示前创建自己的 HWND。QWindowContainer 看到 internalWinId()
    // 后会使用原生父子链，把 foreignWindow_ 设为这个容器的子窗口；Qt/Windows
    // 才能同时保证尺寸同步、WS_CLIPCHILDREN 裁剪和正确的鼠标命中范围。
    windowContainer_->setAttribute(Qt::WA_NativeWindow);
    windowContainer_->winId();

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
/// @brief 清理已嵌入窗口并切回占位页，同时显示失败或等待原因。
/// @param detail 占位页文案；为空时使用默认等待提示。
/// @return 无。
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
/// @brief 查询当前是否已成功嵌入子进程窗口容器。
/// @return 已创建嵌入容器返回 true，否则返回 false。
bool ProcessWindowHost::hasEmbeddedWindow() const
{
    // 以容器是否存在为准；裸 foreignWindow_ 不代表已经成功嵌入。
    return windowContainer_ != nullptr;
}

/// @brief 宿主尺寸变化时同步嵌入容器几何，并重新安排尺寸上报。
/// @param event 尺寸变化事件，先交给基类处理。
/// @return 无。
void ProcessWindowHost::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (windowContainer_ != nullptr)
        windowContainer_->setGeometry(rect());
    scheduleClientSizeNotification();
}

/// @brief 宿主显示时同步嵌入容器几何，并重新安排尺寸上报。
/// @param event 显示事件，先交给基类处理。
/// @return 无。
void ProcessWindowHost::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (windowContainer_ != nullptr)
        windowContainer_->setGeometry(rect());
    scheduleClientSizeNotification();
}

/// @brief 给出宿主控件的推荐尺寸。
/// @return 固定的推荐尺寸，避免外部窗口历史尺寸撑大 Dock。
QSize ProcessWindowHost::sizeHint() const
{
    // 固定宿主提示尺寸，避免 foreign QWindow 的历史尺寸把整个 Dock 拉长。
    return QSize(320, 240);
}

/// @brief 给出宿主控件的最小推荐尺寸。
/// @return 固定的最小推荐尺寸。
QSize ProcessWindowHost::minimumSizeHint() const
{
    return QSize(180, 120);
}

/// @brief 延迟汇总尺寸变化，用定时器合并 Dock 重排过程中的多次 resize。
/// @return 无；未嵌入窗口时不安排上报。
void ProcessWindowHost::scheduleClientSizeNotification()
{
    // QMainWindow 在 Dock 拖动、上下拆分和 restoreState() 过程中，会短暂隐藏
    // ProcessWindowHost，再用新的几何尺寸重新布置它。此时 resizeEvent 仍然代表
    // 有效的最终客户区尺寸，不能因为 QWidget::isVisible() 暂时为 false 就丢弃。
    // 用户是否真正要求隐藏模块，由 MainWindow 保存的菜单/布局意图统一判断。
    if (windowContainer_ == nullptr)
        return;
    resizeTimer_->start();
}

/// @brief 定时器到期后把最终客户区尺寸上报给 MainWindow。
/// @return 无；未嵌入窗口或尺寸非正时不发信号。
void ProcessWindowHost::flushClientSizeNotification()
{
    // 50 ms 到期时 Dock 可能仍处于 Qt 内部重排阶段；只要嵌入容器仍存在，就把
    // 最后一次有效尺寸交给 MainWindow。故障或重启会先 clearEmbeddedWindow()，
    // 因而不会把旧 HWND 的尺寸误发给下一代子进程。
    if (windowContainer_ == nullptr)
        return;
    windowContainer_->setGeometry(rect());
    const QSize clientSize = rect().size(); ///< 当前宿主客户区尺寸，用于通知子进程调整自身窗口。
    if (clientSize.width() > 0 && clientSize.height() > 0)
        emit clientSizeChanged(clientSize);
}

// 先解除原生父子关系，避免同步等待子进程退出时互等窗口消息。
/// @brief 立即解除窗口嵌入，再延迟回收容器和 Qt 包装对象。
/// @return 无。
void ProcessWindowHost::clearEmbeddedWindow()
{
    if (windowContainer_ == nullptr) {
        foreignWindow_ = nullptr;
        return;
    }

    // 旧 HWND 可能仍在处理 resize/paint；保留延迟回收以避免重启边界访问失效句柄。
    QWidget* retiredContainer = windowContainer_; ///< 待退役的旧容器，延后销毁避免访问失效句柄。
    windowContainer_ = nullptr;
    stackedLayout_->removeWidget(retiredContainer);
    retiredContainer->hide();
    if (foreignWindow_ != nullptr) {
        foreignWindow_->setParent(nullptr);
        // 只转交对象所有权；主事件循环结束后也能随旧容器回收，不恢复原生嵌入。
        foreignWindow_->QObject::setParent(retiredContainer);
    }
    retiredContainer->deleteLater();
    foreignWindow_ = nullptr;
}
}
