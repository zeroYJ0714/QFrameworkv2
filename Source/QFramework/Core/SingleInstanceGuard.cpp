// 【阅读导引】防止同一目录重复启动程序。
// 阅读顺序：acquire 先规范化程序目录并计算哈希，再用 Windows CreateMutexW 判断同一会话是否已经运行。
// 需要分清：句柄不是业务互斥锁；持有它代表该目录已有实例。析构关闭句柄，不结束其他程序；不同部署目录互不阻塞。
// 这是执行实现：按入口调用和信号回调追踪，不要按文件行号把所有函数理解成连续执行。

// 文件职责：用程序目录哈希命名 Windows 互斥量，检查同目录是否已有实例。
// 此实现没有 Socket 唤醒消息；析构仅关闭本进程持有的互斥量句柄。
#include "SingleInstanceGuard.h"

#include <QCryptographicHash>
#include <QDir>

// 互斥量名称不能直接使用路径中的反斜杠和冒号，因此先规范化目录，
// 再计算 SHA-256，得到稳定且适合 Win32 命名对象的身份字符串。

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace qframework
{
// 新 Guard 尚未拥有 Win32 互斥量句柄。
/// @brief 构造尚未持有 Win32 互斥量句柄的守卫对象。
/// @return 无（构造函数不返回值）。
SingleInstanceGuard::SingleInstanceGuard()
    : mutexHandle_(nullptr)
{
}

// 释放本实例持有的命名互斥量句柄；Windows 内核对象会在最后句柄关闭后消失。
/// @brief 析构时关闭本进程持有的命名互斥量句柄。
/// @return 无（析构函数不返回值）。
SingleInstanceGuard::~SingleInstanceGuard()
{
#ifdef Q_OS_WIN
    // 只有成功 acquire 后才保存 HANDLE；nullptr 不需要关闭。
    if (mutexHandle_ != nullptr)
        CloseHandle(static_cast<HANDLE>(mutexHandle_));
#endif
}

// 用可执行目录的规范化哈希获取会话内命名互斥量，并区分已运行和系统错误。
/// @brief 按程序目录哈希获取会话内命名互斥量，判定是否已有同目录实例。
/// @param executableDirectory 程序所在目录，用于生成互斥量身份。
/// @param errorMessage 可选的失败原因输出参数，可为空。
/// @return Acquired 表示本进程持有互斥量，AlreadyRunning 表示已有实例，Error 表示系统调用失败。
SingleInstanceResult SingleInstanceGuard::acquire(
    const QString& executableDirectory,
    QString* errorMessage)
{
    // 同一个 Guard 已持有互斥量时无需重复调用 CreateMutexW。
    if (mutexHandle_ != nullptr)
        return SingleInstanceResult::Acquired;

    QDir directory(executableDirectory); ///< 待规范化的程序目录。
    // canonicalPath 能消除符号链接；路径不存在时退回 absolutePath。
    QString identity = directory.canonicalPath(); ///< 目录身份字符串，优先取消除符号链接后的规范路径。
    if (identity.isEmpty())
        identity = directory.absolutePath();
    identity = QDir::fromNativeSeparators(QDir::cleanPath(identity)).toLower();
    if (identity.isEmpty()) { ///< 目录无法解析：上报错误而不是退化成全局单实例。
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"无法确定程序目录");
        return SingleInstanceResult::Error;
    }

    const QByteArray digest = QCryptographicHash::hash(
        identity.toUtf8(), QCryptographicHash::Sha256).toHex(); ///< 目录身份的 SHA-256 十六进制摘要，作为互斥量名后缀。
    const QString mutexName = QStringLiteral("Local\\QFrameworkApp_%1")
        .arg(QString::fromLatin1(digest)); ///< 当前登录会话内唯一的命名互斥量名称。

#ifdef Q_OS_WIN
    // Local\\ 作用域限制在当前登录会话，不与其他 Windows 会话互相阻塞。
    SetLastError(ERROR_SUCCESS);
    HANDLE handle = CreateMutexW(
        nullptr, FALSE, reinterpret_cast<LPCWSTR>(mutexName.utf16())); ///< 新建或打开的命名互斥量句柄。
    if (handle == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QString::fromUtf8(u8"无法创建单实例互斥量，Win32 错误 %1")
                .arg(static_cast<qulonglong>(GetLastError()));
        }
        return SingleInstanceResult::Error;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // 当前 CreateMutexW 返回的 HANDLE 也需要关闭，不能泄漏。
        CloseHandle(handle);
        return SingleInstanceResult::AlreadyRunning;
    }

    mutexHandle_ = handle;
    return SingleInstanceResult::Acquired;
#else
    Q_UNUSED(mutexName)
    if (errorMessage != nullptr)
        *errorMessage = QString::fromUtf8(u8"单实例互斥量仅支持 Windows");
    return SingleInstanceResult::Error;
#endif
}
}
