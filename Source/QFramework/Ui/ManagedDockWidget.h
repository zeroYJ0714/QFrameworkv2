#pragma once

#include <QDockWidget>

#include "QFrameworkGlobal.h"

class QMainWindow;

namespace qframework
{
class QFRAMEWORK_EXPORT ManagedDockWidget : public QDockWidget
{
    Q_OBJECT

public:
    explicit ManagedDockWidget(const QString& title,
                               QMainWindow* mainWindow);

private slots:
    void onDockLocationChanged(Qt::DockWidgetArea area);
    void onTopLevelChanged(bool topLevel);
    void restoreDocking();

private:
    QMainWindow* mainWindow_;
    Qt::DockWidgetArea lastDockArea_;
    bool restoringDock_;
};
}
