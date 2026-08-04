#include "InProcessNonUiModule.h"
#include "InProcessUiModule.h"
#include "ProcessNonUiModule.h"
#include "ProcessUiModule.h"

// 四个基类只连接 Qt 的 QObject/QWidget 生命周期与 ModuleEndpoint 接口；
// 真正的消息和日志行为由 ModuleEndpoint 及其宿主实现。

namespace qframework
{
// 主进程非 UI 基类只初始化 QObject；ModuleEndpoint 的无界面默认行为由
// 多重继承中的另一部分负责，二者不重复保存状态。
InProcessNonUiModule::InProcessNonUiModule(QObject* parent)
    : QObject(parent)
{
}

// 让插件管理器可以通过基类指针安全销毁主进程非 UI 模块。
InProcessNonUiModule::~InProcessNonUiModule() = default;

// QWidget 版本把父对象交给 Qt，控件树会随模块一起释放。
InProcessUiModule::InProcessUiModule(QWidget* parent)
    : QWidget(parent)
{
}

// 虚析构保证 DLL 卸载前完整析构派生 UI 模块。
InProcessUiModule::~InProcessUiModule() = default;

// 子进程非 UI 基类不创建线程；ProcessRuntime 在同一个 Qt 事件循环中驱动它。
ProcessNonUiModule::ProcessNonUiModule(QObject* parent)
    : QObject(parent)
{
}

// 退出时只释放 QObject 资源，Socket/共享内存由 ProcessRuntime 收尾。
ProcessNonUiModule::~ProcessNonUiModule() = default;

// 子进程 UI 基类只负责 QWidget 初始化，窗口句柄上报属于 ProcessRuntime。
ProcessUiModule::ProcessUiModule(QWidget* parent)
    : QWidget(parent)
{
}

// 父进程只借用原生句柄，真正的 QWidget 生命周期仍在此进程结束。
ProcessUiModule::~ProcessUiModule() = default;
}
