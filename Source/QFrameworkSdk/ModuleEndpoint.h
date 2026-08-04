#pragma once

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QStringList>

#include "LogLevel.h"
#include "QFrameworkGlobal.h"

namespace qframework
{
class ModuleEndpoint;

class QFRAMEWORK_EXPORT ModuleHost
{
public:
    virtual ~ModuleHost() = default;

    virtual bool publishFromModule(const QString& moduleId,
                                   const QString& topic,
                                   const QByteArray& data) = 0;
    virtual void logFromModule(LogLevel level,
                               const QString& moduleId,
                               const QString& text) = 0;
};

class QFRAMEWORK_EXPORT ModuleEndpoint
{
public:
    ModuleEndpoint();
    virtual ~ModuleEndpoint();

    QString moduleId() const;
    virtual QStringList publishedTopics() const;
    virtual QStringList subscribedTopics() const;
    virtual bool onStart();
    virtual void onStop();
    virtual void onMessage(const QString& topic,
                           const QString& senderModuleId,
                           const QByteArray& data);

    bool publish(const QString& topic, const QByteArray& data);
    void logDebug(const QString& text);
    void logInfo(const QString& text);
    void logWarning(const QString& text);
    void logError(const QString& text);

    // 仅供框架在模块启动和停止边界绑定宿主。
    void bindHost(const QString& moduleId, ModuleHost* host);
    void setRunning(bool running);

private:
    void log(LogLevel level, const QString& text);

    QString moduleId_;
    ModuleHost* host_;
    bool running_;
    mutable QMutex mutex_;
};
}
