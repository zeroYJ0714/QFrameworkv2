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
};
}
