#include "DialogStyle.h"
#include "WindowTitleBar.h"

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QHash>
#include <QLayout>
#include <QPointer>
#include <QSet>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QWindow>

#ifdef Q_OS_WIN
#include <windows.h>
#include <windowsx.h>
#endif

namespace qframework
{
namespace
{
const char kInstalled[] = "_qframeworkDialogStyleInstalled";

class DialogStyleManager : public QObject, public QAbstractNativeEventFilter
{
public:
    explicit DialogStyleManager(QApplication* application) : QObject(application)
    {
        application->installEventFilter(this);
        application->installNativeEventFilter(this);
    }

    ~DialogStyleManager() override
    {
        if (auto* application = QCoreApplication::instance())
            application->removeNativeEventFilter(this);
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        const auto type = event->type();
        if (type != QEvent::Polish && type != QEvent::Show && type != QEvent::ChildAdded &&
            type != QEvent::StyleChange &&
            type != QEvent::LayoutRequest && type != QEvent::WindowActivate &&
            type != QEvent::WindowDeactivate && type != QEvent::WindowStateChange)
            return false;

        auto* dialog = qobject_cast<QDialog*>(object);
        if (dialog == nullptr || !dialog->isWindow() || attaching_.contains(dialog))
            return false;
        if (type == QEvent::ChildAdded && !titleBars_.contains(dialog))
            return false;

        if (!titleBars_.contains(dialog)) {
            // 已有自绘外框的窗口不再叠加标题栏。
            if (dialog->windowFlags().testFlag(Qt::FramelessWindowHint) ||
                dialog->layout() == nullptr)
                return false;
            decorate(dialog);
        }
        auto* bar = titleBars_.value(dialog).data();
        if (bar == nullptr)
            return false;

        // QMessageBox 在增删说明、详细信息时会删除并重新建立布局。
        if (dialog->layout() != nullptr && dialog->layout()->menuBar() != bar) {
            attaching_.insert(dialog);
            dialog->layout()->setMenuBar(bar);
            bar->show();
            dialog->layout()->invalidate();
            attaching_.remove(dialog);
            refreshLayout(dialog);
        }
        if (type == QEvent::StyleChange)
            refreshLayout(dialog);
        if (type == QEvent::Show || type == QEvent::LayoutRequest)
            updateResizeSupport(dialog);
        if (type == QEvent::WindowStateChange)
            bar->updateWindowControlState(dialog->isMaximized());
        if (type == QEvent::WindowActivate || type == QEvent::WindowDeactivate ||
            type == QEvent::Show) {
            const bool active = type == QEvent::WindowActivate ||
                                (type != QEvent::WindowDeactivate && dialog->isActiveWindow());
            if (bar->property("active").toBool() != active) {
                bar->setProperty("active", active);
                const auto widgets = bar->findChildren<QWidget*>();
                bar->style()->unpolish(bar);
                bar->style()->polish(bar);
                for (auto* widget : widgets) {
                    widget->style()->unpolish(widget);
                    widget->style()->polish(widget);
                    widget->update();
                }
                bar->update();
            }
        }
        return false;
    }

