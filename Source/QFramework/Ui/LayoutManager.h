#pragma once

#include <QHash>
#include <QString>
#include <QStringList>

#include "QFrameworkGlobal.h"

class QDockWidget;
class QMainWindow;

namespace qframework
{
class QFRAMEWORK_EXPORT LayoutManager
{
public:
    explicit LayoutManager(QMainWindow* mainWindow);

    void registerModuleDock(const QString& moduleId, QDockWidget* dockWidget);
    void unregisterModuleDock(const QString& moduleId);

    bool saveLayout(const QString& filePath,
                    QString* errorMessage = nullptr);
    bool loadLayout(const QString& filePath,
                    QString* errorMessage = nullptr,
                    QStringList* unavailableModuleIds = nullptr);

    QString activeFilePath() const;

private:
    bool validateFilePath(const QString& filePath,
                          QString* errorMessage) const;

    QMainWindow* mainWindow_;
    QHash<QString, QDockWidget*> moduleDocks_;
    QString activeFilePath_;
};
}
