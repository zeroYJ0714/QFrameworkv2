#include "Logger.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutexLocker>
#include <QQueue>
#include <QThread>
#include <QWaitCondition>

namespace qframework
{
namespace
{
struct LogRecord
{
    QDateTime timestamp;
    LogLevel level;
    QString moduleId;
    quintptr threadId;
    QString text;
};

QString levelName(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug: return QStringLiteral("DEBUG");
    case LogLevel::Info: return QStringLiteral("INFO");
    case LogLevel::Warning: return QStringLiteral("WARNING");
    case LogLevel::Error: return QStringLiteral("ERROR");
    }
    return QStringLiteral("UNKNOWN");
}
}

class Logger::Worker : public QThread
{
public:
    Worker(const QString& directory, qint64 maxFileBytes)
        : directory_(directory),
          maxFileBytes_(maxFileBytes),
          stopping_(false),
          writing_(false),
          fileIndex_(0)
    {
    }

    void enqueue(const LogRecord& record)
    {
        QMutexLocker locker(&mutex_);
        if (stopping_)
            return;
        queue_.enqueue(record);
        available_.wakeOne();
    }

    void flushQueue()
    {
        QMutexLocker locker(&mutex_);
        while (!queue_.isEmpty() || writing_)
            drained_.wait(&mutex_);
    }

    void stopAndWait()
    {
        {
            QMutexLocker locker(&mutex_);
            stopping_ = true;
            available_.wakeOne();
        }
        wait();
    }

protected:
    void run() override
    {
        for (;;) {
            LogRecord record;
            {
                QMutexLocker locker(&mutex_);
                while (queue_.isEmpty() && !stopping_)
                    available_.wait(&mutex_);
                if (queue_.isEmpty() && stopping_)
                    break;
                record = queue_.dequeue();
                writing_ = true;
            }

            writeRecord(record);

            {
                QMutexLocker locker(&mutex_);
                writing_ = false;
                if (queue_.isEmpty())
                    drained_.wakeAll();
            }
        }
        if (file_.isOpen()) {
            file_.flush();
            file_.close();
        }
        QMutexLocker locker(&mutex_);
        writing_ = false;
        drained_.wakeAll();
    }

private:
    QByteArray formattedRecord(const LogRecord& record) const
    {
        QString text = record.text;
        text.replace(QLatin1Char('\r'), QLatin1Char(' '));
        text.replace(QLatin1Char('\n'), QLatin1Char(' '));
        const QString line = QStringLiteral("%1 [%2] [%3] [thread:%4] %5\n")
            .arg(record.timestamp.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                 levelName(record.level),
                 record.moduleId.isEmpty() ? QStringLiteral("Unknown") : record.moduleId,
                 QString::number(record.threadId),
                 text);
        return line.toUtf8();
    }

    QString filePath(const QDate& date, int index) const
    {
        return QDir(directory_).filePath(
            QStringLiteral("QFramework_%1_%2.log")
                .arg(date.toString(QStringLiteral("yyyy-MM-dd")))
                .arg(index, 3, 10, QLatin1Char('0')));
    }

    bool openFile(const QDate& date)
    {
        if (file_.isOpen() && date == fileDate_)
            return true;

        if (file_.isOpen()) {
            file_.flush();
            file_.close();
        }
        fileDate_ = date;
        fileIndex_ = 0;
        while (QFile::exists(filePath(fileDate_, fileIndex_ + 1)))
            ++fileIndex_;
        if (QFile::exists(filePath(fileDate_, fileIndex_)) &&
            QFileInfo(filePath(fileDate_, fileIndex_)).size() >= maxFileBytes_) {
            ++fileIndex_;
        }
        file_.setFileName(filePath(fileDate_, fileIndex_));
        return file_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    }

