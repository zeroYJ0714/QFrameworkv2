#pragma once

#include <QWidget>

#include "ModuleEndpoint.h"

namespace qframework
{
class QFRAMEWORK_EXPORT InProcessUiModule : public QWidget, public ModuleEndpoint
{
    Q_OBJECT

public:
    explicit InProcessUiModule(QWidget* parent = nullptr);
    ~InProcessUiModule() override;
};
}
