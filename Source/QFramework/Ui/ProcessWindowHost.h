#pragma once

// 文件职责：把子进程提供的原生窗口句柄包装成 QWindow，并嵌入主窗口 Dock。
// 当子进程尚未启动、故障或重启时，改为显示文字占位页。

#include <QSize>
#include <QWidget>

#include "QFrameworkGlobal.h"

class QLabel;
class QResizeEvent;
class QShowEvent;
class QStackedLayout;
class QTimer;
class QWindow;

namespace qframework
{
class QFRAMEWORK_EXPORT ProcessWindowHost : public QWidget
{
    Q_OBJECT

public:
    // stackedLayout_ 同一时间只显示占位页或窗口容器之一。
    explicit ProcessWindowHost(QWidget* parent = nullptr);
    ~ProcessWindowHost() override;

    // 成功后主机显示嵌入窗口；失败保留可读错误且不持有无效句柄。
    bool attachWindow(quintptr windowId, QString* errorMessage = nullptr);
    void showPlaceholder(const QString& detail);
    bool hasEmbeddedWindow() const;

signals:
    // 宿主客户区尺寸稳定后通知 MainWindow，再由它发送 resizeWindow 控制帧。
    void clientSizeChanged(const QSize& size);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

private:
    // 连续拖动只保留最后一次尺寸，避免每个 resize 事件都产生 IPC 帧。
    void scheduleClientSizeNotification();
    void flushClientSizeNotification();

    // 释放 Qt 容器；createWindowContainer 会连同其 QWindow 包装对象一起管理。
    void clearEmbeddedWindow();

    QStackedLayout* stackedLayout_;
    QLabel* placeholderLabel_;
    QWidget* windowContainer_;
    QWindow* foreignWindow_;
    QTimer* resizeTimer_;
};
}
