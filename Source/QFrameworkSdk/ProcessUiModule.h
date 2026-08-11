#pragma once

// 文件职责：提供“子进程、带界面 EXE”模块的 QWidget 基类。
// 窗口句柄会通过 IPC 上报给主进程，再由主进程嵌入 Dock。

#include <QWidget>

#include "ModuleEndpoint.h"

namespace qframework
{
class QFRAMEWORK_EXPORT ProcessUiModule : public QWidget, public ModuleEndpoint
{
    Q_OBJECT

public:
    /// @brief 创建子进程 QWidget 模块基类；参数：parent 是可选父控件；返回值：无。
    /// @param parent Qt 父对象或父控件；用于建立所有权关系，允许为空。
    /// @return 无。
    explicit ProcessUiModule(QWidget* parent = nullptr);
    /// @brief 释放子进程窗口资源；参数：无；返回值：无。
    /// @return 无。
    ~ProcessUiModule() override;
};
}
