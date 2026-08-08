#pragma once

// QFRAMEWORK_IMAGE_RAW：负载为 ImageFrame；图像采集模块发布，处理/显示模块通过 onMessage 接收；非 ADB 主题不使用 Msg.src/dst；配置为 Latest，当前上限 16 MiB。
#define QFRAMEWORK_IMAGE_RAW "QFRAMEWORK_IMAGE_RAW"
// QFRAMEWORK_IMAGE_PROCESSED：负载为 ProcessedImage；算法模块发布，显示/存储模块通过 onMessage 接收；非 ADB 主题不使用 Msg.src/dst；未单独配置时继承 Reliable 和默认大小上限。
#define QFRAMEWORK_IMAGE_PROCESSED "QFRAMEWORK_IMAGE_PROCESSED"
// QFRAMEWORK_LOG_DISPLAY：负载为 LogDisplayMessage；业务模块发布，UI 模块通过 onMessage 接收；非 ADB 主题不使用 Msg.src/dst；未单独配置时继承 Reliable 和默认大小上限。
#define QFRAMEWORK_LOG_DISPLAY "QFRAMEWORK_LOG_DISPLAY"
// QFRAMEWORK_STATUS：负载为 ModuleStatus；模块/监督器发布，状态订阅模块通过 onMessage 接收；非 ADB 主题不使用 Msg.src/dst；未单独配置时继承 Reliable 和默认大小上限。
#define QFRAMEWORK_STATUS "QFRAMEWORK_STATUS"

// QFRAMEWORK_SQL_REQUEST：外层 Msg.str 为 SqlRequest；调用模块发布给 SQL_InProcessNonUi；Reliable，单条上限 16 MiB。
#define QFRAMEWORK_SQL_REQUEST "QFRAMEWORK_SQL_REQUEST"
// QFRAMEWORK_SQL_RESPONSE：外层 Msg.str 为 SqlResponse；SQL_InProcessNonUi 定向回复原请求模块；Reliable，单条上限 16 MiB。
#define QFRAMEWORK_SQL_RESPONSE "QFRAMEWORK_SQL_RESPONSE"
// QFRAMEWORK_CURRENT_DEVICE_QUERY：外层 Msg.str 为 CurrentDeviceQuery；ImageShow_InProcessUi 定向查询 ADBShow_InProcessUi；Reliable。
#define QFRAMEWORK_CURRENT_DEVICE_QUERY "QFRAMEWORK_CURRENT_DEVICE_QUERY"
// QFRAMEWORK_CURRENT_DEVICE_RESPONSE：外层 Msg.str 为 CurrentDeviceResponse；ADBShow_InProcessUi 定向回复 ImageShow_InProcessUi；Reliable。
#define QFRAMEWORK_CURRENT_DEVICE_RESPONSE "QFRAMEWORK_CURRENT_DEVICE_RESPONSE"
// QFRAMEWORK_CURRENT_DEVICE_CHANGED：外层 Msg.str 为 CurrentDeviceChanged；ADBShow_InProcessUi 广播当前 android_id；Reliable。
#define QFRAMEWORK_CURRENT_DEVICE_CHANGED "QFRAMEWORK_CURRENT_DEVICE_CHANGED"

