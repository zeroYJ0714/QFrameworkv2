#include "InProcessNonUiModule.h"
#include "InProcessUiModule.h"
#include "ProcessNonUiModule.h"
#include "ProcessUiModule.h"

namespace qframework
{
InProcessNonUiModule::InProcessNonUiModule(QObject* parent)
    : QObject(parent)
{
}

InProcessNonUiModule::~InProcessNonUiModule() = default;

InProcessUiModule::InProcessUiModule(QWidget* parent)
    : QWidget(parent)
{
}

InProcessUiModule::~InProcessUiModule() = default;

ProcessNonUiModule::ProcessNonUiModule(QObject* parent)
    : QObject(parent)
{
}

ProcessNonUiModule::~ProcessNonUiModule() = default;

ProcessUiModule::ProcessUiModule(QWidget* parent)
    : QWidget(parent)
{
}

ProcessUiModule::~ProcessUiModule() = default;
}
