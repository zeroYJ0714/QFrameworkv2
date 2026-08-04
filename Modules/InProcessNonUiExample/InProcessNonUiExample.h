#pragma once

#include "InProcessNonUiModule.h"
#include "QFrameworkPlugin.h"

class InProcessNonUiExample final : public qframework::InProcessNonUiModule
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID QFRAMEWORK_PLUGIN_IID FILE "InProcessNonUiExample.json")

public:
    explicit InProcessNonUiExample(QObject* parent = nullptr);

    QStringList publishedTopics() const override;
    QStringList subscribedTopics() const override;
    bool onStart() override;
    void onStop() override;
    void onMessage(const QString& topic,
                   const QString& senderModuleId,
                   const QByteArray& data) override;
};
