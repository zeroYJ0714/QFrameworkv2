#pragma once

// 文件职责：提供“主进程、无界面 DLL”模块的最小 Qt 基类。
// 这类模块由 QPluginLoader 加载，生命周期和 MessageBus 都在主进程内。

#include <QObject>

#include "ModuleEndpoint.h"

namespace qframework
{
class QFRAMEWORK_EXPORT InProcessNonUiModule : public QObject, public ModuleEndpoint
{
    Q_OBJECT

public:
    /// @brief 创建主进程无界面模块基类，只初始化 QObject；本基类不创建任何线程。
    /// @param parent Qt 父对象，可为空；插件由 QPluginLoader 构造时通常为空，非空时并入 Qt 父子树。
    /// @return 无（构造函数不返回值）。
    explicit InProcessNonUiModule(QObject* parent = nullptr);
    /// @brief 虚析构，让插件管理器能通过基类指针安全销毁派生模块及其 QObject 子对象。
    /// @return 无（析构函数不返回值）。
    ~InProcessNonUiModule() override;
};
}
