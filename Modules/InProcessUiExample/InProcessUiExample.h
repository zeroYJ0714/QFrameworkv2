#pragma once

#include "InProcessUiModule.h"
#include "QFrameworkPlugin.h"

class QLabel;

class InProcessUiExample final : public qframework::InProcessUiModule
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QFRAMEWORK_PLUGIN_IID FILE "InProcessUiExample.json")

public:
    explicit InProcessUiExample(QWidget* parent = nullptr);

    QStringList publishedTopics() const override;
    QStringList subscribedTopics() const override;
    bool onStart() override;
    void onStop() override;
    void onMessage(const QString& topic,
                   const QString& senderModuleId,
                   const QByteArray& data) override;

signals:
    void logDisplayReceived(const QString& text);

private slots:
    void updateLogDisplay(const QString& text);
    void showModalDialog();
    void showNonModalDialog();

private:
    QLabel* statusLabel_;
    int receivedMessageCount_;
};
