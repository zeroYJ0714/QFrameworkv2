#include "ProcessRuntime.h"

#include <QCoreApplication>
#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QMutexLocker>
#include <QQueue>
#include <QSharedMemory>
#include <QThread>
#include <QUuid>
#include <QWaitCondition>
#include <QWidget>

#include <cstring>
#include <exception>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "ProcessProtocol.h"

namespace qframework
{
namespace
{
const QString kRegister = QStringLiteral("register");
const QString kRegisterAck = QStringLiteral("registerAck");
const QString kStarted = QStringLiteral("started");
const QString kStartFailed = QStringLiteral("startFailed");
const QString kMessage = QStringLiteral("message");
const QString kLog = QStringLiteral("log");
const QString kPing = QStringLiteral("ping");
const QString kPong = QStringLiteral("pong");
const QString kStop = QStringLiteral("stop");
const QString kStopAck = QStringLiteral("stopAck");
const QString kWindowReady = QStringLiteral("windowReady");
const QString kShowWindow = QStringLiteral("showWindow");
const QString kWindowWidth = QStringLiteral("windowWidth");
const QString kWindowHeight = QStringLiteral("windowHeight");
const QString kSharedAck = QStringLiteral("sharedAck");
const QString kDebugWaitTimeout = QStringLiteral("debugWaitTimeout");
const QString kStyleSheet = QStringLiteral("styleSheet");

bool parsePositiveInt(const QString& value, int defaultValue, int* result)
{
    bool ok = false;
    const int parsed = value.toInt(&ok);
    if (!ok || parsed <= 0)
        *result = defaultValue;
    else
        *result = parsed;
    return ok && parsed > 0;
}

QString sharedKey(const QString& prefix)
{
    return QStringLiteral("QFramework_%1_%2")
        .arg(prefix, QUuid::createUuid().toString(QUuid::Id128));
}

void showEmbeddedWidget(QWidget* widget, int width, int height)
{
    if (widget == nullptr)
        return;

    widget->show();
    if (width > 0 && height > 0)
        widget->resize(width, height);
}
}

class ProcessRuntime::RuntimeHost final : public ModuleHost
{
public:
    explicit RuntimeHost(ProcessRuntime* runtime)
        : runtime_(runtime)
    {
    }

    bool publishFromModule(const QString& moduleId,
                           const QString& topic,
                           const QByteArray& data) override
    {
        Q_UNUSED(moduleId)
        return runtime_->queuePublish(topic, data);
    }

    void logFromModule(LogLevel level,
                       const QString& moduleId,
                       const QString& text) override
    {
        Q_UNUSED(moduleId)
        runtime_->queueLog(level, text);
    }

private:
    ProcessRuntime* runtime_;
};

class ProcessRuntime::MessageQueue final : public QThread
{
public:
    explicit MessageQueue(ModuleEndpoint* module)
        : module_(module),
          accepting_(true),
          stopping_(false)
    {
    }

    bool enqueue(const QString& topic,
                 const QString& senderModuleId,
                 const QByteArray& data)
    {
        QMutexLocker locker(&mutex_);
        if (!accepting_)
            return false;
        Message message;
        message.topic = topic;
        message.senderModuleId = senderModuleId;
        message.data = data;
        messages_.enqueue(message);
        available_.wakeOne();
        return true;
    }

    bool stopAndDrain(int timeoutMs)
    {
        {
            QMutexLocker locker(&mutex_);
            accepting_ = false;
            stopping_ = true;
            available_.wakeOne();
        }
        return wait(static_cast<unsigned long>(qMax(1, timeoutMs)));
    }

protected:
    void run() override
    {
        for (;;) {
            Message message;
            {
                QMutexLocker locker(&mutex_);
                while (messages_.isEmpty() && !stopping_)
                    available_.wait(&mutex_);
                if (messages_.isEmpty() && stopping_)
                    return;
                message = messages_.dequeue();
            }
            try {
                module_->onMessage(message.topic,
                                   message.senderModuleId,
                                   message.data);
            } catch (...) {
                module_->logError(QString::fromUtf8(u8"onMessage 未知异常"));
            }
        }
    }

private:
    struct Message
    {
        QString topic;
        QString senderModuleId;
        QByteArray data;
    };

