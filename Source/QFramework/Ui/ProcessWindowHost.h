#pragma once

#include <QWidget>

#include "QFrameworkGlobal.h"

class QLabel;
class QStackedLayout;
class QWindow;

namespace qframework
{
class QFRAMEWORK_EXPORT ProcessWindowHost : public QWidget
{
    Q_OBJECT

public:
    explicit ProcessWindowHost(QWidget* parent = nullptr);
    ~ProcessWindowHost() override;

    bool attachWindow(quintptr windowId, QString* errorMessage = nullptr);
    void showPlaceholder(const QString& detail);
    bool hasEmbeddedWindow() const;

private:
    void clearEmbeddedWindow();

    QStackedLayout* stackedLayout_;
    QLabel* placeholderLabel_;
    QWidget* windowContainer_;
    QWindow* foreignWindow_;
};
}
