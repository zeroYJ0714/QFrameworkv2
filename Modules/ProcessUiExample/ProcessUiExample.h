#pragma once

// 示例职责：展示子进程 UI 模块的消息显示、窗口句柄上报和对话框行为。

#include "ProcessUiModule.h"

class QLabel;

class ProcessUiExample final : public qframework::ProcessUiModule
{
    Q_OBJECT

public:
    // QWidget 由子进程创建，主进程只接收其窗口句柄。
    explicit ProcessUiExample(QWidget* parent = nullptr);

    // 声明向父进程发布状态的主题。
    QStringList publishedTopics() const override;
    // 声明从父进程接收日志显示的主题。
    QStringList subscribedTopics() const override;
    // 注册成功后发布 Running 状态包。
    bool onStart() override;
    // 写停止日志，不直接结束 QApplication。
    void onStop() override;
    // 解码日志并通过队列信号交给 QWidget 线程。
    void onMessage(const QString& topic,
                   const QString& senderModuleId,
                   const QByteArray& data) override;

signals:
    // 将消息线程收到的文字排队转交给 UI 线程。
    void logDisplayReceived(const QString& text);

private slots:
    // UI 槽只在 QWidget 线程执行，避免跨线程访问标签。
    // 在本地 UI 线程更新标签和测试计数。
    void updateLogDisplay(const QString& text);
    // 打开子进程自己的模态演示对话框。
    void showModalDialog();
    // 打开不阻塞消息循环的非模态演示对话框。
    void showNonModalDialog();

private:
    QLabel* statusLabel_;
    int receivedMessageCount_;
};
