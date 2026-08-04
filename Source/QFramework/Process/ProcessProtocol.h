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
QFRAMEWORK_EXPORT QByteArray encodeFrame(const QJsonObject& object);
// 从累计缓冲区最多取出一帧；成功后会移除已消费字节。
QFRAMEWORK_EXPORT FrameResult takeFrame(QByteArray* buffer,
                                        QJsonObject* object,
                                        int maxFrameBytes,
                                        QString* errorMessage = nullptr);
}
}