    ModuleEndpoint* module_;
    QMutex mutex_;
    QWaitCondition available_;
    QQueue<Message> messages_;
    bool accepting_;
    bool stopping_;
};

ProcessRuntime::ProcessRuntime(QCoreApplication* application, ModuleEndpoint* module)
    : QObject(application),
      application_(application),
      module_(module),
      host_(new RuntimeHost(this)),
      messageQueue_(nullptr),
      socket_(new QLocalSocket(this)),
      sharedMemoryThresholdBytes_(1024 * 1024),
      maxMessageBytes_(16 * 1024 * 1024),
      shutdownDrainTimeoutMs_(3000),
      waitForDebugger_(false),
      debuggerWaitTimeoutMs_(30000),
      registrationAcknowledged_(false),
      running_(false),
      stopping_(false),
      exitCode_(0)
{
    connect(socket_, &QLocalSocket::connected,
            this, &ProcessRuntime::onSocketConnected);
    connect(socket_, &QLocalSocket::readyRead,
            this, &ProcessRuntime::onSocketReadyRead);
    connect(socket_, &QLocalSocket::disconnected,
            this, &ProcessRuntime::onSocketDisconnected);
    connect(socket_,
            QOverload<QLocalSocket::LocalSocketError>::of(&QLocalSocket::errorOccurred),
            this,
            &ProcessRuntime::onSocketError);
}

ProcessRuntime::~ProcessRuntime()
{
    if (messageQueue_ != nullptr) {
        if (messageQueue_->isRunning())
            messageQueue_->stopAndDrain(shutdownDrainTimeoutMs_);
        delete messageQueue_;
        messageQueue_ = nullptr;
    }
    if (module_ != nullptr) {
        module_->setRunning(false);
        module_->bindHost(QString(), nullptr);
        delete module_;
        module_ = nullptr;
    }
    clearSharedSegments();
}

int ProcessRuntime::run(QCoreApplication* application, ModuleEndpoint* module)
{
    if (application == nullptr || module == nullptr)
        return 2;
    ProcessRuntime runtime(application, module);
    return runtime.execute();
}

int ProcessRuntime::execute()
{
    if (!parseArguments())
        return 2;
    socket_->connectToServer(serverName_);
    application_->exec();
    return exitCode_;
}

bool ProcessRuntime::parseArguments()
{
    arguments_ = application_->arguments();
    moduleId_ = argumentValue(QStringLiteral("--qframework-module-id"));
    moduleType_ = argumentValue(QStringLiteral("--qframework-module-type"));
    serverName_ = argumentValue(QStringLiteral("--qframework-server"));
    token_ = argumentValue(QStringLiteral("--qframework-token"));
    if (moduleId_.isEmpty() || moduleType_.isEmpty() || serverName_.isEmpty() || token_.isEmpty())
        return false;

    parsePositiveInt(argumentValue(QStringLiteral("--qframework-shared-memory-threshold")),
                     sharedMemoryThresholdBytes_,
                     &sharedMemoryThresholdBytes_);
    parsePositiveInt(argumentValue(QStringLiteral("--qframework-max-message-bytes")),
                     maxMessageBytes_,
                     &maxMessageBytes_);
    parsePositiveInt(argumentValue(QStringLiteral("--qframework-shutdown-drain-timeout")),
                     shutdownDrainTimeoutMs_,
                     &shutdownDrainTimeoutMs_);
    waitForDebugger_ = hasArgument(QStringLiteral("--qframework-wait-for-debugger"));
    parsePositiveInt(argumentValue(QStringLiteral("--qframework-debugger-timeout-ms")),
                     debuggerWaitTimeoutMs_,
                     &debuggerWaitTimeoutMs_);

    if (moduleType_ == QStringLiteral("ProcessUi") && dynamic_cast<QWidget*>(module_) == nullptr)
        return false;
    if (moduleType_ == QStringLiteral("ProcessNonUi") && dynamic_cast<QWidget*>(module_) != nullptr)
        return false;
    module_->bindHost(moduleId_, host_);
    return true;
}

bool ProcessRuntime::hasArgument(const QString& name) const
{
    return arguments_.contains(name);
}

QString ProcessRuntime::argumentValue(const QString& name) const
{
    const int index = arguments_.indexOf(name);
    if (index < 0 || index + 1 >= arguments_.size())
        return QString();
    return arguments_.at(index + 1);
}

void ProcessRuntime::onSocketConnected()
{
    QJsonObject frame;
    frame.insert(QStringLiteral("type"), kRegister);
    frame.insert(QStringLiteral("moduleId"), moduleId_);
    frame.insert(QStringLiteral("moduleType"), moduleType_);
    frame.insert(QStringLiteral("token"), token_);
    frame.insert(QStringLiteral("pid"), static_cast<qint64>(QCoreApplication::applicationPid()));
    QJsonArray published;
    for (const QString& topic : module_->publishedTopics())
        published.append(topic);
    QJsonArray subscribed;
    for (const QString& topic : module_->subscribedTopics())
        subscribed.append(topic);
    frame.insert(QStringLiteral("publishedTopics"), published);
    frame.insert(QStringLiteral("subscribedTopics"), subscribed);
    sendFrame(frame);
}

void ProcessRuntime::onSocketReadyRead()
{
    inputBuffer_.append(socket_->readAll());
    for (;;) {
        QJsonObject frame;
        QString error;
        const process::FrameResult result = process::takeFrame(
            &inputBuffer_,
            &frame,
            qMax(64 * 1024, maxMessageBytes_ * 2),
            &error);
        if (result == process::FrameResult::Incomplete)
            return;
        if (result == process::FrameResult::Invalid) {
            Q_UNUSED(error)
            finish(5);
            return;
        }
        handleFrame(frame);
        if (stopping_)
            return;
    }
}

void ProcessRuntime::onSocketDisconnected()
{
    if (!stopping_)
        finish(4);
}

void ProcessRuntime::onSocketError(QLocalSocket::LocalSocketError error)
{
    Q_UNUSED(error)
    if (!registrationAcknowledged_)
        finish(3);
}

void ProcessRuntime::handleFrame(const QJsonObject& frame)
{
    const QString type = frame.value(QStringLiteral("type")).toString();
    if (type == kRegisterAck) {
        if (!frame.value(QStringLiteral("accepted")).toBool()) {
            finish(3);
            return;
        }
        registrationAcknowledged_ = true;
        QApplication* widgetApplication = qobject_cast<QApplication*>(application_);
        if (widgetApplication != nullptr &&
            frame.value(QStringLiteral("styleSheet")).isString()) {
            widgetApplication->setStyleSheet(
                frame.value(QStringLiteral("styleSheet")).toString());
        }
#ifdef Q_OS_WIN
#ifdef _DEBUG
        if (waitForDebugger_) {
            QElapsedTimer timer;
            timer.start();
            while (!IsDebuggerPresent() && timer.elapsed() < debuggerWaitTimeoutMs_)
                QThread::msleep(50);
            if (IsDebuggerPresent()) {
                // 附加后先停在同步断点，给 Visual Studio 时间绑定模块符号和初始化断点。
                DebugBreak();
            } else {
                QJsonObject timeoutFrame;
                timeoutFrame.insert(QStringLiteral("type"), kDebugWaitTimeout);
                sendFrame(timeoutFrame);
            }
        }
#endif
#endif
        module_->setRunning(true);
        running_ = true;
        bool started = false;
        try {
            started = module_->onStart();
        } catch (const std::exception& exception) {
            QJsonObject failure;
            failure.insert(QStringLiteral("type"), kStartFailed);
            failure.insert(QStringLiteral("detail"), QString::fromUtf8(exception.what()));
            sendFrame(failure);
            module_->setRunning(false);
            running_ = false;
            finish(6);
            return;
        } catch (...) {
            QJsonObject failure;
            failure.insert(QStringLiteral("type"), kStartFailed);
            failure.insert(QStringLiteral("detail"), QString::fromUtf8(u8"未知启动异常"));
            sendFrame(failure);
            module_->setRunning(false);
            running_ = false;
            finish(6);
            return;
        }
        if (!started) {
            QJsonObject failure;
            failure.insert(QStringLiteral("type"), kStartFailed);
            failure.insert(QStringLiteral("detail"), QString::fromUtf8(u8"onStart 返回 false"));
            sendFrame(failure);
            module_->setRunning(false);
            running_ = false;
            finish(6);
            return;
        }
        messageQueue_ = new MessageQueue(module_);
        messageQueue_->start();
        QJsonObject startedFrame;
        startedFrame.insert(QStringLiteral("type"), kStarted);
        sendFrame(startedFrame);
        QWidget* widget = dynamic_cast<QWidget*>(module_);
        if (widget != nullptr) {
            QJsonObject windowFrame;
            windowFrame.insert(QStringLiteral("type"), kWindowReady);
            windowFrame.insert(QStringLiteral("windowId"),
                               QString::number(static_cast<qulonglong>(widget->winId())));
            sendFrame(windowFrame);
        }
        return;
    }
    if (type == kPing) {
        QJsonObject pong;
        pong.insert(QStringLiteral("type"), kPong);
        sendFrame(pong);
        return;
    }
    if (type == kStyleSheet) {
        QApplication* widgetApplication = qobject_cast<QApplication*>(application_);
        if (widgetApplication != nullptr)
            widgetApplication->setStyleSheet(
                frame.value(QStringLiteral("styleSheet")).toString());
        return;
    }
    if (type == kShowWindow && running_) {
        QWidget* widget = dynamic_cast<QWidget*>(module_);
        showEmbeddedWidget(widget,
                           frame.value(kWindowWidth).toInt(),
                           frame.value(kWindowHeight).toInt());
        return;
    }
    if (type == kStop) {
        stopping_ = true;
        module_->setRunning(false);
        running_ = false;
        if (messageQueue_ != nullptr &&
            !messageQueue_->stopAndDrain(shutdownDrainTimeoutMs_)) {
            return;
        }
        try {
            module_->onStop();
        } catch (...) {
            // 停止边界必须隔离模块异常，随后仍发送确认。
        }
        QJsonObject ack;
        ack.insert(QStringLiteral("type"), kStopAck);
        sendFrame(ack);
        socket_->flush();
        socket_->waitForBytesWritten(500);
        finish(0);
        return;
    }
    if (type == kSharedAck) {
        const QString messageId = frame.value(QStringLiteral("messageId")).toString();
        QSharedMemory* shared = outgoingSharedSegments_.take(messageId);
        if (shared != nullptr) {
            shared->detach();
            delete shared;
        }
        return;
    }
    if (type != kMessage || !running_)
        return;

    const QString topic = frame.value(QStringLiteral("topic")).toString();
    const QString sender = frame.value(QStringLiteral("senderModuleId")).toString();
    const QString transport = frame.value(QStringLiteral("transport")).toString();
    QByteArray data;
    if (transport == QStringLiteral("shared")) {
        const QString messageId = frame.value(QStringLiteral("messageId")).toString();
        const QString key = frame.value(QStringLiteral("sharedKey")).toString();
        QSharedMemory shared(key);
        if (shared.attach(QSharedMemory::ReadOnly)) {
            if (shared.lock()) {
                data = QByteArray(static_cast<const char*>(shared.constData()), shared.size());
                shared.unlock();
            }
            shared.detach();
        }
        sendSharedAck(messageId);
    } else {
        data = QByteArray::fromBase64(
            frame.value(QStringLiteral("data")).toString().toLatin1());
    }
    if (data.size() > maxMessageBytes_)
        return;
    if (messageQueue_ != nullptr)
        messageQueue_->enqueue(topic, sender, data);
}

void ProcessRuntime::onSendPublish(const QString& topic, const QByteArray& data)
{
    if (!running_ || socket_->state() != QLocalSocket::ConnectedState ||
        data.size() > maxMessageBytes_)
        return;

    const QString messageId = QUuid::createUuid().toString(QUuid::Id128);
    QJsonObject frame;
    frame.insert(QStringLiteral("type"), kMessage);
    frame.insert(QStringLiteral("topic"), topic);
    frame.insert(QStringLiteral("senderModuleId"), moduleId_);
    frame.insert(QStringLiteral("messageId"), messageId);
    if (data.size() >= sharedMemoryThresholdBytes_) {
        const QString key = sharedKey(moduleId_);
        QSharedMemory* shared = new QSharedMemory(key, this);
        if (!shared->create(data.size())) {
            delete shared;
            return;
        }
        if (!shared->lock()) {
            shared->detach();
            delete shared;
            return;
        }
        std::memcpy(shared->data(), data.constData(), static_cast<size_t>(data.size()));
        shared->unlock();
        outgoingSharedSegments_.insert(messageId, shared);
        frame.insert(QStringLiteral("transport"), QStringLiteral("shared"));
        frame.insert(QStringLiteral("sharedKey"), key);
        frame.insert(QStringLiteral("size"), data.size());
    } else {
        frame.insert(QStringLiteral("transport"), QStringLiteral("inline"));
        frame.insert(QStringLiteral("data"), QString::fromLatin1(data.toBase64()));
    }
    sendFrame(frame);
}

void ProcessRuntime::onSendLog(int level, const QString& text)
{
    if (socket_->state() != QLocalSocket::ConnectedState)
        return;
    QJsonObject frame;
    frame.insert(QStringLiteral("type"), kLog);
    frame.insert(QStringLiteral("level"), level);
    frame.insert(QStringLiteral("text"), text);
    sendFrame(frame);
}

bool ProcessRuntime::queuePublish(const QString& topic, const QByteArray& data)
{
    if (!running_ || socket_->state() != QLocalSocket::ConnectedState)
        return false;
    return QMetaObject::invokeMethod(
        this,
        "onSendPublish",
        Qt::QueuedConnection,
        Q_ARG(QString, topic),
        Q_ARG(QByteArray, data));
}

void ProcessRuntime::queueLog(LogLevel level, const QString& text)
{
    QMetaObject::invokeMethod(
        this,
        "onSendLog",
        Qt::QueuedConnection,
        Q_ARG(int, static_cast<int>(level)),
        Q_ARG(QString, text));
}

void ProcessRuntime::sendFrame(const QJsonObject& frame)
{
    if (socket_->state() != QLocalSocket::ConnectedState)
        return;
    socket_->write(process::encodeFrame(frame));
}

void ProcessRuntime::sendSharedAck(const QString& messageId)
{
    QJsonObject ack;
    ack.insert(QStringLiteral("type"), kSharedAck);
    ack.insert(QStringLiteral("messageId"), messageId);
    sendFrame(ack);
}

void ProcessRuntime::finish(int exitCode)
{
    exitCode_ = exitCode;
    stopping_ = true;
    if (application_ != nullptr)
        application_->quit();
}

void ProcessRuntime::clearSharedSegments()
{
    const QList<QSharedMemory*> segments = outgoingSharedSegments_.values();
    outgoingSharedSegments_.clear();
    for (QSharedMemory* shared : segments) {
        shared->detach();
        delete shared;
    }
}
}
