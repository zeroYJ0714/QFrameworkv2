#pragma once

// 文件职责：提供“子进程、无界面 EXE”模块的 Qt 基类。
// 子进程入口创建 QCoreApplication，再交给 ProcessRuntime 连接父进程。

#include <QObject>

#include "ModuleEndpoint.h"

namespace qframework
{
class QFRAMEWORK_EXPORT ProcessNonUiModule : public QObject, public ModuleEndpoint
{
    Q_OBJECT

public:
    // 父对象通常为 nullptr，由 ProcessRuntime 在停止阶段统一销毁模块。
    explicit ProcessNonUiModule(QObject* parent = nullptr);
    // 虚析构保证子进程退出时派生模块完整析构。
    ~ProcessNonUiModule() override;
};
}
