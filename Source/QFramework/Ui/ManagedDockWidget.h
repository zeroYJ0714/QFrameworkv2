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
    /// @brief 创建 ManagedDockWidget 对象并初始化其基础状态。
    /// @param title 界面显示标题。
    /// @param mainWindow 主窗口借用指针，同时作为 Dock 的 Qt 父对象。
    /// @return 无（构造函数不返回值）。
    explicit ManagedDockWidget(const QString& title,
                               QMainWindow* mainWindow);

signals:
    // 只表示用户按下 Dock 关闭按钮；标签切换不会发出该信号。
    /// @brief 关闭 `closeRequested` 对应的框架操作。
    /// @return 无。
    void closeRequested();

protected:
    /// @brief 关闭 `closeEvent` 对应的框架操作。
    /// @param event Qt 事件对象；仅在当前回调期间有效。
    /// @return 无。
    void closeEvent(QCloseEvent* event) override;
};
}
