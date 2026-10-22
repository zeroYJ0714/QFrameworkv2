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
    /// @brief 创建守卫对象；此时还没有创建或持有任何互斥体句柄。
    /// @return 无（构造函数不返回值）。
    SingleInstanceGuard();
    // 析构自动 CloseHandle，进程退出后其他实例即可获取。
    /// @brief 关闭互斥体句柄，让后续启动的实例能够重新获取。
    /// @return 无（析构函数不返回值）。
    ~SingleInstanceGuard();

    // 可重复调用；已经成功获取时直接返回 Acquired。
    /// @brief 以程序所在目录为名获取全局互斥体，判断是否已有实例在运行。
    /// @param executableDirectory 程序所在目录，用作互斥体名称基准；
    ///        按目录而非当前工作目录命名，避免从不同目录启动时误判为多实例。
    /// @param errorMessage 可选失败原因输出参数，可为空；失败时写入 Win32 错误描述。
    /// @return 自己首次拿到互斥体返回 Acquired，已有实例返回 AlreadyRunning，
    ///         创建或等待失败返回 Error。
    SingleInstanceResult acquire(const QString& executableDirectory,
                                 QString* errorMessage = nullptr);

private:
    // HANDLE 不能安全复制，因此显式禁止复制构造和赋值。
    /// @brief 禁用拷贝构造：Win32 HANDLE 不可安全复制，复制会导致重复关闭。
    /// @return 无（构造函数不返回值）。
    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    /// @brief 禁用拷贝赋值，理由同拷贝构造。
    /// @return 无（赋值运算符不返回值）。
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    // 头文件避免包含 windows.h，使用 void* 保存 HANDLE。
    void* mutexHandle_; ///< Win32 互斥体 HANDLE，用 void* 保存以避免头文件引入 windows.h；未成功获取时为空。
};
}
