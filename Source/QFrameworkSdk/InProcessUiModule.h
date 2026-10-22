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
