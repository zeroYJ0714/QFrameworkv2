#include "InProcessUiExample.h"

#include <QDateTime>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QVariant>
#include <QVBoxLayout>

#include "Generated/log_messages.pb.h"
#include "Generated/status_messages.pb.h"
#include "MessageTopics.h"

namespace
{
QString boundedStatusPreview(const QString& text)
{
    const int maximumPreviewLength = 160;
    if (text.size() <= maximumPreviewLength)
        return text;
    return text.left(maximumPreviewLength) + QStringLiteral("...");
}
}

// 主进程 UI 示例的数据流：日志主题进入消息回调后发信号，
// QueuedConnection 把文字交给窗口线程；窗口按钮只演示对话框行为。
InProcessUiExample::InProcessUiExample(QWidget* parent)
    : qframework::InProcessUiModule(parent),
      statusLabel_(new QLabel(QString::fromUtf8(u8"等待模块消息"), this)),
      receivedMessageCount_(0)
{
    // 用 QObject 父子关系管理控件，避免手工 delete 每个按钮和布局对象。
    setObjectName(QStringLiteral("InProcessUiExample"));
    setProperty("receivedMessageCount", QVariant(receivedMessageCount_));
    // 业务消息长度不可信，标签不能把任意长文本变成 Dock 的最小宽度。
    statusLabel_->setTextFormat(Qt::PlainText);
    statusLabel_->setWordWrap(true);
    statusLabel_->setMinimumWidth(0);
    statusLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

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
    // 消息回调可能来自非 GUI 线程，明确使用队列连接保护 QWidget。
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
    // UI 模块向其他模块发布自己的运行状态。
    return QStringList() << QString::fromLatin1(QFRAMEWORK_STATUS);
}

QStringList InProcessUiExample::subscribedTopics() const
{
    // UI 模块只显示日志主题，不直接读取 Logger 文件。
    return QStringList() << QString::fromLatin1(QFRAMEWORK_LOG_DISPLAY);
}

bool InProcessUiExample::onStart()
{
    // 启动时构造一个 Protobuf 状态包，让其他模块知道本模块已 ready。
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
    // 框架停止消息投递后调用此函数；这里只记录可观察的生命周期变化。
    logInfo(QString::fromUtf8(u8"主进程 UI 模块已停止"));
}

void InProcessUiExample::onMessage(const QString& topic,
                                   const QString& senderModuleId,
                                   const QByteArray& messageData)
{
    // 该回调只做解码和发信号，不直接 setText，避免跨线程触碰 QWidget。
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
    // 完整消息已经完成解码和计数；界面只显示有界预览，避免超长负载撑坏布局。
    ++receivedMessageCount_;
    setProperty("receivedMessageCount", QVariant(receivedMessageCount_));
    statusLabel_->setText(boundedStatusPreview(text));
}

void InProcessUiExample::showModalDialog()
{
    // information 是同步模态调用，会暂时阻塞当前 UI 线程的继续执行。
    QMessageBox::information(
        this,
        QString::fromUtf8(u8"模态对话框"),
        QString::fromUtf8(u8"该对话框仅阻塞主进程 UI 模块所在界面线程。"));
}

void InProcessUiExample::showNonModalDialog()
{
    // 非模态对话框由父对象托管，并在关闭时自动删除，不阻塞主窗口。
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
