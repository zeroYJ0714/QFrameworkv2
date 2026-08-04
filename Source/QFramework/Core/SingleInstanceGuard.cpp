#include "SingleInstanceGuard.h"

#include <QCryptographicHash>
#include <QDir>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace qframework
{
SingleInstanceGuard::SingleInstanceGuard()
    : mutexHandle_(nullptr)
{
}

SingleInstanceGuard::~SingleInstanceGuard()
{
#ifdef Q_OS_WIN
    if (mutexHandle_ != nullptr)
        CloseHandle(static_cast<HANDLE>(mutexHandle_));
#endif
}

SingleInstanceResult SingleInstanceGuard::acquire(
    const QString& executableDirectory,
    QString* errorMessage)
{
    if (mutexHandle_ != nullptr)
        return SingleInstanceResult::Acquired;

    QDir directory(executableDirectory);
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
