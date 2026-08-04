#pragma once

// 文件职责：提供线程安全的异步集中日志接口。
// 调用线程只提交 LogRecord，Worker 独占 QFile；这样业务、Qt 消息处理器和
// 子进程日志都使用同一套滚动、批量刷新和停止规则。

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
    // 返回进程内唯一 Logger；对象由静态存储期管理，调用方不 delete。
    static Logger& instance();

    // 使用默认 100 ms 批量刷新间隔启动；兼容旧调用方。
    bool start(const QString& directory,
               qint64 maxFileBytes,
               QString* errorMessage = nullptr);
    // flushIntervalMs 只影响普通日志的批量落盘频率；显式 flush、Fatal、
    // 滚动和停止仍会立即刷新，不会因为这个间隔丢日志。
    bool start(const QString& directory,
               qint64 maxFileBytes,
               int flushIntervalMs,
               QString* errorMessage = nullptr);
    // stop 有有限等待，并在返回前尽量写完队列中的尾部记录。
    void stop();
    // 等待当前队列和文件写入完成，带超时，不会无限阻塞调用线程。
    void flush();
    // 只读查询 Worker 是否存在，不代表当前队列已经清空。
    bool isRunning() const;

    // 线程安全地提交一条日志；普通调用不会同步等待磁盘 flush。
    void log(LogLevel level, const QString& moduleId, const QString& text);
    // 把 Qt 全局 qDebug/qWarning 等消息转成 Logger 记录。
    void installQtMessageHandler();
    // 恢复安装前的 Qt 消息处理器。
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

    // mutex_ 只保护 Worker 指针和处理器安装状态，Worker 自己保护记录队列。
    mutable QMutex mutex_;
    Worker* worker_;
    QtMessageHandler previousQtHandler_;
    bool qtHandlerInstalled_;
};
}
