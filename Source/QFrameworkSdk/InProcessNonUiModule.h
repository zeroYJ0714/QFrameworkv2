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
    /// @brief 创建主进程无界面模块基类；参数：parent 是 Qt 所有者；返回值：无。
    /// @param parent Qt 父对象或父控件；用于建立所有权关系，允许为空。
    /// @return 无。
    explicit InProcessNonUiModule(QObject* parent = nullptr);
    /// @brief 通过虚析构完整释放派生模块和 QObject 子对象；参数：无；返回值：无。
    /// @return 无。
    ~InProcessNonUiModule() override;
};
}