// QFRAMEWORK_ADB_REQUEST：外层 Msg.str 为 AdbRequest；调用模块发布，src=调用模块、dst=ADB_InProcessNonUi；由 ADB_InProcessNonUi::onMessage 接收后以 QueuedConnection 交给协调 Worker；Reliable。
#define QFRAMEWORK_ADB_REQUEST "QFRAMEWORK_ADB_REQUEST"
// QFRAMEWORK_ADB_RESPONSE：外层 Msg.str 为 AdbResponse；ADB_InProcessNonUi 发布，src=ADB_InProcessNonUi、dst=原请求模块；请求模块通过 onMessage 接收；Reliable，request_id 原样返回。
#define QFRAMEWORK_ADB_RESPONSE "QFRAMEWORK_ADB_RESPONSE"
// QFRAMEWORK_ADB_DEVICE_EVENT：外层 Msg.str 为 AdbDeviceEvent；ADB_InProcessNonUi 根据 AndroidDeviceManager::devicesChanged/deviceStateChanged 信号发布，src=ADB_InProcessNonUi、dst 为空广播；Reliable。
#define QFRAMEWORK_ADB_DEVICE_EVENT "QFRAMEWORK_ADB_DEVICE_EVENT"
// QFRAMEWORK_ADB_COMMAND_OUTPUT：外层 Msg.str 为 AdbOutputEvent；由 adbOutputReady/adbErrorOutputReady 信号触发发布，src=ADB_InProcessNonUi、dst=命令原请求模块；订阅模块 onMessage 接收；Reliable。
#define QFRAMEWORK_ADB_COMMAND_OUTPUT "QFRAMEWORK_ADB_COMMAND_OUTPUT"
// QFRAMEWORK_ADB_COMMAND_FINISHED：外层 Msg.str 为 AdbCommandFinishedEvent；由 adbCommandFinished 信号触发发布，src=ADB_InProcessNonUi、dst=命令原请求模块；订阅模块 onMessage 接收；Reliable，每个已接受命令只发布一次最终事件。
#define QFRAMEWORK_ADB_COMMAND_FINISHED "QFRAMEWORK_ADB_COMMAND_FINISHED"
// QFRAMEWORK_ADB_VIDEO_EVENT：外层 Msg.str 只允许 AdbVideoEvent.frame；由 videoFrameReady 信号触发发布，src=ADB_InProcessNonUi、dst 为空广播；订阅模块 onMessage 接收；Latest，单条消息上限 64 MiB，禁止发布 encoded。
#define QFRAMEWORK_ADB_VIDEO_EVENT "QFRAMEWORK_ADB_VIDEO_EVENT"
// QFRAMEWORK_ADB_AUDIO_EVENT：外层 Msg.str 只允许 AdbAudioEvent.pcm；由 audioPcmReady 信号触发发布，src=ADB_InProcessNonUi、dst 为空广播；订阅模块 onMessage 接收；Latest，禁止发布 encoded。
#define QFRAMEWORK_ADB_AUDIO_EVENT "QFRAMEWORK_ADB_AUDIO_EVENT"
// QFRAMEWORK_ADB_DISPLAY_EVENT：外层 Msg.str 为 AdbDisplayEvent；由 displayInfoChanged 信号触发发布，src=ADB_InProcessNonUi、dst 为空广播；订阅模块 onMessage 接收；Reliable。
#define QFRAMEWORK_ADB_DISPLAY_EVENT "QFRAMEWORK_ADB_DISPLAY_EVENT"
// QFRAMEWORK_ADB_MEDIA_EVENT：外层 Msg.str 为 AdbMediaEvent；由 mediaStateChanged/mediaStatisticsChanged 信号触发发布，src=ADB_InProcessNonUi、dst 为空广播；订阅模块 onMessage 接收；Reliable。
#define QFRAMEWORK_ADB_MEDIA_EVENT "QFRAMEWORK_ADB_MEDIA_EVENT"
// QFRAMEWORK_ADB_CLIPBOARD_EVENT：外层 Msg.str 为 AdbClipboardEvent；由 clipboardReceived 信号触发发布，src=ADB_InProcessNonUi、dst 为空广播；订阅模块 onMessage 接收；Reliable。
#define QFRAMEWORK_ADB_CLIPBOARD_EVENT "QFRAMEWORK_ADB_CLIPBOARD_EVENT"
// QFRAMEWORK_ADB_ERROR_EVENT：外层 Msg.str 为 AdbErrorEvent；由 errorOccurred 信号或适配层错误路径触发发布，src=ADB_InProcessNonUi、dst 为空广播；订阅模块 onMessage 接收；Reliable。
#define QFRAMEWORK_ADB_ERROR_EVENT "QFRAMEWORK_ADB_ERROR_EVENT"
