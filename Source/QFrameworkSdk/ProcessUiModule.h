// 【阅读导引】独立进程 UI 模块基类。
// 阅读顺序：模块在自己的 QApplication 中运行，ProcessRuntime 负责向主程序注册和传输消息。
// 需要分清：跨进程不能直接传 QWidget 指针，宿主通过原生窗口句柄嵌入。
// 这是声明/类型入口：先看 public 接口和字段注释；复杂执行过程再到同名 .cpp 或指定实现文件阅读。

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
