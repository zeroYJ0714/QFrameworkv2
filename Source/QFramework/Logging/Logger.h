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
    /// @brief 取得进程内唯一的日志对象。
    /// @return Logger 单例的引用；首次调用时构造，静态存储期由运行库管理。
    static Logger& instance();

    // 使用默认 100 ms 批量刷新间隔启动；兼容旧调用方。
    /// @brief 以固定的 100 ms 刷新周期启动日志（兼容旧调用方式）。
    /// @param directory 日志输出目录。
    /// @param maxFileBytes 单个日志文件的字节上限。
    /// @param errorMessage 可选的失败原因输出参数，可为空。
    /// @return 启动成功返回 true；目录或上限无效、目录创建失败时返回 false。
    bool start(const QString& directory,
               qint64 maxFileBytes,
               QString* errorMessage = nullptr);
    // flushIntervalMs 只影响普通日志的批量落盘频率；显式 flush、Fatal、
    // 滚动和停止仍会立即刷新，不会因为这个间隔丢日志。
    /// @brief 创建日志目录并启动唯一写盘线程，重复调用视为已成功。
    /// @param directory 日志输出目录。
    /// @param maxFileBytes 单个日志文件的字节上限，必须大于 0。
    /// @param flushIntervalMs 普通批次刷新周期，非法值回退为 100 ms。
    /// @param errorMessage 可选的失败原因输出参数，可为空。
    /// @return 启动成功或已在运行返回 true；参数无效或目录创建失败返回 false。
    bool start(const QString& directory,
               qint64 maxFileBytes,
               int flushIntervalMs,
               QString* errorMessage = nullptr);
    // stop 有有限等待，并在返回前尽量写完队列中的尾部记录。
    /// @brief 摘除并停止写盘线程；超时的线程交给 deleteLater 自回收。
    /// @return 无；最多等待约 5 秒。
    void stop();
    // 等待当前队列和文件写入完成，带超时，不会无限阻塞调用线程。
    /// @brief 同步等待队列中的记录全部落盘。
    /// @return 无；等待有超时上限，未启动写盘线程时立即返回。
    void flush();
    // 只读查询 Worker 是否存在，不代表当前队列已经清空。
    /// @brief 查询写盘线程是否仍在运行。
    /// @return 已启动且未停止时返回 true。
    bool isRunning() const;

    // 线程安全地提交一条日志；普通调用不会同步等待磁盘 flush。
    /// @brief 采集时间与线程 ID 后把记录入队，不做任何文件 I/O。
    /// @param level 日志级别。
    /// @param moduleId 记录来源模块 ID。
    /// @param text 原始消息文本。
    /// @return 无；Logger 未启动时静默丢弃。
    void log(LogLevel level, const QString& moduleId, const QString& text);
    // 把 Qt 全局 qDebug/qWarning 等消息转成 Logger 记录。
    /// @brief 安装全局 Qt 消息处理器，把 qDebug 等输出接入 Logger。
    /// @return 无；重复安装时安全忽略。
    void installQtMessageHandler();
    // 恢复安装前的 Qt 消息处理器。
    /// @brief 恢复安装前保存的 Qt 消息处理器。
    /// @return 无；未安装时安全忽略。
    void uninstallQtMessageHandler();

private:
    class Worker;

    /// @brief 创建日志单例；此时写盘线程未启动，记录会被静默丢弃。
    /// @return 无（构造函数不返回值）。
    Logger();
    /// @brief 停止写盘线程并释放 Worker；未启动时安全无操作。
    /// @return 无（析构函数不返回值）。
    ~Logger();
    /// @brief 禁用拷贝构造，保证全局只有一个日志入口。
    /// @return 无（构造函数不返回值）。
    Logger(const Logger&) = delete;
    /// @brief 禁用拷贝赋值，理由同拷贝构造。
    /// @return 无（赋值运算符不返回值）。
    Logger& operator=(const Logger&) = delete;
    /// @brief Qt 全局消息处理器的回调实现，把 qDebug 等输出转成日志记录。
    /// @param type Qt 消息级别，映射为 LogLevel。
    /// @param context Qt 提供的源码位置上下文，本实现只取类别名。
    /// @param message Qt 已格式化好的消息文本。
    /// @return 无。
    static void qtMessageHandler(QtMsgType type,
                                 const QMessageLogContext& context,
                                 const QString& message);

    // mutex_ 只保护 Worker 指针和处理器安装状态，Worker 自己保护记录队列。
    mutable QMutex mutex_; ///< 保护 worker_ 与 qtHandlerInstalled_；不保护 Worker 内部队列。
    Worker* worker_; ///< 写盘 Worker；非空表示写盘线程已启动，由 stop() 摘除并释放。
    QtMessageHandler previousQtHandler_; ///< 安装本 Logger 处理器之前保存的 Qt 消息处理器，卸载时用于恢复。
    bool qtHandlerInstalled_; ///< 是否已接管 Qt 全局消息处理器，避免重复安装和重复恢复。
};
}
