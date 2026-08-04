#pragma once

// 示例职责：展示主进程非 UI 插件如何订阅状态并发布日志显示消息。
// 这是教学模块，不负责创建线程、Socket 或配置文件。

#include "InProcessNonUiModule.h"
#include "QFrameworkPlugin.h"

class InProcessNonUiExample final : public qframework::InProcessNonUiModule
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QFRAMEWORK_PLUGIN_IID FILE "InProcessNonUiExample.json")

public:
    // 插件管理器创建对象并负责其生命周期。
    explicit InProcessNonUiExample(QObject* parent = nullptr);

    // 下面两个列表构成注册契约，必须与实际 publish/onMessage 主题一致。
    // 声明唯一发布主题，MessageBus 注册时据此授予发布权限。
    QStringList publishedTopics() const override;
    // 声明唯一订阅主题，收到后由 onMessage 解码状态包。
    QStringList subscribedTopics() const override;
    // 启动时写一条日志并报告成功。
    bool onStart() override;
    // 停止时写生命周期日志，不自行退出进程。
    void onStop() override;
    // 解码状态消息并生成日志显示回复。
    void onMessage(const QString& topic,
                   const QString& senderModuleId,
                   const QByteArray& data) override;
};
