#include <QApplication>

#include "ProcessRuntime.h"
#include "ProcessUiExample.h"

// 子进程 UI 入口创建 QApplication；ProcessRuntime 随后负责连接父进程、
// 应用样式、调用模块生命周期并管理退出码。
int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    return qframework::ProcessRuntime::run(&application, new ProcessUiExample);
}
