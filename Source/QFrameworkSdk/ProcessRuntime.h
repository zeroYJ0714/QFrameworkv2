#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QLocalSocket>

#include "ModuleEndpoint.h"
#include "QFrameworkGlobal.h"

class QCoreApplication;
class QLocalSocket;
class QSharedMemory;

namespace qframework
{
class QFRAMEWORK_EXPORT ProcessRuntime : public QObject
{
    Q_OBJECT

public:
    // 子进程 main.cpp 只负责创建 QApplication/QCoreApplication 和模块实例。
    static int run(QCoreApplication* application, ModuleEndpoint* module);

private slots:
    void onSocketConnected();
    void onSocketReadyRead();
    void onSocketDisconnected();
    void onSocketError(QLocalSocket::LocalSocketError error);
    void onSendPublish(const QString& topic, const QByteArray& data);
    void onSendLog(int level, const QString& text);

private:
    class RuntimeHost;
    class MessageQueue;

    ProcessRuntime(QCoreApplication* application, ModuleEndpoint* module);
    ~ProcessRuntime() override;

    int execute();
    bool parseArguments();
    bool hasArgument(const QString& name) const;
    QString argumentValue(const QString& name) const;
    void handleFrame(const QJsonObject& frame);
    void sendFrame(const QJsonObject& frame);
    void sendSharedAck(const QString& messageId);
    void finish(int exitCode);
    void clearSharedSegments();
    bool queuePublish(const QString& topic, const QByteArray& data);
    void queueLog(LogLevel level, const QString& text);

    QCoreApplication* application_;
    ModuleEndpoint* module_;
    RuntimeHost* host_;
    MessageQueue* messageQueue_;
    QLocalSocket* socket_;
    QByteArray inputBuffer_;
    QStringList arguments_;
    QString moduleId_;
    QString moduleType_;
    QString serverName_;
    QString token_;
    int sharedMemoryThresholdBytes_;
    int maxMessageBytes_;
    int shutdownDrainTimeoutMs_;
    bool waitForDebugger_;
    int debuggerWaitTimeoutMs_;
    bool registrationAcknowledged_;
    bool running_;
    bool stopping_;
    int exitCode_;
    QHash<QString, QSharedMemory*> outgoingSharedSegments_;
};
}
