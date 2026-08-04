#include "ProtocolVersion.h"

#include <google/protobuf/stubs/common.h>

namespace qframework
{
const char* protobufRuntimeVersion()
{
    static const std::string version =
        google::protobuf::internal::VersionString(GOOGLE_PROTOBUF_VERSION);
    return version.c_str();
}
}
