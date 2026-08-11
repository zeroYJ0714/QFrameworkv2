#pragma once

// 文件职责：向应用和插件提供框架版本字符串查询接口。

#include "QFrameworkGlobal.h"

namespace qframework
{
/// @brief 查询当前 QFramework 二进制版本。
/// @return 指向静态只读、以 NUL 结尾的版本字符串；调用方不得释放或修改。
QFRAMEWORK_EXPORT const char* frameworkVersion();
}
