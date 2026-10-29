// 【阅读导引】报告编译使用的 Protobuf 版本。
// 阅读顺序：实现读取 GOOGLE_PROTOBUF_VERSION 并转换成版本字符串，便于诊断依赖是否匹配。
// 需要分清：这不是业务协议迁移版本，也不会升级数据库；调用方只读取返回的静态字符存储。
// 这是声明/类型入口：先看 public 接口和字段注释；复杂执行过程再到同名 .cpp 或指定实现文件阅读。

#pragma once

// 文件职责：声明协议 DLL 的 Protobuf 运行时版本查询接口，供诊断和测试使用。

#include "QFrameworkGlobal.h"

namespace qframework
{
// 返回 Protobuf 头文件编译版本对应的静态字符串，不需要调用方释放。
/// @brief 返回编译所使用的 Protobuf 版本字符串，供诊断依赖版本。
/// @return 静态存储的只读字符指针，调用者不能释放。
QFRAMEWORK_PROTOCOLS_EXPORT const char* protobufRuntimeVersion();
}
