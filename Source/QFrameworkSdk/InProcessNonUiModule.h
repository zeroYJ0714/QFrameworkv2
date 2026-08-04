#pragma once

#include <QObject>

#include "ModuleEndpoint.h"

namespace qframework
{
class QFRAMEWORK_EXPORT InProcessNonUiModule : public QObject, public ModuleEndpoint
{
    Q_OBJECT

public:
    explicit InProcessNonUiModule(QObject* parent = nullptr);
    ~InProcessNonUiModule() override;
};
}
