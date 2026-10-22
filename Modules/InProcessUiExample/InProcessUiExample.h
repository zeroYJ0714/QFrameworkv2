#pragma once

// 示例职责：展示主进程 UI 插件的 Dock、消息转 UI 信号以及模态/非模态对话框。

#include "InProcessUiModule.h"
#include "QFrameworkPlugin.h"

class QLabel;

class InProcessUiExample final : public qframework::InProcessUiModule
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QFRAMEWORK_PLUGIN_IID FILE "InProcessUiExample.json")

public:
    // statusLabel_ 和按钮由构造函数创建，QObject 父子关系负责回收。
    explicit InProcessUiExample(QWidget* parent = nullptr);

    // 发布模块运行状态，供非 UI 示例订阅。
    QStringList publishedTopics() const override;
    // 订阅日志显示消息，随后通过信号更新标签。
    QStringList subscribedTopics() const override;
    // 序列化并发布 Running 状态包。
    bool onStart() override;
    // 记录停止，不在 UI 回调中阻塞等待。
    void onStop() override;
    // 只解码消息并发信号，避免跨线程直接访问 QWidget。
    void onMessage(const QString& topic,
                   const QString& senderModuleId,
                   const QByteArray& data) override;

signals:
    // onMessage 不直接操作控件，而是发信号回到 UI 线程更新显示。
    void logDisplayReceived(const QString& text);

private slots:
    // 这些槽都运行在 QWidget 所属线程，适合安全访问控件。
    // 在 UI 线程更新计数属性和标签。
    void updateLogDisplay(const QString& text);
    // 打开同步模态演示对话框。
    void showModalDialog();
    // 打开由 Qt 父对象托管的非模态演示对话框。
    void showNonModalDialog();

private:
    // 标签显示最近一条日志，计数属性供自动化测试观察。
    QLabel* statusLabel_;
    int receivedMessageCount_;
};
