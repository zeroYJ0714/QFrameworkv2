#pragma once

// 文件职责：向应用和插件提供框架版本字符串查询接口。

#include "QFrameworkGlobal.h"

namespace qframework
{
// 返回静态存储的版本字符串，调用方不需要释放，也不应修改返回内容。
QFRAMEWORK_EXPORT const char* frameworkVersion();
}
