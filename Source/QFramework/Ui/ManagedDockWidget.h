// 【阅读导引】提供受控停靠面板。
// 阅读顺序：Qt 负责基本停靠；本类把关闭操作转成 closeRequested，让主窗口决定隐藏和关闭门禁。
// 需要分清：面板关闭不等于卸载 DLL，也不等于终止设备线程。不要在这里放某个业务插件的保存或录制逻辑。
// 这是声明/类型入口：先看 public 接口和字段注释；复杂执行过程再到同名 .cpp 或指定实现文件阅读。

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
    /// @brief 创建可停靠面板，统一设成可移动、可关闭并允许四个停靠区域；关闭只代表隐藏，不销毁模块对象。
    /// @param title 面板标题，显示在标题栏和标签页上。
    /// @param mainWindow 宿主主窗口，同时作为本 Dock 的 Qt 父对象，由它负责本对象的生命周期。
    /// @return 无（构造函数不返回值）。
    explicit ManagedDockWidget(const QString& title,
                               QMainWindow* mainWindow);

signals:
    /// @brief 用户按下 Dock 关闭按钮时发射，请求主窗口裁决是否隐藏；标签切换不会发射。
    /// @return 无。
    void closeRequested();

protected:
    /// @brief 覆写关闭事件：先 ignore 并发射 closeRequested 交给主窗口裁决，仅在主窗口已把本 Dock 隐藏后才 accept。
    /// @param event Qt 关闭事件对象，只在本次回调期间有效，不允许为空。
    /// @return 无。
    void closeEvent(QCloseEvent* event) override;
};
}
