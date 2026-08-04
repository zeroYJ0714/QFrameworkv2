#pragma once

#include <QObject>

#include "ModuleEndpoint.h"

namespace qframework
{
class QFRAMEWORK_EXPORT ProcessNonUiModule : public QObject, public ModuleEndpoint
{
    Q_OBJECT

public:
    explicit ProcessNonUiModule(QObject* parent = nullptr);
    ~ProcessNonUiModule() override;
};
}
