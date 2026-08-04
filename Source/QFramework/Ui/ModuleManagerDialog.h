#pragma once

#include <QDialog>
#include <QHash>
#include <QVector>

#include "FrameworkConfig.h"
#include "QFrameworkGlobal.h"

class QTableWidget;
class QToolButton;

namespace qframework
{
class QFRAMEWORK_EXPORT ModuleManagerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ModuleManagerDialog(const QVector<ModuleConfig>& modules,
                                 QWidget* parent = nullptr);

    void setModuleState(const QString& moduleId,
                        const QString& state,
                        const QString& detail);
    void setUiAvailable(const QString& moduleId, bool available);

signals:
    void showModuleRequested(const QString& moduleId);
    void restartModuleRequested(const QString& moduleId);

private slots:
    void onShowButtonClicked();
    void onRestartButtonClicked();

private:
    QString moduleTypeText(ModuleType type) const;

    QTableWidget* tableWidget_;
    QHash<QString, int> rows_;
    QHash<QString, QToolButton*> showButtons_;
};
}
