#include "ProtocolVersion.h"

// 实现只读取 Protobuf 的编译期版本，不创建网络连接，也不修改全局状态。

#include <google/protobuf/stubs/common.h>

namespace qframework
{
const char* protobufRuntimeVersion()
{
    // static 保证 std::string 的存储在函数返回后仍然存在；返回 c_str() 才安全。
    static const std::string version =
        google::protobuf::internal::VersionString(GOOGLE_PROTOBUF_VERSION);
    return version.c_str();
}
}
