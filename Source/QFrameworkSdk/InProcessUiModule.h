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
    // QWidget parent 由框架的 Dock/插件管理器设置。
    explicit InProcessUiModule(QWidget* parent = nullptr);
    // 释放 QWidget 资源，同时通过 ModuleEndpoint 虚析构完成框架解绑。
    ~InProcessUiModule() override;
};
}
