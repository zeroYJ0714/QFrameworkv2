#pragma once

#include <QProcess>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include "FrameworkConfig.h"
#include "QFrameworkGlobal.h"

namespace qframework
{
class MessageBus;

class QFRAMEWORK_EXPORT ProcessSupervisor : public QObject
{
    Q_OBJECT

public:
    explicit ProcessSupervisor(MessageBus* messageBus,
                               const MessageBusConfig& messageBusConfig,
                               const ProcessConfig& processConfig,
                               QObject* parent = nullptr);
    ~ProcessSupervisor() override;

    bool startAll(const QVector<ModuleConfig>& modules,
                  QStringList* errors = nullptr);
    bool stop(const QString& moduleId, QString* errorMessage = nullptr);
    bool restart(const QString& moduleId, QString* errorMessage = nullptr);
    bool showWindow(const QString& moduleId, QString* errorMessage = nullptr);
    bool showWindow(const QString& moduleId,
                    int width,
                    int height,
                    QString* errorMessage = nullptr);
    // 强制结束用于故障注入和监督器上层的紧急停止；正常关闭应使用 stop/shutdown。
    bool terminate(const QString& moduleId);
    void shutdown();

    QStringList runningModuleIds() const;
    QString state(const QString& moduleId) const;

signals:
    void moduleStateChanged(const QString& moduleId,
                            const QString& state,
                            const QString& detail);
    void moduleFault(const QString& moduleId, const QString& detail);
    void windowHandleReady(const QString& moduleId, quintptr windowId);

public slots:
    void applyStyleSheet(const QString& styleSheet);

private slots:
    void onServerConnection();
    void onSocketReadyRead();
    void onSocketDisconnected();
    void onProcessError(QProcess::ProcessError error);
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onSupervisionTick();
    void sendMessageToChild(const QString& moduleId,
                            const QString& topic,
                            const QString& senderModuleId,
                            const QByteArray& data);

private:
    struct Entry;

    Entry* findEntry(const QString& moduleId) const;
    Entry* findEntryByServer(QObject* object) const;
    Entry* findEntryBySocket(QObject* object) const;
    Entry* findEntryByProcess(QObject* object) const;
    bool startEntry(Entry* entry, QString* errorMessage);
    bool waitForRunning(Entry* entry, int timeoutMs, QString* errorMessage);
    bool sendFrame(Entry* entry, const QJsonObject& frame);
    void handleFrame(Entry* entry, const QJsonObject& frame);
    void handleFault(Entry* entry, const QString& detail);
    void destroyRuntime(Entry* entry);
    void clearOutgoingShared(Entry* entry);
    void emitState(Entry* entry, const QString& state, const QString& detail);

    MessageBus* messageBus_;
    MessageBusConfig messageBusConfig_;
    ProcessConfig processConfig_;
    QVector<Entry*> entries_;
    QTimer* supervisionTimer_;
    QString styleSheet_;
    bool shuttingDown_;
};
}
