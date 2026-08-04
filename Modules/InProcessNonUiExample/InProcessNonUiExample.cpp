#include "InProcessNonUiExample.h"

#include <QDateTime>
#include <QThread>

#include "Generated/log_messages.pb.h"
#include "Generated/status_messages.pb.h"
#include "MessageTopics.h"

InProcessNonUiExample::InProcessNonUiExample(QObject* parent)
    : qframework::InProcessNonUiModule(parent)
{
    setObjectName(QStringLiteral("InProcessNonUiExample"));
}

QStringList InProcessNonUiExample::publishedTopics() const
{
    return QStringList() << QString::fromLatin1(QFRAMEWORK_LOG_DISPLAY);
}

QStringList InProcessNonUiExample::subscribedTopics() const
{
    return QStringList() << QString::fromLatin1(QFRAMEWORK_STATUS);
}

bool InProcessNonUiExample::onStart()
{
    logInfo(QString::fromUtf8(u8"主进程非 UI 模块已启动"));
    return true;
}

void InProcessNonUiExample::onStop()
{
    logInfo(QString::fromUtf8(u8"主进程非 UI 模块已停止"));
}

void InProcessNonUiExample::onMessage(const QString& topic,
                                      const QString& senderModuleId,
                                      const QByteArray& data)
{
    if (topic != QString::fromLatin1(QFRAMEWORK_STATUS))
        return;

    qframework::protocols::ModuleStatus status;
    if (!status.ParseFromArray(data.constData(), data.size())) {
        logWarning(QString::fromUtf8(u8"解析模块状态消息失败"));
        return;
    }

    qframework::protocols::LogDisplayMessage reply;
    reply.set_level("INFO");
    reply.set_module_id("InProcessNonUiExample");
    reply.set_text("Status received from " + senderModuleId.toStdString());
    reply.set_timestamp_ms(QDateTime::currentMSecsSinceEpoch());
    reply.set_thread_id(reinterpret_cast<quintptr>(QThread::currentThreadId()));

    std::string bytes;
    if (!reply.SerializeToString(&bytes)) {
        logError(QString::fromUtf8(u8"序列化日志显示消息失败"));
        return;
    }
    publish(QString::fromLatin1(QFRAMEWORK_LOG_DISPLAY), QByteArray::fromStdString(bytes));
}
