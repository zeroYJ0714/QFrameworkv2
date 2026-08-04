#pragma once

#include "ProcessNonUiModule.h"

class ProcessNonUiExample final : public qframework::ProcessNonUiModule
{
    Q_OBJECT

public:
    explicit ProcessNonUiExample(QObject* parent = nullptr);

    QStringList publishedTopics() const override;
    QStringList subscribedTopics() const override;
    bool onStart() override;
    void onStop() override;
    void onMessage(const QString& topic,
                   const QString& senderModuleId,
                   const QByteArray& data) override;
};
