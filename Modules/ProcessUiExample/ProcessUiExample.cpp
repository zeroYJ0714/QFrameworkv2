#include "ProcessUiExample.h"

#include <QDateTime>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVariant>
#include <QVBoxLayout>

#include "Generated/status_messages.pb.h"
#include "Generated/log_messages.pb.h"
#include "MessageTopics.h"

// 子进程 UI 示例的业务代码与主进程 UI 示例保持相同 API；
// 进程边界、共享内存和 ACK 都隐藏在 ProcessRuntime 中。
ProcessUiExample::ProcessUiExample(QWidget* parent)
    : qframework::ProcessUiModule(parent),
      statusLabel_(new QLabel(QString::fromUtf8(u8"等待子进程消息"), this)),
      receivedMessageCount_(0)
{
    // 所有控件以 this 为父对象，窗口关闭时由 Qt 统一释放。
    setObjectName(QStringLiteral("ProcessUiExample"));
    setProperty("receivedMessageCount", QVariant(receivedMessageCount_));

    QPushButton* modalButton = new QPushButton(QString::fromUtf8(u8"打开模态对话框"), this);
    QPushButton* nonModalButton = new QPushButton(QString::fromUtf8(u8"打开非模态对话框"), this);
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QString::fromUtf8(u8"子进程 UI 模块"), this));
    layout->addWidget(statusLabel_);
    layout->addWidget(modalButton);
    layout->addWidget(nonModalButton);
    layout->addStretch();

    connect(this,
            &ProcessUiExample::logDisplayReceived,
            this,
            &ProcessUiExample::updateLogDisplay,
            Qt::QueuedConnection);
    // onMessage 可能运行在消息线程，使用队列信号切回 UI 线程。
    connect(modalButton,
            &QPushButton::clicked,
            this,
            &ProcessUiExample::showModalDialog);
    connect(nonModalButton,
            &QPushButton::clicked,
            this,
            &ProcessUiExample::showNonModalDialog);
}

QStringList ProcessUiExample::publishedTopics() const
{
    // 子进程向父进程发布状态，父进程再由 MessageBus 路由给订阅者。
    return QStringList() << QString::fromLatin1(QFRAMEWORK_STATUS);
}

QStringList ProcessUiExample::subscribedTopics() const
{
    // 只订阅日志显示主题。
    return QStringList() << QString::fromLatin1(QFRAMEWORK_LOG_DISPLAY);
}

bool ProcessUiExample::onStart()
{
    // 只有 onStart 返回 true 后，ProcessRuntime 才发送 started 给监督器。
    qframework::protocols::ModuleStatus status;
    status.set_module_id("ProcessUiExample");
    status.set_state(qframework::protocols::MODULE_STATE_RUNNING);
    status.set_detail("ready");
    status.set_timestamp_ms(QDateTime::currentMSecsSinceEpoch());

    std::string bytes;
    if (!status.SerializeToString(&bytes)) {
        logError(QString::fromUtf8(u8"序列化子进程状态失败"));
        return false;
    }
    return publish(QString::fromLatin1(QFRAMEWORK_STATUS), QByteArray::fromStdString(bytes));
}

void ProcessUiExample::onStop()
{
    // 停止阶段不再创建新窗口或发布重要业务消息。
    logInfo(QString::fromUtf8(u8"子进程 UI 模块已停止"));
}

void ProcessUiExample::onMessage(const QString& topic,
                                 const QString& senderModuleId,
                                 const QByteArray& messageData)
{
    // 先验证主题和 Protobuf，再通过信号更新控件；不直接碰 QWidget。
    Q_UNUSED(senderModuleId)
    if (topic != QString::fromLatin1(QFRAMEWORK_LOG_DISPLAY))
        return;
    qframework::protocols::LogDisplayMessage message;
    if (!message.ParseFromArray(messageData.constData(), messageData.size())) {
        logWarning(QString::fromUtf8(u8"解析子进程日志消息失败"));
        return;
    }
    emit logDisplayReceived(QString::fromStdString(message.text()));
}

void ProcessUiExample::updateLogDisplay(const QString& text)
{
    // UI 线程槽：记录接收数量并刷新标签。
    ++receivedMessageCount_;
    setProperty("receivedMessageCount", QVariant(receivedMessageCount_));
    statusLabel_->setText(text);
}

void ProcessUiExample::showModalDialog()
{
    // 模态对话框只阻塞子进程自己的 UI 线程，不会锁住父进程。
    QMessageBox::information(
        this,
        QString::fromUtf8(u8"模态对话框"),
        QString::fromUtf8(u8"该对话框仅阻塞子进程 UI 线程。"));
}

void ProcessUiExample::showNonModalDialog()
{
    // WA_DeleteOnClose 让 Qt 在用户关闭后释放这个临时对话框。
    QMessageBox* dialog = new QMessageBox(
        QMessageBox::Information,
        QString::fromUtf8(u8"非模态对话框"),
        QString::fromUtf8(u8"子进程可以继续处理消息。"),
        QMessageBox::Ok,
        this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::NonModal);
    dialog->setModal(false);
    dialog->show();
}