    bool nativeEventFilter(const QByteArray&, void* message, long* result) override
    {
#ifdef Q_OS_WIN
        auto* msg = static_cast<MSG*>(message);
        if (msg == nullptr || result == nullptr ||
            (msg->message != WM_NCHITTEST && msg->message != WM_GETMINMAXINFO &&
             msg->message != WM_NCCALCSIZE))
            return false;
        auto* dialog = qobject_cast<QDialog*>(QWidget::find(reinterpret_cast<WId>(msg->hwnd)));
        auto* bar = titleBars_.value(dialog).data();
        if (dialog == nullptr || bar == nullptr)
            return false;

        // 保留 WS_THICKFRAME 的系统缩放能力，但整个窗口仍由 Qt/QSS 绘制。
        if (msg->message == WM_NCCALCSIZE) {
            *result = 0;
            return true;
        }

        const qreal scale = dialog->devicePixelRatioF();
        if (msg->message == WM_GETMINMAXINFO) {
            auto* limits = reinterpret_cast<MINMAXINFO*>(msg->lParam);
            MONITORINFO monitor = {};
            monitor.cbSize = sizeof(monitor);
            if (limits == nullptr || !GetMonitorInfo(
                    MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
                return false;
            limits->ptMaxPosition.x = monitor.rcWork.left - monitor.rcMonitor.left;
            limits->ptMaxPosition.y = monitor.rcWork.top - monitor.rcMonitor.top;
            limits->ptMaxSize.x = monitor.rcWork.right - monitor.rcWork.left;
            limits->ptMaxSize.y = monitor.rcWork.bottom - monitor.rcWork.top;
            limits->ptMinTrackSize.x = qRound(dialog->minimumWidth() * scale);
            limits->ptMinTrackSize.y = qRound(dialog->minimumHeight() * scale);
            if (dialog->maximumWidth() < QWIDGETSIZE_MAX)
                limits->ptMaxTrackSize.x = qRound(dialog->maximumWidth() * scale);
            if (dialog->maximumHeight() < QWIDGETSIZE_MAX)
                limits->ptMaxTrackSize.y = qRound(dialog->maximumHeight() * scale);
            limits->ptMaxSize.x = qMin(limits->ptMaxSize.x, limits->ptMaxTrackSize.x);
            limits->ptMaxSize.y = qMin(limits->ptMaxSize.y, limits->ptMaxTrackSize.y);
            *result = 0;
            return true;
        }

        POINT point = {GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        if (!ScreenToClient(msg->hwnd, &point))
            return false;
        const QPoint logical(qRound(point.x / scale), qRound(point.y / scale));
        if (bar->isInteractiveAt(bar->mapFrom(dialog, logical))) {
            *result = HTCLIENT;
            return true;
        }
        RECT rect = {};
        if (!GetWindowRect(msg->hwnd, &rect))
            return false;
        const int border = qMax(1, qRound(5 * scale));
        const bool fixed = dialog->windowFlags().testFlag(Qt::MSWindowsFixedSizeDialogHint) ||
                           (dialog->layout() != nullptr &&
                            dialog->layout()->sizeConstraint() == QLayout::SetFixedSize);
        const bool horizontal = !fixed && !dialog->isMaximized() &&
                                dialog->minimumWidth() < dialog->maximumWidth();
        const bool vertical = !fixed && !dialog->isMaximized() &&
                              dialog->minimumHeight() < dialog->maximumHeight();
        const int x = GET_X_LPARAM(msg->lParam);
        const int y = GET_Y_LPARAM(msg->lParam);
        const bool left = horizontal && x < rect.left + border;
        const bool right = horizontal && x >= rect.right - border;
        const bool top = vertical && y < rect.top + border;
        const bool bottom = vertical && y >= rect.bottom - border;
        *result = top ? (left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP)
                  : bottom ? (left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM)
                  : left ? HTLEFT : right ? HTRIGHT : HTCLIENT;
        return true;
#else
        Q_UNUSED(message)
        Q_UNUSED(result)
        return false;
#endif
    }

private:
    void updateResizeSupport(QDialog* dialog)
    {
        const bool fixed = dialog->windowFlags().testFlag(Qt::MSWindowsFixedSizeDialogHint) ||
                           (dialog->layout() != nullptr &&
                            dialog->layout()->sizeConstraint() == QLayout::SetFixedSize);
        const bool resizable = !fixed && (dialog->minimumWidth() < dialog->maximumWidth() ||
                                         dialog->minimumHeight() < dialog->maximumHeight());
        if (dialog->isSizeGripEnabled() != resizable)
            dialog->setSizeGripEnabled(resizable);
#ifdef Q_OS_WIN
        const auto handle = reinterpret_cast<HWND>(dialog->winId());
        const LONG_PTR style = GetWindowLongPtr(handle, GWL_STYLE);
        const LONG_PTR updated = resizable ? (style | WS_THICKFRAME) : (style & ~WS_THICKFRAME);
        if (style != updated) {
            // Qt 的 FramelessWindowHint 会去掉 WS_THICKFRAME；只有 HT* 命中值
            // 会出现缩放光标，却无法让 Windows 真正进入边缘拖动缩放。
            SetWindowLongPtr(handle, GWL_STYLE, updated);
            SetWindowPos(handle, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
#endif
    }

    void refreshLayout(QDialog* dialog)
    {
        if (pendingLayouts_.contains(dialog))
            return;
        pendingLayouts_.insert(dialog);
        // QMessageBox::updateSize 会清掉已投递的 LayoutRequest；等它退出后再让
        // Qt 自己重算一次，才能计入新标题栏和刚完成 polish 的按钮尺寸。
        QTimer::singleShot(0, dialog, [this, dialog] {
            pendingLayouts_.remove(dialog);
            if (dialog->layout() != nullptr)
                dialog->layout()->invalidate();
            QEvent request(QEvent::LayoutRequest);
            QCoreApplication::sendEvent(dialog, &request);
        });
    }

    void decorate(QDialog* dialog)
    {
        attaching_.insert(dialog);
        const auto flags = dialog->windowFlags();
        const bool explicitlySized = dialog->testAttribute(Qt::WA_Resized);
        const QSize size = dialog->size();
        const QSize minimum = dialog->minimumSize();
        const QSize maximum = dialog->maximumSize();
        auto* bar = new WindowTitleBar(dialog);
        titleBars_.insert(dialog, bar);
        connect(dialog, &QObject::destroyed, this, [this, dialog] {
            titleBars_.remove(dialog);
            attaching_.remove(dialog);
            pendingLayouts_.remove(dialog);
        });
        bar->setDialogMode(flags);
        bar->setTitle(dialog->windowTitle());
        bar->setProperty("active", dialog->isActiveWindow());
        connect(dialog, &QWidget::windowTitleChanged, bar, &WindowTitleBar::setTitle);
        connect(bar, &WindowTitleBar::closeRequested, dialog, &QWidget::close);
        connect(bar, &WindowTitleBar::minimizeRequested, dialog, &QWidget::showMinimized);
        connect(bar, &WindowTitleBar::maximizeRestoreRequested, dialog, [dialog, flags] {
            if (!flags.testFlag(Qt::WindowMaximizeButtonHint))
                return;
            if (dialog->isMaximized())
                dialog->showNormal();
            else
                dialog->showMaximized();
        });
        connect(bar, &WindowTitleBar::moveRequested, dialog,
                [dialog](const QPoint& global, const QPoint& titlePosition) {
            if (dialog->isMaximized()) {
                const qreal ratio = qreal(titlePosition.x()) / qMax(1, dialog->width());
                dialog->showNormal();
                dialog->move(global - QPoint(qRound(dialog->width() * ratio), titlePosition.y()));
            }
            if (dialog->windowHandle() != nullptr)
                dialog->windowHandle()->startSystemMove();
        });
        dialog->setProperty("qframeworkStyledDialog", true);
        dialog->setAttribute(Qt::WA_StyledBackground);
        dialog->setWindowFlags(flags | Qt::FramelessWindowHint);
        dialog->layout()->setMenuBar(bar);
        const int extra = bar->height();
        dialog->setMinimumSize(minimum.width(), qMin(QWIDGETSIZE_MAX, minimum.height() + extra));
        if (maximum.height() < QWIDGETSIZE_MAX)
            dialog->setMaximumHeight(qMin(QWIDGETSIZE_MAX, maximum.height() + extra));
        if (explicitlySized)
            dialog->resize(size.width(), size.height() + extra);
        attaching_.remove(dialog);
    }

    QHash<QDialog*, QPointer<WindowTitleBar>> titleBars_;
    QSet<QDialog*> attaching_;
    QSet<QDialog*> pendingLayouts_;
};
}

void installDialogStyle(QApplication* application)
{
    if (application == nullptr || application->property(kInstalled).toBool())
        return;
    Q_ASSERT(QThread::currentThread() == application->thread());
    application->setProperty(kInstalled, true);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    new DialogStyleManager(application);
}
}
