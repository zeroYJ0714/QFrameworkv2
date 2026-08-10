#pragma once

// QFRAMEWORK_IMAGE_RAW：负载为序列化后的外层 Msg；配置为 Latest，当前上限 16 MiB。
#define QFRAMEWORK_IMAGE_RAW "QFRAMEWORK_IMAGE_RAW"
// QFRAMEWORK_IMAGE_PROCESSED：负载为序列化后的外层 Msg；未单独配置时继承 Reliable 和默认大小上限。
#define QFRAMEWORK_IMAGE_PROCESSED "QFRAMEWORK_IMAGE_PROCESSED"
// QFRAMEWORK_LOG_DISPLAY：负载为序列化后的外层 Msg；未单独配置时继承 Reliable 和默认大小上限。
#define QFRAMEWORK_LOG_DISPLAY "QFRAMEWORK_LOG_DISPLAY"
// QFRAMEWORK_STATUS：负载为序列化后的外层 Msg；未单独配置时继承 Reliable 和默认大小上限。
#define QFRAMEWORK_STATUS "QFRAMEWORK_STATUS"
