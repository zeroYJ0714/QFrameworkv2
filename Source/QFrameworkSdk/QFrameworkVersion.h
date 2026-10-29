// 【阅读导引】声明框架版本查询接口。
// 阅读顺序：实现位于 Source/QFramework/Core/FrameworkVersion.cpp，头文件只提供调用约定。
// 需要分清：导出宏使其他 DLL 可以调用该函数，函数返回的字符指针不转移所有权。
// 这是声明/类型入口：先看 public 接口和字段注释；复杂执行过程再到同名 .cpp 或指定实现文件阅读。

#pragma once

// 文件职责：向应用和插件提供框架版本字符串查询接口。

#include "QFrameworkGlobal.h"

namespace qframework
{
/// @brief 查询当前 QFramework 二进制版本。
/// @return 指向静态只读、以 NUL 结尾的版本字符串；调用方不得释放或修改。
QFRAMEWORK_EXPORT const char* frameworkVersion();
}
