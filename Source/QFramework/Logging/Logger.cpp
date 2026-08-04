#include "Logger.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QQueue>
#include <QThread>
#include <QWaitCondition>

// 日志数据流：调用线程 -> Worker::queue_ -> batch -> QFile。
// 只有 Worker 线程操作 QFile，因此不会出现多个业务线程同时 flush 的竞态。

namespace qframework
{
namespace
{
struct LogRecord
{
    // 生产线程只创建这份值对象并放入队列；实际 QFile 始终由 Worker 线程独占。
    QDateTime timestamp;
    LogLevel level;
    QString moduleId;
    quintptr threadId;
    QString text;
};

// 将枚举转换成日志文件中的稳定大写名称；未知值使用 UNKNOWN 便于诊断。
QString levelName(LogLevel level)
{
    // 文件格式使用稳定的大写文本，便于人工检索和脚本解析。
    switch (level) {
    case LogLevel::Debug: return QStringLiteral("DEBUG");
    case LogLevel::Info: return QStringLiteral("INFO");
    case LogLevel::Warning: return QStringLiteral("WARNING");
    case LogLevel::Error: return QStringLiteral("ERROR");
    }
    return QStringLiteral("UNKNOWN");
}
}

// 后台写盘线程。
//
// 调用线程只操作受 mutex_ 保护的 queue_；run() 是唯一允许打开、写入、
// 滚动和 flush QFile 的位置。这样既保留异步性能，也明确了文件所有权。
class Logger::Worker : public QThread
{
public:
    // directory/maxFileBytes 决定输出和滚动，flushIntervalMs 决定普通批次期限。
    Worker(const QString& directory, qint64 maxFileBytes, int flushIntervalMs)
        : directory_(directory),
          maxFileBytes_(maxFileBytes),
          flushIntervalMs_(qMax(1, flushIntervalMs)),
          stopping_(false),
          writing_(false),
          fileIndex_(0),
          flushRequestSerial_(0),
          flushedSerial_(0)
    {
        // Worker 线程独占 directory_ 对应的 QFile；flushIntervalMs 非法时在
        // 这里也兜底为正数，避免条件变量收到 0 ms 的忙等。
    }

    // 非阻塞地接收一条记录；停止已经开始时丢弃后来者，避免无限延长退出。
    void enqueue(const LogRecord& record)
    {
        QMutexLocker locker(&mutex_);
        if (stopping_)
            return;
        // 这里仅复制记录并唤醒 Worker，不做文件 I/O，所以业务线程不会被
        // 磁盘速度拖慢；queue_ 受 mutex_ 保护，允许多个线程同时记录。
        queue_.enqueue(record);
        available_.wakeOne();
    }

    // 请求并等待“调用前记录已写入且 QFile 已 flush”，最多等待 5 秒。
    void flushQueue()
    {
        // serial 是“刷新请求的版本号”。调用者记下 target 后等待 Worker 把
        // 至少这个版本的队列写完并 flush；比单纯等待 queue_ 为空更可靠，
        // 因为 Worker 可能正在写一批已经从 queue_ 取走的记录。
        QElapsedTimer timer;
        timer.start();
        QMutexLocker locker(&mutex_);
        const quint64 target = ++flushRequestSerial_;
        available_.wakeOne();
        while ((!queue_.isEmpty() || writing_ || flushedSerial_ < target) &&
               timer.elapsed() < 5000) {
            // 100 ms 的条件变量等待既能及时响应 Worker 的 drained_ 通知，
            // 也能在故障时周期性检查 5 秒硬超时。
            drained_.wait(&mutex_, 100);
        }
    }

