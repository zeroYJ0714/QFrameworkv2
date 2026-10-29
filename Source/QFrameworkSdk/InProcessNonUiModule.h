// 【阅读导引】主进程后台插件基类。
// 阅读顺序：继承 QObject 与 ModuleEndpoint，适合 SQL/ADB 服务；需要后台线程时由派生模块创建。
// 需要分清：NonUi 只表示没有窗口，不表示自动拥有工作线程。
// 这是声明/类型入口：先看 public 接口和字段注释；复杂执行过程再到同名 .cpp 或指定实现文件阅读。

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
