#pragma once

#include <QMutex>
#include <QString>
#include <QtGlobal>

#include "LogLevel.h"
#include "QFrameworkGlobal.h"

namespace qframework
{
class QFRAMEWORK_EXPORT Logger
{
public:
    static Logger& instance();

    bool start(const QString& directory,
               qint64 maxFileBytes,
               QString* errorMessage = nullptr);
    void stop();
    void flush();
    bool isRunning() const;

    void log(LogLevel level, const QString& moduleId, const QString& text);
    void installQtMessageHandler();
    void uninstallQtMessageHandler();

private:
    class Worker;

    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static void qtMessageHandler(QtMsgType type,
                                 const QMessageLogContext& context,
                                 const QString& message);

    mutable QMutex mutex_;
    Worker* worker_;
    QtMessageHandler previousQtHandler_;
    bool qtHandlerInstalled_;
};
}
