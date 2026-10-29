// 【阅读导引】实现四类模块共同的最小构造行为。
// 阅读顺序：InProcess 表示 DLL 在宿主进程；Process 表示独立 EXE；Ui 使用 QWidget，NonUi 使用 QObject。
// 需要分清：这些基类不创建业务设备或数据库。派生模块重写生命周期方法，通用发布能力由 ModuleEndpoint 提供。
// 这是执行实现：按入口调用和信号回调追踪，不要按文件行号把所有函数理解成连续执行。

// 文件职责：提供四类模块基类的最小默认行为（空主题、默认启动/停止和消息忽略）。
// 基类不拥有宿主或业务资源，也不创建线程；具体模块在自己的实现文件中声明主题并覆写
// onStart/onStop/onMessage。默认实现保持幂等，便于测试模块只覆写需要的部分。
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
/// @brief 构造主进程非 UI 模块基类，仅初始化 QObject 父对象。
/// @param parent 所属 QObject，可为空。
/// @return 无（构造函数不返回值）。
InProcessNonUiModule::InProcessNonUiModule(QObject* parent)
    : QObject(parent)
{
}

// 让插件管理器可以通过基类指针安全销毁主进程非 UI 模块。
/// @brief 虚析构，保证通过基类指针删除派生模块时正确析构。
/// @return 无（析构函数不返回值）。
InProcessNonUiModule::~InProcessNonUiModule() = default;

// QWidget 版本把父对象交给 Qt，控件树会随模块一起释放。
/// @brief 构造主进程 UI 模块基类，仅初始化 QWidget 父对象。
/// @param parent 父控件，可为空。
/// @return 无（构造函数不返回值）。
InProcessUiModule::InProcessUiModule(QWidget* parent)
    : QWidget(parent)
{
}

// 虚析构保证 DLL 卸载前完整析构派生 UI 模块。
/// @brief 虚析构，保证卸载 DLL 前完整析构派生 UI 模块。
/// @return 无（析构函数不返回值）。
InProcessUiModule::~InProcessUiModule() = default;

// 子进程非 UI 基类不创建线程；ProcessRuntime 在同一个 Qt 事件循环中驱动它。
/// @brief 构造子进程非 UI 模块基类，仅初始化 QObject 父对象。
/// @param parent 所属 QObject，可为空。
/// @return 无（构造函数不返回值）。
ProcessNonUiModule::ProcessNonUiModule(QObject* parent)
    : QObject(parent)
{
}

// 退出时只释放 QObject 资源，Socket/共享内存由 ProcessRuntime 收尾。
/// @brief 虚析构，退出时只释放 QObject 资源。
/// @return 无（析构函数不返回值）。
ProcessNonUiModule::~ProcessNonUiModule() = default;

// 子进程 UI 基类只负责 QWidget 初始化，窗口句柄上报属于 ProcessRuntime。
/// @brief 构造子进程 UI 模块基类，仅初始化 QWidget 父对象。
/// @param parent 父控件，可为空。
/// @return 无（构造函数不返回值）。
ProcessUiModule::ProcessUiModule(QWidget* parent)
    : QWidget(parent)
{
}

// 父进程只借用原生句柄，真正的 QWidget 生命周期仍在此进程结束。
/// @brief 虚析构，窗口生命周期始终在本子进程内结束。
/// @return 无（析构函数不返回值）。
ProcessUiModule::~ProcessUiModule() = default;
}
