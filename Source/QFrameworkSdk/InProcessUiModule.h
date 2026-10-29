// 【阅读导引】主进程 UI 插件基类。
// 阅读顺序：继承 QWidget 与 ModuleEndpoint；界面放入宿主 Dock，通过端点发布消息。
// 需要分清：QObject 的父子树管理控件寿命，消息回调不保证运行在 GUI 线程。
// 这是声明/类型入口：先看 public 接口和字段注释；复杂执行过程再到同名 .cpp 或指定实现文件阅读。

#pragma once

// 文件职责：提供“主进程、带界面 DLL”模块的 QWidget 基类。
// 该 QWidget 最终会被放进主窗口的受控 Dock 中，而不是独立创建 Socket。

#include <QWidget>

#include "ModuleEndpoint.h"

namespace qframework
{
class QFRAMEWORK_EXPORT InProcessUiModule : public QWidget, public ModuleEndpoint
{
    Q_OBJECT

public:
    /// @brief 创建主进程 UI 模块基类，只初始化 QWidget；此时尚未加入宿主 Dock。
    /// @param parent Qt 父控件，可为空；插件由 QPluginLoader 构造时通常为空，非空时并入 Qt 父子树。
    /// @return 无（构造函数不返回值）。
    explicit InProcessUiModule(QWidget* parent = nullptr);
    /// @brief 虚析构，保证卸载 DLL 前完整析构派生模块的控件子树。
    /// @return 无（析构函数不返回值）。
    ~InProcessUiModule() override;
};
}
