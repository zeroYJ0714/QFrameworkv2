#include "ModuleEndpoint.h"

#include <QMutexLocker>

namespace qframework
{
ModuleEndpoint::ModuleEndpoint()
    : host_(nullptr),
      running_(false)
{
}

ModuleEndpoint::~ModuleEndpoint() = default;

QString ModuleEndpoint::moduleId() const
{
    QMutexLocker locker(&mutex_);
    return moduleId_;
}

QStringList ModuleEndpoint::publishedTopics() const
{
    return QStringList();
}

QStringList ModuleEndpoint::subscribedTopics() const
{
    return QStringList();
}

bool ModuleEndpoint::onStart()
{
    return true;
}

void ModuleEndpoint::onStop()
{
}

void ModuleEndpoint::onMessage(const QString& topic,
                               const QString& senderModuleId,
                               const QByteArray& data)
{
    Q_UNUSED(topic)
    Q_UNUSED(senderModuleId)
    Q_UNUSED(data)
}

bool ModuleEndpoint::publish(const QString& topic, const QByteArray& data)
{
    ModuleHost* host = nullptr;
    QString id;
    {
        QMutexLocker locker(&mutex_);
        if (host_ == nullptr || !running_)
            return false;
        host = host_;
        id = moduleId_;
    }

    return host->publishFromModule(id, topic, data);
}

void ModuleEndpoint::logDebug(const QString& text)
{
    log(LogLevel::Debug, text);
}

void ModuleEndpoint::logInfo(const QString& text)
{
    log(LogLevel::Info, text);
}

void ModuleEndpoint::logWarning(const QString& text)
{
    log(LogLevel::Warning, text);
}

void ModuleEndpoint::logError(const QString& text)
{
    log(LogLevel::Error, text);
}

void ModuleEndpoint::bindHost(const QString& moduleId, ModuleHost* host)
{
    QMutexLocker locker(&mutex_);
    moduleId_ = moduleId;
    host_ = host;
}

void ModuleEndpoint::setRunning(bool running)
{
    QMutexLocker locker(&mutex_);
    running_ = running;
}

void ModuleEndpoint::log(LogLevel level, const QString& text)
{
    ModuleHost* host = nullptr;
    QString id;
    {
        QMutexLocker locker(&mutex_);
        host = host_;
        id = moduleId_;
    }
    if (host != nullptr)
        host->logFromModule(level, id, text);
}
}
