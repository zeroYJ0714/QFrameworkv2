#pragma once

#include <QString>

#include "QFrameworkGlobal.h"

namespace qframework
{
enum class SingleInstanceResult
{
    Acquired,
    AlreadyRunning,
    Error
};

class QFRAMEWORK_EXPORT SingleInstanceGuard
{
public:
    SingleInstanceGuard();
    ~SingleInstanceGuard();

    SingleInstanceResult acquire(const QString& executableDirectory,
                                 QString* errorMessage = nullptr);

private:
    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    void* mutexHandle_;
};
}
