#include "InProcessNonUiExample.h"

#include <QDateTime>
#include <QThread>

#include "Generated/log_messages.pb.h"
#include "Generated/status_messages.pb.h"
#include "MessageTopics.h"

// 本文件演示最小的“状态消息 -> 日志显示消息”转换流程：
// 1) 主进程插件声明主题；2) MessageBus 异步调用 onMessage；
// 3) Protobuf 解码后重新编码回复；4) publish 交回中央总线。
InProcessNonUiExample::InProcessNonUiExample(QObject* parent)
    : qframework::InProcessNonUiModule(parent)
{
    setObjectName(QStringLiteral("InProcessNonUiExample"));
}

QStringList InProcessNonUiExample::publishedTopics() const
{
    // 只发布日志显示主题，框架会在注册时检查该声明。
    return QStringList() << QString::fromLatin1(QFRAMEWORK_LOG_DISPLAY);
}

QStringList InProcessNonUiExample::subscribedTopics() const
{
    // 只接收状态主题，其他主题即使到达也会被忽略。
    return QStringList() << QString::fromLatin1(QFRAMEWORK_STATUS);
}

bool InProcessNonUiExample::onStart()
{
    // onStart 在模块被标记为可运行后调用；这里仅记录启动，不主动阻塞。
    logInfo(QString::fromUtf8(u8"主进程非 UI 模块已启动"));
    return true;
}

void InProcessNonUiExample::onStop()
{
    // 停止回调应只做轻量清理；框架随后会解除插件和 MessageBus 绑定。
    logInfo(QString::fromUtf8(u8"主进程非 UI 模块已停止"));
}

void InProcessNonUiExample::onMessage(const QString& topic,
                                      const QString& senderModuleId,
                                      const QByteArray& data)
{
    // onMessage 可能在框架的消息线程执行，不能假设它是 GUI 线程。
    if (topic != QString::fromLatin1(QFRAMEWORK_STATUS))
        return;

    qframework::protocols::ModuleStatus status;
    // Protobuf 解析失败时不能使用半解析对象继续业务处理。
    if (!status.ParseFromArray(data.constData(), data.size())) {
        logWarning(QString::fromUtf8(u8"解析模块状态消息失败"));
        return;
    }

    qframework::protocols::LogDisplayMessage reply;
    // 回复只携带演示所需的来源和时间信息，实际 payload 仍由 QByteArray 传输。
    reply.set_level("INFO");
    reply.set_module_id("InProcessNonUiExample");
    reply.set_text("Status received from " + senderModuleId.toStdString());
    reply.set_timestamp_ms(QDateTime::currentMSecsSinceEpoch());
    reply.set_thread_id(reinterpret_cast<quintptr>(QThread::currentThreadId()));

    std::string bytes;
    // SerializeToString 失败时不发布空的伪消息，避免下游误判为有效日志。
    if (!reply.SerializeToString(&bytes)) {
        logError(QString::fromUtf8(u8"序列化日志显示消息失败"));
        return;
    }
    publish(QString::fromLatin1(QFRAMEWORK_LOG_DISPLAY), QByteArray::fromStdString(bytes));
}