    // 唤醒 Worker 写完尾部队列并等待有限时间；极端卡死时才终止线程兜底。
    void stopAndWait()
    {
        {
            QMutexLocker locker(&mutex_);
            // stop 唤醒正常等待和“空队列但即将停止”的分支，让 run() 做完
            // 最后一批后退出；wait 的两个阶段都有限时。
            stopping_ = true;
            available_.wakeAll();
        }
        if (!wait(5000)) {
            terminate();
            wait(1000);
        }
    }

protected:
    // 主循环按批次搬走队列、写记录，并在周期/显式请求/停止边界统一刷新一次。
    void run() override
    {
        QElapsedTimer flushTimer;
        flushTimer.start();
        for (;;) {
            QQueue<LogRecord> batch;
            quint64 requestedSerial = 0;
            bool stopAfterBatch = false;
            {
                QMutexLocker locker(&mutex_);
                // 没有新记录时，等待到下一次刷新期限或新日志到来。普通日志
                // 因此会按间隔成批处理，而不是每条都调用 QFile::flush()。
                if (queue_.isEmpty() && !stopping_ &&
                    flushedSerial_ >= flushRequestSerial_) {
                    const qint64 elapsed = flushTimer.elapsed();
                    const int waitMs = elapsed >= flushIntervalMs_
                        ? 1
                        : qMax(1, flushIntervalMs_ - static_cast<int>(elapsed));
                    available_.wait(&mutex_, static_cast<unsigned long>(waitMs));
                }
                if (!queue_.isEmpty()) {
                    // 先把当前队列整体搬到本地 batch，再释放 mutex；生产者可
                    // 继续入队，Worker 同时写 batch，两个阶段互不阻塞。
                    batch = queue_;
                    queue_.clear();
                    writing_ = true;
                }
                requestedSerial = flushRequestSerial_;
                stopAfterBatch = stopping_ && batch.isEmpty() && queue_.isEmpty();
            }

            for (const LogRecord& record : batch)
                writeRecord(record);

            const bool due = flushTimer.elapsed() >= flushIntervalMs_;
            if (requestedSerial > flushedSerial_ || due || stopAfterBatch) {
                // 一个批次只 flush 一次。显式请求、到期、滚动/停止都会走到
                // 这里，因此这些路径不必等待下一个普通间隔。
                flushFile();
                flushTimer.restart();
            }

            {
                QMutexLocker locker(&mutex_);
                if (requestedSerial > flushedSerial_)
                    // 只有真正完成 QFile::flush() 后才推进版本号，唤醒 flush()
                    // 的调用者，避免“已通知但文件仍在缓冲区”的假完成。
                    flushedSerial_ = requestedSerial;
                writing_ = false;
                if (queue_.isEmpty() && !writing_ &&
                    flushedSerial_ >= flushRequestSerial_)
                    drained_.wakeAll();
                if (stopping_ && queue_.isEmpty())
                    stopAfterBatch = true;
            }
            if (stopAfterBatch)
                break;
        }
        // 退出循环后再做一次兜底 flush，覆盖最后一批和 stop 竞态产生的记录。
        flushFile();
        if (file_.isOpen())
            file_.close();
        QMutexLocker locker(&mutex_);
        writing_ = false;
        flushedSerial_ = flushRequestSerial_;
        drained_.wakeAll();
    }

private:
    // 生成单行 UTF-8 文本；不在这里访问 QFile，便于保持职责单一。
    QByteArray formattedRecord(const LogRecord& record) const
    {
        // 把换行折叠成空格，保证一条 LogRecord 始终对应日志文件一行。
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

    // 根据日期和滚动序号生成当前日志路径。
    QString filePath(const QDate& date, int index) const
    {
        // 文件名同时包含日期和三位序号，按日期/大小滚动时可自然排序。
        return QDir(directory_).filePath(
            QStringLiteral("QFramework_%1_%2.log")
                .arg(date.toString(QStringLiteral("yyyy-MM-dd")))
                .arg(index, 3, 10, QLatin1Char('0')));
    }

    // 确保对应日期文件已打开；换日时先刷新并关闭旧句柄。
    bool openFile(const QDate& date)
    {
        if (file_.isOpen() && date == fileDate_)
            return true;

        if (file_.isOpen()) {
            // 换日或切换文件前立即刷新旧文件，避免尾部记录留在旧句柄缓冲区。
            flushFile();
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

    // 写入一条已格式化记录，并在越过大小阈值前滚动文件。
    void writeRecord(const LogRecord& record)
    {
        const QByteArray bytes = formattedRecord(record);
        if (!openFile(record.timestamp.date()))
            return;
        if (file_.size() > 0 && file_.size() + bytes.size() > maxFileBytes_) {
            // 按大小滚动是立即刷新边界：先落盘旧文件，再打开新文件写当前行。
            flushFile();
            file_.close();
            ++fileIndex_;
            file_.setFileName(filePath(fileDate_, fileIndex_));
            if (!file_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
                return;
        }
        file_.write(bytes);
    }

    // 立即刷新当前打开文件；未打开时是安全空操作。
    void flushFile()
    {
        // QFile::flush() 只在 Worker 线程调用，避免多个线程同时操作同一个句柄。
        if (file_.isOpen())
            file_.flush();
    }

    // 前三个字段是构造后的只读配置；其余字段构成线程同步和文件写入状态。
    QString directory_;
    qint64 maxFileBytes_;
    int flushIntervalMs_;
    QMutex mutex_;
    QWaitCondition available_;
    QWaitCondition drained_;
    QQueue<LogRecord> queue_;
    bool stopping_;
    bool writing_;
    QFile file_;
    QDate fileDate_;
    int fileIndex_;
    quint64 flushRequestSerial_;
    quint64 flushedSerial_;
};

// 使用函数内静态对象实现进程内单例，首次调用时由 C++ 线程安全初始化。
Logger& Logger::instance()
{
    static Logger logger;
    return logger;
}

// 构造时尚未启动 Worker，也尚未替换 Qt 全局消息处理器。
Logger::Logger()
    : worker_(nullptr),
      previousQtHandler_(nullptr),
      qtHandlerInstalled_(false)
{
}

// 析构先恢复 Qt 处理器再停止 Worker，防止退出日志继续进入已销毁线程。
Logger::~Logger()
{
    uninstallQtMessageHandler();
    stop();
}

// 兼容旧调用方式：未提供刷新周期时固定使用 100 ms。
bool Logger::start(const QString& directory,
                   qint64 maxFileBytes,
                   QString* errorMessage)
{
    // 新 Worker 启动成功后，后续 log() 才会接受记录。
    // 旧 API 没有刷新间隔参数，统一委托到新重载并使用计划中的 100 ms。
    return start(directory, maxFileBytes, 100, errorMessage);
}

// 创建输出目录并启动唯一 Worker；重复 start 视为已经成功运行。
bool Logger::start(const QString& directory,
                   qint64 maxFileBytes,
                   int flushIntervalMs,
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
    worker_ = new Worker(QDir(directory).absolutePath(),
                         maxFileBytes,
                         flushIntervalMs > 0 ? flushIntervalMs : 100);
    worker_->start();
    return true;
}

// 从共享指针中摘除 Worker 后再等待，保证并发 log() 不会访问正在析构的对象。
void Logger::stop()
{
    Worker* worker = nullptr;
    {
        QMutexLocker locker(&mutex_);
        worker = worker_;
        worker_ = nullptr;
    }
    if (worker != nullptr) {
        // 先从共享指针中摘除 Worker，阻止新的 log() 把记录交给正在停止的线程，
        // 再等待其写尾并删除对象。
        worker->stopAndWait();
        delete worker;
    }
}

// 把同步刷新请求转给 Worker；Logger 未启动时是安全空操作。
void Logger::flush()
{
    QMutexLocker locker(&mutex_);
    if (worker_ != nullptr)
        // Worker 内部用 serial + drained_ 等待“写入并 flush”完成。
        worker_->flushQueue();
}

// 在 mutex_ 保护下读取 Worker 指针，避免与 stop() 并发数据竞争。
bool Logger::isRunning() const
{
    QMutexLocker locker(&mutex_);
    return worker_ != nullptr;
}

// 在调用线程采集时间和线程 ID 后入队，文件格式化与 I/O 留给 Worker。
void Logger::log(LogLevel level, const QString& moduleId, const QString& text)
{
    LogRecord record;
    record.timestamp = QDateTime::currentDateTime();
    record.level = level;
    record.moduleId = moduleId;
    record.threadId = reinterpret_cast<quintptr>(QThread::currentThreadId());
    record.text = text;

    QMutexLocker locker(&mutex_);
    // 普通日志只入队；批量写入和定时 flush 由 Worker 负责。
    if (worker_ != nullptr)
        worker_->enqueue(record);
}

// 全局安装一次 Qt 消息处理器，并保存原处理器以便恢复。
void Logger::installQtMessageHandler()
{
    QMutexLocker locker(&mutex_);
    if (qtHandlerInstalled_)
        return;
    // 保存旧处理器，卸载时恢复，避免破坏宿主应用原有的 Qt 日志行为。
    previousQtHandler_ = qInstallMessageHandler(&Logger::qtMessageHandler);
    qtHandlerInstalled_ = true;
}

// 恢复 Logger 安装前的处理器；未安装时不做任何操作。
void Logger::uninstallQtMessageHandler()
{
    QMutexLocker locker(&mutex_);
    if (!qtHandlerInstalled_)
        return;
    // qInstallMessageHandler(nullptr/旧指针) 是全局操作，所以由 Logger mutex 串行化。
    qInstallMessageHandler(previousQtHandler_);
    previousQtHandler_ = nullptr;
    qtHandlerInstalled_ = false;
}

// Qt 的静态回调把消息级别映射到 LogLevel；Fatal 写入后额外同步刷新。
void Logger::qtMessageHandler(QtMsgType type,
                              const QMessageLogContext& context,
                              const QString& message)
{
    // Qt Fatal 也先进入统一日志，再显式 flush，避免随后 abort 丢失最后一行。
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
        // Fatal 通常会马上终止进程，不能等待 100 ms 的普通刷新周期。
        Logger::instance().flush();
}
}
