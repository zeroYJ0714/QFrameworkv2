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
    /// @brief 创建子进程无界面模块基类；参数：parent 通常为空，由运行时统一销毁；返回值：无。
    /// @param parent Qt 父对象或父控件；用于建立所有权关系，允许为空。
    /// @return 无。
    explicit ProcessNonUiModule(QObject* parent = nullptr);
    /// @brief 完整析构派生模块和 QObject 子对象；参数：无；返回值：无。
    /// @return 无。
    ~ProcessNonUiModule() override;
};
}
