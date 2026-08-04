#include "ProcessNonUiExample.h"

#include <QDateTime>
#include <QThread>

#include "Generated/log_messages.pb.h"
#include "Generated/status_messages.pb.h"
#include "MessageTopics.h"

// 本示例与主进程非 UI 示例使用相同的主题契约，差别在于所有消息先经过
// QLocalSocket；模块代码本身仍只面对 publish/onMessage。
ProcessNonUiExample::ProcessNonUiExample(QObject* parent)
    : qframework::ProcessNonUiModule(parent)
{
    setObjectName(QStringLiteral("ProcessNonUiExample"));
}

QStringList ProcessNonUiExample::publishedTopics() const
{
    // 注册时告知父进程：本模块允许发布日志显示消息。
    return QStringList() << QString::fromLatin1(QFRAMEWORK_LOG_DISPLAY);
}

QStringList ProcessNonUiExample::subscribedTopics() const
{
    // 注册时告知父进程：本模块接收状态消息。
    return QStringList() << QString::fromLatin1(QFRAMEWORK_STATUS);
}

bool ProcessNonUiExample::onStart()
{
    // 构造一条较大的日志消息，便于运行示例时观察共享内存传输阈值。
    qframework::protocols::LogDisplayMessage message;
    message.set_level("INFO");
    message.set_module_id("ProcessNonUiExample");
    message.set_text(std::string("ProcessNonUiExample ready ") + std::string(2048, 'X'));
    message.set_timestamp_ms(QDateTime::currentMSecsSinceEpoch());
    message.set_thread_id(reinterpret_cast<quintptr>(QThread::currentThreadId()));
    std::string bytes;
    if (!message.SerializeToString(&bytes))
        return false;
    logInfo(QString::fromUtf8(u8"子进程非 UI 模块已启动"));
    return publish(QString::fromLatin1(QFRAMEWORK_LOG_DISPLAY), QByteArray::fromStdString(bytes));
}

void ProcessNonUiExample::onStop()
{
    // stopAck 发送前由 ProcessRuntime 调用，不能在这里自行退出进程。
    logInfo(QString::fromUtf8(u8"子进程非 UI 模块已停止"));
}

void ProcessNonUiExample::onMessage(const QString& topic,
                                    const QString& senderModuleId,
                                    const QByteArray& data)
{
    // 此函数在 ProcessRuntime 的输入消息线程执行；不要直接依赖 GUI 对象。
    if (topic != QString::fromLatin1(QFRAMEWORK_STATUS))
        return;
    qframework::protocols::ModuleStatus status;
    if (!status.ParseFromArray(data.constData(), data.size())) {
        logWarning(QString::fromUtf8(u8"解析子进程状态消息失败"));
        return;
    }
    qframework::protocols::LogDisplayMessage reply;
    // 解析成功后把发送者 ID 写入回复，方便观察跨进程方向。
    reply.set_level("INFO");
    reply.set_module_id("ProcessNonUiExample");
    reply.set_text("Status received from " + senderModuleId.toStdString());
    reply.set_timestamp_ms(QDateTime::currentMSecsSinceEpoch());
    reply.set_thread_id(reinterpret_cast<quintptr>(QThread::currentThreadId()));
    std::string bytes;
    if (reply.SerializeToString(&bytes))
        publish(QString::fromLatin1(QFRAMEWORK_LOG_DISPLAY), QByteArray::fromStdString(bytes));
}
