#pragma once

#include <QtTest>

class BaselineTest : public QObject
{
    Q_OBJECT

private slots:
    void qtAndFrameworkVersions();
    void moduleLifecycleDefaults();
    void protobufRoundTrip();
    void configIsReadOnlyAndResolvesPaths();
    void loggerRollsAndCapturesQtMessages();
    void messageBusPoliciesAndOrdering();
    void pluginLoaderAndModuleIntegration();
    void processProtocolFraming();
    void processIpcAndSupervision();
    void processRejectsInvalidToken();
    void processRegistrationTimeout();
    void processHeartbeatTimeout();
    void processDebuggerWaitDefersHeartbeat();
    void singleInstancePerDirectory();
    void layoutPersistenceAndDockingRules();
    void styleSheetReloadAndFailureRecovery();
};
