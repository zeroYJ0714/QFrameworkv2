#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QMessageBox>

#include "FrameworkRuntime.h"
#include "SingleInstanceGuard.h"

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("QFrameworkApp"));
    QCoreApplication::setOrganizationName(QStringLiteral("QFramework"));

    qframework::SingleInstanceGuard singleInstance;
    QString error;
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
    if (!runtime.initialize(configFilePath, &error)) {
        QMessageBox::critical(
            nullptr,
            QString::fromUtf8(u8"框架初始化失败"),
            error);
        return 2;
    }

    runtime.show();
    const int exitCode = application.exec();
    runtime.shutdown();
    return exitCode;
}
