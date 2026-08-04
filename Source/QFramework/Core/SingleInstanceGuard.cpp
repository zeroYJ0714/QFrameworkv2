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
SingleInstanceGuard::SingleInstanceGuard()
    : mutexHandle_(nullptr)
{
}

// 释放本实例持有的命名互斥量句柄；Windows 内核对象会在最后句柄关闭后消失。
SingleInstanceGuard::~SingleInstanceGuard()
{
#ifdef Q_OS_WIN
    // 只有成功 acquire 后才保存 HANDLE；nullptr 不需要关闭。
    if (mutexHandle_ != nullptr)
        CloseHandle(static_cast<HANDLE>(mutexHandle_));
#endif
}

// 用可执行目录的规范化哈希获取会话内命名互斥量，并区分已运行和系统错误。
SingleInstanceResult SingleInstanceGuard::acquire(
    const QString& executableDirectory,
    QString* errorMessage)
{
    // 同一个 Guard 已持有互斥量时无需重复调用 CreateMutexW。
    if (mutexHandle_ != nullptr)
        return SingleInstanceResult::Acquired;

    QDir directory(executableDirectory);
    // canonicalPath 能消除符号链接；路径不存在时退回 absolutePath。
    QString identity = directory.canonicalPath();
    if (identity.isEmpty())
        identity = directory.absolutePath();
    identity = QDir::fromNativeSeparators(QDir::cleanPath(identity)).toLower();
    if (identity.isEmpty()) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"无法确定程序目录");
        return SingleInstanceResult::Error;
    }

    const QByteArray digest = QCryptographicHash::hash(
        identity.toUtf8(), QCryptographicHash::Sha256).toHex();
    const QString mutexName = QStringLiteral("Local\\QFrameworkApp_%1")
        .arg(QString::fromLatin1(digest));

#ifdef Q_OS_WIN
    // Local\\ 作用域限制在当前登录会话，不与其他 Windows 会话互相阻塞。
    SetLastError(ERROR_SUCCESS);
    HANDLE handle = CreateMutexW(
        nullptr, FALSE, reinterpret_cast<LPCWSTR>(mutexName.utf16()));
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
