#include <QCoreApplication>

#include "ProcessNonUiExample.h"
#include "ProcessRuntime.h"

// 子进程非 UI 入口的职责很小：创建 QCoreApplication 和模块，
// 其余注册、IPC、心跳、队列和退出顺序全部交给 ProcessRuntime。
int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    // run() 接管模块对象的生命周期；返回值直接作为进程退出码。
    return qframework::ProcessRuntime::run(&application, new ProcessNonUiExample);
}
