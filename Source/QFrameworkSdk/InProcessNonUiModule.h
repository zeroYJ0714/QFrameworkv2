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
    // parent 由插件管理器决定；模块通常不需要自己管理 QObject 生命周期。
    explicit InProcessNonUiModule(QObject* parent = nullptr);
    // 虚析构保证通过 ModuleEndpoint 指针销毁派生模块时行为正确。
    ~InProcessNonUiModule() override;
};
}
