// 【编码约定】MSVC v141 在 /utf-8 下把 u"" 前缀与窄字面量拼接时，会改用系统 ANSI
// 代码页(936) 解码字节，使 QStringLiteral("中文") 产出乱码并触发 warning C4819。
// 故本文件所有含非 ASCII 的 QStringLiteral 一律写成 QStringLiteral(u"中文")：
// 两个 u 字面量直接拼接，不经过字符集转换。新增中文文案请照此办理。
// 【阅读导引】给父子进程通信定义消息边界。
// 阅读顺序：Socket 传的是连续字节，先读取四字节长度，再等待完整 JSON；Incomplete 表示继续等，Invalid 表示不能继续信任该帧。
// 需要分清：一次 readyRead 不一定是一整条消息，也可能含多条。必须先检查长度再分配/解析，业务 Protobuf 载荷不等于外层 IPC JSON。
// 这是执行实现：按入口调用和信号回调追踪，不要按文件行号把所有函数理解成连续执行。

// 文件职责：编码和解析进程间控制帧、认证帧、心跳帧以及 Latest/Reliable 消息帧。
// 本文件只处理 QByteArray 和协议结构，不创建线程、Socket 或 QWidget；调用方负责决定
// 帧进入哪个队列。解析失败返回明确错误，长度字段和 payload 上限在这里统一校验。
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
/// @brief 把 JSON 对象编码成 4 字节大端长度前缀加紧凑 JSON 的完整帧。
/// @param object 待发送的 IPC 控制消息对象。
/// @return 可直接写入 QLocalSocket 的完整帧字节。
QByteArray encodeFrame(const QJsonObject& object)
{
    // Compact JSON 减少控制帧大小；二进制 payload 由上层决定 inline/shared。
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact); ///< 序列化后的紧凑 JSON 载荷。
    QByteArray frame; ///< 待返回的完整帧，先写长度头再追加 payload。
    frame.reserve(static_cast<int>(sizeof(quint32)) + payload.size());
    QDataStream stream(&frame, QIODevice::WriteOnly); ///< 直接向 frame 写入长度头的序列化流。
    // 固定 Qt 5.15 和大端序，使父子进程的长度编码保持一致。
    stream.setVersion(QDataStream::Qt_5_15);
    stream.setByteOrder(QDataStream::BigEndian);
    stream << static_cast<quint32>(payload.size());
    frame.append(payload);
    return frame;
}

/// @brief 从累计接收缓冲区中最多取出一帧，成功时消费掉已解析的字节。
/// @param buffer 累计接收缓冲区；解析成功后在原地移除已消费部分。
/// @param object 输出参数，解析成功时写入取出的 JSON 对象。
/// @param maxFrameBytes payload 长度上限，超过该值即视为不可信帧。
/// @param errorMessage 可选输出参数，失败时写入中文失败原因。
/// @return 帧状态：Incomplete 表示继续等待，Ready 表示取出成功，Invalid 表示必须断开连接。
FrameResult takeFrame(QByteArray* buffer,
                      QJsonObject* object,
                      int maxFrameBytes,
                      QString* errorMessage)
{
    // 指针和上限是调用契约，任何一个无效都不能继续解析。
    if (buffer == nullptr || object == nullptr || maxFrameBytes <= 0) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral(u"IPC 帧参数无效");
        return FrameResult::Invalid;
    }
    if (buffer->size() < static_cast<int>(sizeof(quint32)))
        // 连 4 字节长度头都不完整，保留原缓冲区等待下一次 readyRead。
        return FrameResult::Incomplete;

    QDataStream stream(*buffer); ///< 只读读取长度头的流，直接作用于调用方的缓冲区。
    stream.setVersion(QDataStream::Qt_5_15);
    stream.setByteOrder(QDataStream::BigEndian);
    quint32 payloadSize = 0; ///< 长度头声明的 payload 字节数。
    stream >> payloadSize;
    if (stream.status() != QDataStream::Ok || payloadSize > static_cast<quint32>(maxFrameBytes)) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral(u"IPC 帧大小超过限制");
        return FrameResult::Invalid;
    }

    const int frameSize = static_cast<int>(sizeof(quint32)) + static_cast<int>(payloadSize); ///< 整帧长度，等于长度头加 payload。
    if (buffer->size() < frameSize)
        // 已知长度但 payload 未收全，同样不消费任何字节。
        return FrameResult::Incomplete;

    const QByteArray payload = buffer->mid(static_cast<int>(sizeof(quint32)),
                                           static_cast<int>(payloadSize)); ///< 已收全的 JSON 载荷副本。
    // 只在完整帧到齐后移除字节，后面的粘连帧继续留在 buffer 中。
    buffer->remove(0, frameSize);
    QJsonParseError parseError; ///< 承接 JSON 解析的错误码和出错位置。
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError); ///< 从 payload 解析出的 JSON 文档。
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (errorMessage != nullptr)
            *errorMessage = QStringLiteral(u"IPC 帧 JSON 无效");
        return FrameResult::Invalid;
    }
    *object = document.object();
    return FrameResult::Ready;
}
}
}
