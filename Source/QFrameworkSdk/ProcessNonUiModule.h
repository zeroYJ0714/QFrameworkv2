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
    /// @brief 创建子进程无界面模块基类，只初始化 QObject；本基类不创建线程，由运行时的事件循环驱动。
    /// @param parent Qt 父对象，通常为空；模块对象由 ProcessRuntime 统一销毁。
    /// @return 无（构造函数不返回值）。
    explicit ProcessNonUiModule(QObject* parent = nullptr);
    /// @brief 虚析构，退出时只释放 QObject 资源；Socket 与共享内存由 ProcessRuntime 收尾。
    /// @return 无（析构函数不返回值）。
    ~ProcessNonUiModule() override;
};
}
