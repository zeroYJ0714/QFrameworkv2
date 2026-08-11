#pragma once

// 文件职责：声明协议 DLL 的 Protobuf 运行时版本查询接口，供诊断和测试使用。

#include "QFrameworkGlobal.h"

namespace qframework
{
// 返回 Protobuf 头文件编译版本对应的静态字符串，不需要调用方释放。
/// @brief 执行 `protobufRuntimeVersion` 所定义的类职责。
/// @return 返回借用指针；找不到目标时返回 nullptr。
QFRAMEWORK_PROTOCOLS_EXPORT const char* protobufRuntimeVersion();
}
