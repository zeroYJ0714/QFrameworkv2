#pragma once

#include "ProcessUiModule.h"

class QLabel;

class ProcessUiExample final : public qframework::ProcessUiModule
{
    Q_OBJECT

public:
    explicit ProcessUiExample(QWidget* parent = nullptr);

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
