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
    // 窗口对象由模块创建，ProcessRuntime 负责在生命周期边界显示和停止。
    explicit ProcessUiModule(QWidget* parent = nullptr);
    // 释放窗口资源；父进程只保存句柄，不拥有这个 QWidget。
    ~ProcessUiModule() override;
};
}
