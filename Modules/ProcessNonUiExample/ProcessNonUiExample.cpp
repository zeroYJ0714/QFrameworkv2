#include "ProcessNonUiExample.h"

#include <QDateTime>
#include <QThread>

#include "Generated/log_messages.pb.h"
#include "Generated/status_messages.pb.h"
#include "MessageTopics.h"

ProcessNonUiExample::ProcessNonUiExample(QObject* parent)
    : qframework::ProcessNonUiModule(parent)
{
    setObjectName(QStringLiteral("ProcessNonUiExample"));
}

QStringList ProcessNonUiExample::publishedTopics() const
{
    return QStringList() << QString::fromLatin1(QFRAMEWORK_LOG_DISPLAY);
}

QStringList ProcessNonUiExample::subscribedTopics() const
{
    return QStringList() << QString::fromLatin1(QFRAMEWORK_STATUS);
}

bool ProcessNonUiExample::onStart()
{
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
    logInfo(QString::fromUtf8(u8"子进程非 UI 模块已停止"));
}

void ProcessNonUiExample::onMessage(const QString& topic,
                                    const QString& senderModuleId,
                                    const QByteArray& data)
{
    if (topic != QString::fromLatin1(QFRAMEWORK_STATUS))
        return;
    qframework::protocols::ModuleStatus status;
    if (!status.ParseFromArray(data.constData(), data.size())) {
        logWarning(QString::fromUtf8(u8"解析子进程状态消息失败"));
        return;
    }
    qframework::protocols::LogDisplayMessage reply;
    reply.set_level("INFO");
    reply.set_module_id("ProcessNonUiExample");
    reply.set_text("Status received from " + senderModuleId.toStdString());
    reply.set_timestamp_ms(QDateTime::currentMSecsSinceEpoch());
    reply.set_thread_id(reinterpret_cast<quintptr>(QThread::currentThreadId()));
    std::string bytes;
    if (reply.SerializeToString(&bytes))
        publish(QString::fromLatin1(QFRAMEWORK_LOG_DISPLAY), QByteArray::fromStdString(bytes));
}
