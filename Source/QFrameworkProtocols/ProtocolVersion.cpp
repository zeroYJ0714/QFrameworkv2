// 【阅读导引】报告编译使用的 Protobuf 版本。
// 阅读顺序：实现读取 GOOGLE_PROTOBUF_VERSION 并转换成版本字符串，便于诊断依赖是否匹配。
// 需要分清：这不是业务协议迁移版本，也不会升级数据库；调用方只读取返回的静态字符存储。
// 这是执行实现：按入口调用和信号回调追踪，不要按文件行号把所有函数理解成连续执行。

// 文件职责：返回 Protobuf 编译版本的可读字符串；不是父子进程 IPC 格式版本。
// 不创建 QObject、不访问网络或数据库，适合用于启动日志和依赖诊断。
#include "ProtocolVersion.h"

// 实现只读取 Protobuf 的编译期版本，不创建网络连接，也不修改全局状态。

#include <google/protobuf/stubs/common.h>

namespace qframework
{
/// @brief 返回 Protobuf 编译期版本的可读字符串。
/// @return 指向函数内静态 std::string 的只读 C 字符串，调用方不得释放。
const char* protobufRuntimeVersion()
{
    // static 保证 std::string 的存储在函数返回后仍然存在；返回 c_str() 才安全。
    static const std::string version =
        google::protobuf::internal::VersionString(GOOGLE_PROTOBUF_VERSION); ///< 编译期确定的版本文本，静态存储期保证返回指针长期有效。
    return version.c_str();
}
}
