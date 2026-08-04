#include "ProcessProtocol.h"

#include <QDataStream>
#include <QJsonDocument>
#include <QJsonParseError>

// 解析器支持 Socket 的常见情况：半帧、多帧粘连和错误帧。
// maxFrameBytes 在复制 payload 前检查，防止伪造长度导致内存失控。

namespace qframework
{
namespace process
{
QByteArray encodeFrame(const QJsonObject& object)
{
    // Compact JSON 减少控制帧大小；二进制 payload 由上层决定 inline/shared。
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QByteArray frame;
    frame.reserve(static_cast<int>(sizeof(quint32)) + payload.size());
    QDataStream stream(&frame, QIODevice::WriteOnly);
    // 固定 Qt 5.15 和大端序，使父子进程的长度编码保持一致。
    stream.setVersion(QDataStream::Qt_5_15);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << static_cast<quint32>(payload.size());
    frame.append(payload);
    return frame;
}

FrameResult takeFrame(QByteArray* buffer,
                      QJsonObject* object,
                      int maxFrameBytes,
                      QString* errorMessage)
{
    // 指针和上限是调用契约，任何一个无效都不能继续解析。
    if (buffer == nullptr || object == nullptr || maxFrameBytes <= 0) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("IPC 帧参数无效");
        return FrameResult::Invalid;
    }
    if (buffer->size() < static_cast<int>(sizeof(quint32)))
        // 连 4 字节长度头都不完整，保留原缓冲区等待下一次 readyRead。
        return FrameResult::Incomplete;

    QDataStream stream(*buffer);
    stream.setVersion(QDataStream::Qt_5_15);
    stream.setByteOrder(QDataStream::BigEndian);
    quint32 payloadSize = 0;
    stream >> payloadSize;
    if (stream.status() != QDataStream::Ok || payloadSize > static_cast<quint32>(maxFrameBytes)) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("IPC 帧大小超过限制");
        return FrameResult::Invalid;
    }

    const int frameSize = static_cast<int>(sizeof(quint32)) + static_cast<int>(payloadSize);
    if (buffer->size() < frameSize)
        // 已知长度但 payload 未收全，同样不消费任何字节。
        return FrameResult::Incomplete;

    const QByteArray payload = buffer->mid(static_cast<int>(sizeof(quint32)),
                                           static_cast<int>(payloadSize));
    // 只在完整帧到齐后移除字节，后面的粘连帧继续留在 buffer 中。
    buffer->remove(0, frameSize);
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("IPC 帧 JSON 无效");
        return FrameResult::Invalid;
    }
    *object = document.object();
    return FrameResult::Ready;
}
}
}
