// 文件职责：创建 Qt 应用、初始化框架运行时并进入 GUI 事件循环。
// QApplication 和所有顶层 QWidget 都属于主 GUI 线程；本文件不直接执行 SQLite、ADB
// 或媒体解码。框架对象在事件循环开始前完成配置，退出时先按统一关闭门禁清理模块，
// 最后由 QApplication::exec() 返回进程退出码。
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QMessageBox>

#include "FrameworkRuntime.h"
#include "SingleInstanceGuard.h"

// 应用入口只负责组装 Qt 应用对象和 FrameworkRuntime。
// 具体的配置、日志、插件、子进程和窗口生命周期都集中在 runtime 中，
// 这样入口代码不会重复实现框架规则。
/// @brief 程序入口：组装 Qt 应用与框架运行时，跑完一次完整的启动/退出生命周期。
/// @param argc 命令行参数个数，由 Qt 转交。
/// @param argv 命令行参数数组，由 Qt 转交。
/// @return 进程退出码：0 表示已有实例或正常退出，1 表示单实例检查失败，2 表示框架初始化失败。
int main(int argc, char* argv[])
{
    // QApplication 必须先于任何 QWidget 和框架 UI 对象创建。
    QApplication application(argc, argv); ///< Qt 应用对象，持有 GUI 线程和全局事件分发。
    QCoreApplication::setApplicationName(QStringLiteral("QFrameworkApp")); ///< 应用名，影响配置目录与日志标识。
    QCoreApplication::setOrganizationName(QStringLiteral("QFramework")); ///< 组织名，影响 QSettings 等 Qt 全局路径。

    qframework::SingleInstanceGuard singleInstance; ///< 单实例守卫，持有跨进程锁直到本函数返回。
    QString error; ///< 承接各步骤的失败原因，仅在返回非零退出码时被读取。
    // 锁定“程序所在目录”而不是当前工作目录，避免从不同目录启动时
    // 意外产生多个实例。
    const qframework::SingleInstanceResult instanceResult = singleInstance.acquire(
        QCoreApplication::applicationDirPath(), &error); ///< 单实例检查结果，决定是否继续启动。
    if (instanceResult == qframework::SingleInstanceResult::AlreadyRunning) { ///< 已有实例：直接退出，不再初始化框架。
        QMessageBox::information(
            nullptr,
            QStringLiteral("QFramework"),
            QString::fromUtf8(u8"程序已在运行"));
        return 0;
    }
    if (instanceResult == qframework::SingleInstanceResult::Error) { ///< 锁失败：报告具体原因后退出。
        QMessageBox::critical(
            nullptr,
            QString::fromUtf8(u8"启动失败"),
            error);
        return 1;
    }

    const QString configFilePath = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("config/QFramework.ini")); ///< 只读运行配置的绝对路径，基准是程序所在目录。
    qframework::FrameworkRuntime runtime(&application); ///< 框架运行时，负责配置、日志、插件和主窗口。
    // initialize 负责读取只读 INI、启动日志和模块；失败时不进入事件循环。
    if (!runtime.initialize(configFilePath, &error)) { ///< 初始化失败：弹出错误并返回，不进入事件循环。
        QMessageBox::critical(
            nullptr,
            QString::fromUtf8(u8"框架初始化失败"),
            error);
        return 2;
    }

    runtime.show(); ///< 显示主窗口；此时模块和 Dock 已按配置装配完成。
    // Qt 事件循环驱动窗口、Socket、定时器和模块回调。
    const int exitCode = application.exec(); ///< 事件循环返回码，通常为用户关闭窗口时的 0。
    // exec 返回后再显式 shutdown，覆盖用户关闭窗口和系统退出两种路径。
    runtime.shutdown(); ///< 按统一关闭门禁回收模块、线程和子进程。
    return exitCode;
}
