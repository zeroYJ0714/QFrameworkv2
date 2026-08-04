#pragma once

// 文件职责：按可执行程序目录创建 Windows 命名互斥量，防止同一部署目录
// 启动多个 QFrameworkApp；不同目录的便携副本仍可各自运行。

#include <QString>

#include "QFrameworkGlobal.h"

namespace qframework
{
enum class SingleInstanceResult
{
    // 当前进程成功持有互斥量。
    Acquired,
    // 同一目录已有进程持有互斥量。
    AlreadyRunning,
    // 路径解析或 Win32 API 调用失败。
    Error
};

class QFRAMEWORK_EXPORT SingleInstanceGuard
{
public:
    // 构造时尚未持有 Win32 HANDLE。
    SingleInstanceGuard();
    // 析构自动 CloseHandle，进程退出后其他实例即可获取。
    ~SingleInstanceGuard();

    // 可重复调用；已经成功获取时直接返回 Acquired。
    SingleInstanceResult acquire(const QString& executableDirectory,
                                 QString* errorMessage = nullptr);

private:
    // HANDLE 不能安全复制，因此显式禁止复制构造和赋值。
    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    // 头文件避免包含 windows.h，使用 void* 保存 HANDLE。
    void* mutexHandle_;
};
}
