#pragma once

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

// 兼容旧 publish(const QByteArray&) 的一次性包装入口。
// QByteArray 本身使用隐式共享，因此这里创建独立只读对象但不深拷贝字节缓冲区。
inline MessagePayload makeMessagePayload(const QByteArray& data)
{
    return MessagePayload(new QByteArray(data));
}

// 跨进程接收端已经拥有独立 QByteArray 时转移其缓冲区，避免再增加一次临时副本。
inline MessagePayload makeMessagePayload(QByteArray&& data)
{
    return MessagePayload(new QByteArray(qMove(data)));
}
}
