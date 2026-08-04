#include <QApplication>

#include "ProcessRuntime.h"
#include "ProcessUiExample.h"

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    return qframework::ProcessRuntime::run(&application, new ProcessUiExample);
}
