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
    /// @brief 执行 `instance` 所定义的类职责。
    /// @return 返回声明类型的结果值。
    static Logger& instance();

    // 使用默认 100 ms 批量刷新间隔启动；兼容旧调用方。
    /// @brief 启动 `start` 对应的框架操作。
    /// @param directory 传给该操作的 `directory` 参数；取值应符合声明类型和函数用途。
    /// @param maxFileBytes 传给该操作的 `maxFileBytes` 参数；取值应符合声明类型和函数用途。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool start(const QString& directory,
               qint64 maxFileBytes,
               QString* errorMessage = nullptr); ///< `errorMessage` 对应的对象指针；所有权和线程归属见本类文件级说明。
    // flushIntervalMs 只影响普通日志的批量落盘频率；显式 flush、Fatal、
    // 滚动和停止仍会立即刷新，不会因为这个间隔丢日志。
    /// @brief 启动 `start` 对应的框架操作。
    /// @param directory 传给该操作的 `directory` 参数；取值应符合声明类型和函数用途。
    /// @param maxFileBytes 传给该操作的 `maxFileBytes` 参数；取值应符合声明类型和函数用途。
    /// @param flushIntervalMs 传给该操作的 `flushIntervalMs` 参数；取值应符合声明类型和函数用途。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool start(const QString& directory,
               qint64 maxFileBytes,
               int flushIntervalMs,
               QString* errorMessage = nullptr); ///< `errorMessage` 对应的对象指针；所有权和线程归属见本类文件级说明。
    // stop 有有限等待，并在返回前尽量写完队列中的尾部记录。
    /// @brief 停止 `stop` 对应的框架操作。
    /// @return 无。
    void stop();
    // 等待当前队列和文件写入完成，带超时，不会无限阻塞调用线程。
    /// @brief 执行 `flush` 所定义的类职责。
    /// @return 无。
    void flush();
    // 只读查询 Worker 是否存在，不代表当前队列已经清空。
    /// @brief 检查 `isRunning` 对应的框架操作。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool isRunning() const;

    // 线程安全地提交一条日志；普通调用不会同步等待磁盘 flush。
    /// @brief 记录 `log` 对应的框架操作。
    /// @param level 日志严重级别。
    /// @param moduleId 稳定模块 ID。
    /// @param text 待显示、发送或记录的文本。
    /// @return 无。
    void log(LogLevel level, const QString& moduleId, const QString& text);
    // 把 Qt 全局 qDebug/qWarning 等消息转成 Logger 记录。
    /// @brief 执行 `installQtMessageHandler` 所定义的类职责。
    /// @return 无。
    void installQtMessageHandler();
    // 恢复安装前的 Qt 消息处理器。
    /// @brief 执行 `uninstallQtMessageHandler` 所定义的类职责。
    /// @return 无。
    void uninstallQtMessageHandler();

private:
    class Worker;

    /// @brief 创建 Logger 对象并初始化其基础状态。
    /// @return 无（构造函数不返回值）。
    Logger();
    /// @brief 销毁 Logger 对象并释放其拥有的资源。
    /// @return 无（析构函数不返回值）。
    ~Logger();
    /// @brief 创建 Logger 对象并初始化其基础状态。
    /// @return 无（构造函数不返回值）。
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /// @brief 执行 `qtMessageHandler` 所定义的类职责。
    /// @param type 传给该操作的 `type` 参数；取值应符合声明类型和函数用途。
    /// @param context 传给该操作的 `context` 参数；取值应符合声明类型和函数用途。
    /// @param message 平台原生消息指针。
    /// @return 无。
    static void qtMessageHandler(QtMsgType type,
                                 const QMessageLogContext& context,
                                 const QString& message);

    // mutex_ 只保护 Worker 指针和处理器安装状态，Worker 自己保护记录队列。
    mutable QMutex mutex_; ///< 保护同一注释分组内共享状态的互斥量。
    Worker* worker_; ///< `worker_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QtMessageHandler previousQtHandler_; ///< 保存 `previousQtHandler` 对应的对象状态或配置值。
    bool qtHandlerInstalled_; ///< `qtHandlerInstalled_` 对应的布尔状态标志。
};
}
