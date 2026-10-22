#pragma once

// 文件职责：声明父子进程共用的长度前缀 JSON 帧协议。
// 帧格式为 4 字节大端 payload 长度 + 紧凑 JSON 字节。

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include "QFrameworkGlobal.h"

namespace qframework
{
namespace process
{
enum class FrameResult
{
    // 缓冲区尚未收到完整头或 payload，调用方应继续读取 Socket。
    Incomplete,
    // 成功取出一个 JSON 对象，缓冲区中可能仍有下一帧。
    Ready,
    // 参数、长度或 JSON 无效，调用方应终止当前不可信连接。
    Invalid
};

// 把一个 JSON 对象编码成可直接写入 QLocalSocket 的完整帧。
/// @brief 把 JSON 对象编码成 4 字节大端长度前缀加紧凑 JSON 的完整帧。
/// @param object 待发送的 IPC 控制消息对象。
/// @return 可直接写入 QLocalSocket 的完整帧字节。
QFRAMEWORK_EXPORT QByteArray encodeFrame(const QJsonObject& object);
// 从累计缓冲区最多取出一帧；成功后会移除已消费字节。
/// @brief 从累计接收缓冲区中最多取出一帧，成功时消费掉已解析的字节。
/// @param buffer 累计接收缓冲区；解析成功后在原地移除已消费部分。
/// @param object 输出参数，解析成功时写入取出的 JSON 对象。
/// @param maxFrameBytes payload 长度上限，超过该值即视为不可信帧。
/// @param errorMessage 可选输出参数，失败时写入中文失败原因。
/// @return 帧状态：Incomplete 表示继续等待，Ready 表示取出成功，Invalid 表示必须断开连接。
QFRAMEWORK_EXPORT FrameResult takeFrame(QByteArray* buffer,
                                        QJsonObject* object,
                                        int maxFrameBytes,
                                        QString* errorMessage = nullptr);
}
}
