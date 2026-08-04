#pragma once

// 示例职责：展示子进程非 UI 模块如何通过 ProcessRuntime 与父进程交换 Protobuf。

#include "ProcessNonUiModule.h"

class ProcessNonUiExample final : public qframework::ProcessNonUiModule
{
    Q_OBJECT

public:
    // ProcessRuntime 在子进程入口创建模块对象。
    explicit ProcessNonUiExample(QObject* parent = nullptr);

    // 向父进程声明日志显示发布权限。
    QStringList publishedTopics() const override;
    // 向父进程声明状态消息订阅权限。
    QStringList subscribedTopics() const override;
    // 发送一条足够大的 ready 日志以演示共享内存路径。
    bool onStart() override;
    // 记录停止并等待 ProcessRuntime 发送 stopAck。
    void onStop() override;
    // 解码父进程状态并回传带发送者信息的日志。
    void onMessage(const QString& topic,
                   const QString& senderModuleId,
                   const QByteArray& data) override;
};
