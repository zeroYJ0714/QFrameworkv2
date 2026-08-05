#pragma once

// 文件职责：统一 QFramework 模块 Dock 的可移动/可关闭规则。
// Dock 不允许永久浮动，主窗口和布局管理器因此只处理受控停靠状态。

#include <QDockWidget>

#include "QFrameworkGlobal.h"

class QMainWindow;
class QCloseEvent;

namespace qframework
{
class QFRAMEWORK_EXPORT ManagedDockWidget : public QDockWidget
{
    Q_OBJECT

public:
    // mainWindow 同时作为 QObject 父对象和 QDockWidget 宿主。
    explicit ManagedDockWidget(const QString& title,
                               QMainWindow* mainWindow);

signals:
    // 只表示用户按下 Dock 关闭按钮；标签切换不会发出该信号。
    void closeRequested();

protected:
    void closeEvent(QCloseEvent* event) override;
};
}
