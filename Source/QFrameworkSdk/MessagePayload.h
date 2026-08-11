#pragma once

// 文件职责：定义进程内 MessageBus 使用的只读共享载荷类型。
// 发布者创建一次 QByteArray，队列和订阅回调只复制 QSharedPointer；最后一个引用释放后才销毁。
// 它不跨进程、不改变公开 QByteArray 回调签名，也不允许订阅者通过 const 指针修改内容。

// 文件职责：定义框架内部共享的不可变消息载荷。
// MessagePayload 只共享 QByteArray 对象的只读所有权，不改变模块公开的
// onMessage(topic, senderModuleId, const QByteArray&) 回调签名。
#include <QByteArray>
#include <QSharedPointer>
#include <QtGlobal>

namespace qframework
{
// 同进程发布链中的队列只复制这个智能指针；最后一个队列或回调释放引用后，
// QByteArray 自动销毁。const 限定禁止订阅者通过共享指针修改公共载荷。
using MessagePayload = QSharedPointer<const QByteArray>;

/// @brief 把 const QByteArray 包装成不可变共享消息载荷。
/// @param data 输入字节。
/// @return 非空 MessagePayload；其最后一个引用释放时自动销毁 QByteArray。
inline MessagePayload makeMessagePayload(const QByteArray& data)
{
    /// @brief 执行 `MessagePayload` 所定义的类职责。
    /// @return 返回声明类型的结果值。
    return MessagePayload(new QByteArray(data));
}

/// @brief 移动已有 QByteArray 缓冲区生成不可变共享载荷。
/// @param data 右值，调用后其内容进入未指定但有效状态。
/// @return 拥有原缓冲区的非空 MessagePayload。
inline MessagePayload makeMessagePayload(QByteArray&& data)
{
    /// @brief 执行 `MessagePayload` 所定义的类职责。
    /// @return 返回声明类型的结果值。
    return MessagePayload(new QByteArray(qMove(data)));
}
}
