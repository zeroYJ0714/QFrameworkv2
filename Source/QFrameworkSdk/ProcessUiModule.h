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
    /// @brief 创建子进程 UI 模块基类，只初始化 QWidget；窗口句柄上报由 ProcessRuntime 负责。
    /// @param parent Qt 父控件，通常为空；模块窗口在本子进程内创建，父进程只借用原生句柄。
    /// @return 无（构造函数不返回值）。
    explicit ProcessUiModule(QWidget* parent = nullptr);
    /// @brief 虚析构，窗口生命周期始终在本子进程内结束，父进程只借用原生句柄。
    /// @return 无（析构函数不返回值）。
    ~ProcessUiModule() override;
};
}
