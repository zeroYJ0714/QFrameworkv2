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
    /// @brief 创建主进程 QWidget 模块基类；参数：parent 是 Dock/插件管理器设置的父控件；返回值：无。
    /// @param parent Qt 父对象或父控件；用于建立所有权关系，允许为空。
    /// @return 无。
    explicit InProcessUiModule(QWidget* parent = nullptr);
    /// @brief 释放 QWidget 子树并完成端点析构；参数：无；返回值：无。
    /// @return 无。
    ~InProcessUiModule() override;
};
}
