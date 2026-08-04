#pragma once

#include <QObject>
#include <QString>

#include "QFrameworkGlobal.h"

namespace qframework
{
class QFRAMEWORK_EXPORT StyleManager : public QObject
{
    Q_OBJECT

public:
    explicit StyleManager(QObject* parent = nullptr);

    bool loadStyleSheet(const QString& filePath,
                        QString* errorMessage = nullptr);
    bool reloadStyleSheet(QString* errorMessage = nullptr);

    QString currentFilePath() const;
    QString currentStyleSheet() const;

signals:
    void styleSheetChanged(const QString& styleSheet);

private:
    bool readStyleSheet(const QString& filePath,
                        QString* styleSheet,
                        QString* errorMessage) const;
    bool isStructurallyValid(const QString& styleSheet) const;

    QString currentFilePath_;
    QString currentStyleSheet_;
};
}
