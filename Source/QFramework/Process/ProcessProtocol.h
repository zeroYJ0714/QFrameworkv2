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
/// @brief 执行 `encodeFrame` 所定义的类职责。
/// @param object 用于反查所属记录的 QObject 指针。
/// @return 返回对应 Qt 值或容器的安全副本。
QFRAMEWORK_EXPORT QByteArray encodeFrame(const QJsonObject& object);
// 从累计缓冲区最多取出一帧；成功后会移除已消费字节。
/// @brief 执行 `takeFrame` 所定义的类职责。
/// @param buffer 传给该操作的 `buffer` 参数；取值应符合声明类型和函数用途。
/// @param object 用于反查所属记录的 QObject 指针。
/// @param maxFrameBytes 传给该操作的 `maxFrameBytes` 参数；取值应符合声明类型和函数用途。
/// @param errorMessage 可选错误说明输出参数。
/// @return 返回声明类型的结果值。
QFRAMEWORK_EXPORT FrameResult takeFrame(QByteArray* buffer,
                                        QJsonObject* object,
                                        int maxFrameBytes,
                                        QString* errorMessage = nullptr); ///< `errorMessage` 对应的对象指针；所有权和线程归属见本类文件级说明。
}
}
