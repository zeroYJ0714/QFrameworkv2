#include "ManagedDockWidget.h"

#include <QMainWindow>
#include <QTimer>

namespace qframework
{
ManagedDockWidget::ManagedDockWidget(const QString& title,
                                     QMainWindow* mainWindow)
    : QDockWidget(title, mainWindow),
      mainWindow_(mainWindow),
      lastDockArea_(Qt::LeftDockWidgetArea),
      restoringDock_(false)
{
    setAllowedAreas(Qt::AllDockWidgetAreas);
    setFeatures(QDockWidget::DockWidgetClosable |
                QDockWidget::DockWidgetMovable);
    connect(this,
            &QDockWidget::dockLocationChanged,
            this,
            &ManagedDockWidget::onDockLocationChanged);
    connect(this,
            &QDockWidget::topLevelChanged,
            this,
            &ManagedDockWidget::onTopLevelChanged);
}

void ManagedDockWidget::onDockLocationChanged(Qt::DockWidgetArea area)
{
    if (area != Qt::NoDockWidgetArea)
        lastDockArea_ = area;
}

void ManagedDockWidget::onTopLevelChanged(bool topLevel)
{
    if (!topLevel || restoringDock_ || mainWindow_ == nullptr)
        return;

    restoringDock_ = true;
    // 等 Qt 完成当前拖拽布局事务后再回停，避免重入 QDockAreaLayout。
    QTimer::singleShot(0, this, &ManagedDockWidget::restoreDocking);
}

void ManagedDockWidget::restoreDocking()
{
    if (mainWindow_ == nullptr) {
        restoringDock_ = false;
        return;
    }
    if (isFloating())
        setFloating(false);
    show();
    restoringDock_ = false;
}
}
