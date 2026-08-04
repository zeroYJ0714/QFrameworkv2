#pragma once

// 文件职责：把子进程提供的原生窗口句柄包装成 QWindow，并嵌入主窗口 Dock。
// 当子进程尚未启动、故障或重启时，改为显示文字占位页。

#include <QWidget>

#include "QFrameworkGlobal.h"

class QLabel;
class QStackedLayout;
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

private:
    // 释放 Qt 容器；createWindowContainer 会连同其 QWindow 包装对象一起管理。
    void clearEmbeddedWindow();

    QStackedLayout* stackedLayout_;
    QLabel* placeholderLabel_;
    QWidget* windowContainer_;
    QWindow* foreignWindow_;
};
}
