#include "ManagedDockWidget.h"

#include <QMainWindow>

namespace qframework
{
ManagedDockWidget::ManagedDockWidget(const QString& title,
                                     QMainWindow* mainWindow)
    : QDockWidget(title, mainWindow)
{
    setAllowedAreas(Qt::AllDockWidgetAreas);
    setFeatures(QDockWidget::DockWidgetClosable |
                QDockWidget::DockWidgetMovable);
}
}
