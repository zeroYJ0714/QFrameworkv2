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

ProcessUiExample::ProcessUiExample(QWidget* parent)
    : qframework::ProcessUiModule(parent),
      statusLabel_(new QLabel(QString::fromUtf8(u8"等待子进程消息"), this)),
      receivedMessageCount_(0)
{
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
    return QStringList() << QString::fromLatin1(QFRAMEWORK_STATUS);
}

QStringList ProcessUiExample::subscribedTopics() const
{
    return QStringList() << QString::fromLatin1(QFRAMEWORK_LOG_DISPLAY);
}

bool ProcessUiExample::onStart()
{
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
    logInfo(QString::fromUtf8(u8"子进程 UI 模块已停止"));
}

void ProcessUiExample::onMessage(const QString& topic,
                                 const QString& senderModuleId,
                                 const QByteArray& messageData)
{
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
    ++receivedMessageCount_;
    setProperty("receivedMessageCount", QVariant(receivedMessageCount_));
    statusLabel_->setText(text);
}

void ProcessUiExample::showModalDialog()
{
    QMessageBox::information(
        this,
        QString::fromUtf8(u8"模态对话框"),
        QString::fromUtf8(u8"该对话框仅阻塞子进程 UI 线程。"));
}

void ProcessUiExample::showNonModalDialog()
{
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
