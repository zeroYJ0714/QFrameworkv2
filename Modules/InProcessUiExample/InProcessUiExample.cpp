#include "InProcessUiExample.h"

#include <QDateTime>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVariant>
#include <QVBoxLayout>

#include "Generated/log_messages.pb.h"
#include "Generated/status_messages.pb.h"
#include "MessageTopics.h"

InProcessUiExample::InProcessUiExample(QWidget* parent)
    : qframework::InProcessUiModule(parent),
      statusLabel_(new QLabel(QString::fromUtf8(u8"等待模块消息"), this)),
      receivedMessageCount_(0)
{
    setObjectName(QStringLiteral("InProcessUiExample"));
    setProperty("receivedMessageCount", QVariant(receivedMessageCount_));

    QPushButton* modalButton = new QPushButton(QString::fromUtf8(u8"打开模态对话框"), this);
    QPushButton* nonModalButton = new QPushButton(QString::fromUtf8(u8"打开非模态对话框"), this);
    QVBoxLayout* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(QString::fromUtf8(u8"主进程 UI 模块"), this));
    layout->addWidget(statusLabel_);
    layout->addWidget(modalButton);
    layout->addWidget(nonModalButton);
    layout->addStretch();

    connect(this,
            &InProcessUiExample::logDisplayReceived,
            this,
            &InProcessUiExample::updateLogDisplay,
            Qt::QueuedConnection);
    connect(modalButton,
            &QPushButton::clicked,
            this,
            &InProcessUiExample::showModalDialog);
    connect(nonModalButton,
            &QPushButton::clicked,
            this,
            &InProcessUiExample::showNonModalDialog);
}

QStringList InProcessUiExample::publishedTopics() const
{
    return QStringList() << QString::fromLatin1(QFRAMEWORK_STATUS);
}

QStringList InProcessUiExample::subscribedTopics() const
{
    return QStringList() << QString::fromLatin1(QFRAMEWORK_LOG_DISPLAY);
}

bool InProcessUiExample::onStart()
{
    qframework::protocols::ModuleStatus status;
    status.set_module_id("InProcessUiExample");
    status.set_state(qframework::protocols::MODULE_STATE_RUNNING);
    status.set_detail("ready");
    status.set_timestamp_ms(QDateTime::currentMSecsSinceEpoch());

    std::string bytes;
    if (!status.SerializeToString(&bytes)) {
        logError(QString::fromUtf8(u8"序列化状态消息失败"));
        return false;
    }

    logInfo(QString::fromUtf8(u8"主进程 UI 模块已启动"));
    return publish(QString::fromLatin1(QFRAMEWORK_STATUS), QByteArray::fromStdString(bytes));
}

void InProcessUiExample::onStop()
{
    logInfo(QString::fromUtf8(u8"主进程 UI 模块已停止"));
}

void InProcessUiExample::onMessage(const QString& topic,
                                   const QString& senderModuleId,
                                   const QByteArray& messageData)
{
    Q_UNUSED(senderModuleId)
    if (topic != QString::fromLatin1(QFRAMEWORK_LOG_DISPLAY))
        return;

    qframework::protocols::LogDisplayMessage message;
    if (!message.ParseFromArray(messageData.constData(), messageData.size())) {
        logWarning(QString::fromUtf8(u8"解析日志显示消息失败"));
        return;
    }
    emit logDisplayReceived(QString::fromStdString(message.text()));
}

void InProcessUiExample::updateLogDisplay(const QString& text)
{
    ++receivedMessageCount_;
    setProperty("receivedMessageCount", QVariant(receivedMessageCount_));
    statusLabel_->setText(text);
}

void InProcessUiExample::showModalDialog()
{
    QMessageBox::information(
        this,
        QString::fromUtf8(u8"模态对话框"),
        QString::fromUtf8(u8"该对话框仅阻塞主进程 UI 模块所在界面线程。"));
}

void InProcessUiExample::showNonModalDialog()
{
    QMessageBox* dialog = new QMessageBox(
        QMessageBox::Information,
        QString::fromUtf8(u8"非模态对话框"),
        QString::fromUtf8(u8"此对话框不阻塞用户继续操作。"),
        QMessageBox::Ok,
        this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::NonModal);
    dialog->setModal(false);
    dialog->show();
}
