#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QMessageBox>

#include "FrameworkRuntime.h"
#include "SingleInstanceGuard.h"

// 应用入口只负责组装 Qt 应用对象和 FrameworkRuntime。
// 具体的配置、日志、插件、子进程和窗口生命周期都集中在 runtime 中，
// 这样入口代码不会重复实现框架规则。
int main(int argc, char* argv[])
{
    // QApplication 必须先于任何 QWidget 和框架 UI 对象创建。
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("QFrameworkApp"));
    QCoreApplication::setOrganizationName(QStringLiteral("QFramework"));

    qframework::SingleInstanceGuard singleInstance;
    QString error;
    // 锁定“程序所在目录”而不是当前工作目录，避免从不同目录启动时
    // 意外产生多个实例。
    const qframework::SingleInstanceResult instanceResult = singleInstance.acquire(
        QCoreApplication::applicationDirPath(), &error);
    if (instanceResult == qframework::SingleInstanceResult::AlreadyRunning) {
        QMessageBox::information(
            nullptr,
            QStringLiteral("QFramework"),
            QString::fromUtf8(u8"程序已在运行"));
        return 0;
    }
    if (instanceResult == qframework::SingleInstanceResult::Error) {
        QMessageBox::critical(
            nullptr,
            QString::fromUtf8(u8"启动失败"),
            error);
        return 1;
    }

    const QString configFilePath = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("config/QFramework.ini"));
    qframework::FrameworkRuntime runtime(&application);
    // initialize 负责读取只读 INI、启动日志和模块；失败时不进入事件循环。
    if (!runtime.initialize(configFilePath, &error)) {
        QMessageBox::critical(
            nullptr,
            QString::fromUtf8(u8"框架初始化失败"),
            error);
        return 2;
    }

    runtime.show();
    // Qt 事件循环驱动窗口、Socket、定时器和模块回调。
    const int exitCode = application.exec();
    // exec 返回后再显式 shutdown，覆盖用户关闭窗口和系统退出两种路径。
    runtime.shutdown();
    return exitCode;
}