    void writeRecord(const LogRecord& record)
    {
        const QByteArray bytes = formattedRecord(record);
        if (!openFile(record.timestamp.date()))
            return;
        if (file_.size() > 0 && file_.size() + bytes.size() > maxFileBytes_) {
            file_.flush();
            file_.close();
            ++fileIndex_;
            file_.setFileName(filePath(fileDate_, fileIndex_));
            if (!file_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
                return;
        }
        file_.write(bytes);
        file_.flush();
    }

    QString directory_;
    qint64 maxFileBytes_;
    QMutex mutex_;
    QWaitCondition available_;
    QWaitCondition drained_;
    QQueue<LogRecord> queue_;
    bool stopping_;
    bool writing_;
    QFile file_;
    QDate fileDate_;
    int fileIndex_;
};

Logger& Logger::instance()
{
    static Logger logger;
    return logger;
}

Logger::Logger()
    : worker_(nullptr),
      previousQtHandler_(nullptr),
      qtHandlerInstalled_(false)
{
}

Logger::~Logger()
{
    uninstallQtMessageHandler();
    stop();
}

bool Logger::start(const QString& directory,
                   qint64 maxFileBytes,
                   QString* errorMessage)
{
    QMutexLocker locker(&mutex_);
    if (worker_ != nullptr)
        return true;
    if (directory.isEmpty() || maxFileBytes <= 0) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"日志目录或最大文件大小无效");
        return false;
    }
    if (!QDir().mkpath(directory)) {
        if (errorMessage != nullptr)
            *errorMessage = QString::fromUtf8(u8"无法创建日志目录：%1").arg(directory);
        return false;
    }
    worker_ = new Worker(QDir(directory).absolutePath(), maxFileBytes);
    worker_->start();
    return true;
}

void Logger::stop()
{
    Worker* worker = nullptr;
    {
        QMutexLocker locker(&mutex_);
        worker = worker_;
        worker_ = nullptr;
    }
    if (worker != nullptr) {
        worker->stopAndWait();
        delete worker;
    }
}

void Logger::flush()
{
    QMutexLocker locker(&mutex_);
    if (worker_ != nullptr)
        worker_->flushQueue();
}

bool Logger::isRunning() const
{
    QMutexLocker locker(&mutex_);
    return worker_ != nullptr;
}

void Logger::log(LogLevel level, const QString& moduleId, const QString& text)
{
    LogRecord record;
    record.timestamp = QDateTime::currentDateTime();
    record.level = level;
    record.moduleId = moduleId;
    record.threadId = reinterpret_cast<quintptr>(QThread::currentThreadId());
    record.text = text;

    QMutexLocker locker(&mutex_);
    if (worker_ != nullptr)
        worker_->enqueue(record);
}

void Logger::installQtMessageHandler()
{
    QMutexLocker locker(&mutex_);
    if (qtHandlerInstalled_)
        return;
    previousQtHandler_ = qInstallMessageHandler(&Logger::qtMessageHandler);
    qtHandlerInstalled_ = true;
}

void Logger::uninstallQtMessageHandler()
{
    QMutexLocker locker(&mutex_);
    if (!qtHandlerInstalled_)
        return;
    qInstallMessageHandler(previousQtHandler_);
    previousQtHandler_ = nullptr;
    qtHandlerInstalled_ = false;
}

void Logger::qtMessageHandler(QtMsgType type,
                              const QMessageLogContext& context,
                              const QString& message)
{
    Q_UNUSED(context)
    LogLevel level = LogLevel::Debug;
    switch (type) {
    case QtDebugMsg: level = LogLevel::Debug; break;
    case QtInfoMsg: level = LogLevel::Info; break;
    case QtWarningMsg: level = LogLevel::Warning; break;
    case QtCriticalMsg:
    case QtFatalMsg: level = LogLevel::Error; break;
    }
    Logger::instance().log(level, QStringLiteral("Unknown"), message);
    if (type == QtFatalMsg)
        Logger::instance().flush();
}
}
