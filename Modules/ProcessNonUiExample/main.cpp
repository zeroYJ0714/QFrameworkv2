#include <QCoreApplication>

#include "ProcessNonUiExample.h"
#include "ProcessRuntime.h"

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    return qframework::ProcessRuntime::run(&application, new ProcessNonUiExample);
}
