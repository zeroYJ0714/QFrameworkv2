#pragma once

#include "QFrameworkGlobal.h"

class QApplication;

namespace qframework
{
// 每个 GUI 进程安装一次；QApplication 持有管理器，弹窗仍拥有自己的控件。
QFRAMEWORK_EXPORT void installDialogStyle(QApplication* application);
}
