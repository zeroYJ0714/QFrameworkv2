#include "ProcessProtocol.h"

#include <QDataStream>
#include <QJsonDocument>
#include <QJsonParseError>

namespace qframework
{
namespace process
{
QByteArray encodeFrame(const QJsonObject& object)
{
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QByteArray frame;
    frame.reserve(static_cast<int>(sizeof(quint32)) + payload.size());
    QDataStream stream(&frame, QIODevice::WriteOnly);
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
    if (buffer == nullptr || object == nullptr || maxFrameBytes <= 0) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral("IPC 帧参数无效");
        return FrameResult::Invalid;
    }
    if (buffer->size() < static_cast<int>(sizeof(quint32)))
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
        return FrameResult::Incomplete;

    const QByteArray payload = buffer->mid(static_cast<int>(sizeof(quint32)),
                                           static_cast<int>(payloadSize));
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
