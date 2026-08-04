#pragma once

#include <QWidget>

#include "ModuleEndpoint.h"

namespace qframework
{
class QFRAMEWORK_EXPORT ProcessUiModule : public QWidget, public ModuleEndpoint
{
    Q_OBJECT

public:
    explicit ProcessUiModule(QWidget* parent = nullptr);
    ~ProcessUiModule() override;
};
}
