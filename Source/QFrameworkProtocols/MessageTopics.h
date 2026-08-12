#pragma once

// 本文件中每个主题只说明三项：发送模块 -> 接收模块、作用、Msg.str 对应的 Proto 结构。

// 发送：图像采集模块 -> 图像处理模块、图像显示模块。
// 作用：传递未经算法处理的原始图像帧。
// Proto：外层为 Msg，Msg.str 中保存 ImageFrame。
#define QFRAMEWORK_IMAGE_RAW "QFRAMEWORK_IMAGE_RAW"
// 发送：图像处理模块 -> 图像显示模块、图像存储模块。
// 作用：传递算法处理后的图像及其来源帧序号。
// Proto：外层为 Msg，Msg.str 中保存 ProcessedImage。
#define QFRAMEWORK_IMAGE_PROCESSED "QFRAMEWORK_IMAGE_PROCESSED"
// 发送：业务模块 -> 日志显示 UI 模块。
// 作用：把结构化日志内容发送到界面显示。
// Proto：外层为 Msg，Msg.str 中保存 LogDisplayMessage。
#define QFRAMEWORK_LOG_DISPLAY "QFRAMEWORK_LOG_DISPLAY"
// 发送：业务模块或进程监督模块 -> 状态显示/监控模块。
// 作用：通知模块当前的生命周期状态和状态说明。
// Proto：外层为 Msg，Msg.str 中保存 ModuleStatus。
#define QFRAMEWORK_STATUS "QFRAMEWORK_STATUS"

// 发送：ADBShow_InProcessUi、ImageShow_InProcessUi 或其他调用模块 -> SQL_InProcessNonUi。
// 作用：请求执行 SQL 语句、查询或事务批次。
// Proto：外层为 Msg，Msg.str 中保存 SqlRequest。
#define QFRAMEWORK_SQL_REQUEST "QFRAMEWORK_SQL_REQUEST"
// 发送：SQL_InProcessNonUi -> 原 SQL 请求模块。
// 作用：返回 SQL 执行结果、查询数据或错误信息。
// Proto：外层为 Msg，Msg.str 中保存 SqlResponse。
#define QFRAMEWORK_SQL_RESPONSE "QFRAMEWORK_SQL_RESPONSE"
// 发送：ImageShow_InProcessUi -> ADBShow_InProcessUi。
// 作用：查询 ADBShow 当前选择的设备。
// Proto：外层为 Msg，Msg.str 中保存 CurrentDeviceQuery。
#define QFRAMEWORK_CURRENT_DEVICE_QUERY "QFRAMEWORK_CURRENT_DEVICE_QUERY"
// 发送：ADBShow_InProcessUi -> ImageShow_InProcessUi。
// 作用：返回当前选择设备的 android_id。
// Proto：外层为 Msg，Msg.str 中保存 CurrentDeviceResponse。
#define QFRAMEWORK_CURRENT_DEVICE_RESPONSE "QFRAMEWORK_CURRENT_DEVICE_RESPONSE"
// 发送：ADBShow_InProcessUi -> ImageShow_InProcessUi。
// 作用：当前设备发生切换时通知新的 android_id。
// Proto：外层为 Msg，Msg.str 中保存 CurrentDeviceChanged。
#define QFRAMEWORK_CURRENT_DEVICE_CHANGED "QFRAMEWORK_CURRENT_DEVICE_CHANGED"

// 发送：ADBShow_InProcessUi、ImageShow_InProcessUi 或其他调用模块 -> ADB_InProcessNonUi。
// 作用：请求执行设备枚举、启停、ADB 命令、媒体或控制操作。
// Proto：外层为 Msg，Msg.str 中保存 AdbRequest。
#define QFRAMEWORK_ADB_REQUEST "QFRAMEWORK_ADB_REQUEST"
// 发送：ADB_InProcessNonUi -> 原 ADB 请求模块。
// 作用：确认请求是否被接受，并返回同步结果或错误。
// Proto：外层为 Msg，Msg.str 中保存 AdbResponse。
#define QFRAMEWORK_ADB_RESPONSE "QFRAMEWORK_ADB_RESPONSE"
// 发送：ADB_InProcessNonUi -> ADBShow_InProcessUi、ImageShow_InProcessUi。
// 作用：广播设备列表变化或单台设备状态变化。
// Proto：外层为 Msg，Msg.str 中保存 AdbDeviceEvent。
#define QFRAMEWORK_ADB_DEVICE_EVENT "QFRAMEWORK_ADB_DEVICE_EVENT"
// 发送：ADB_InProcessNonUi -> 原 ADB 命令请求模块。
// 作用：流式返回 ADB 命令的标准输出或错误输出。
// Proto：外层为 Msg，Msg.str 中保存 AdbOutputEvent。
#define QFRAMEWORK_ADB_COMMAND_OUTPUT "QFRAMEWORK_ADB_COMMAND_OUTPUT"
// 发送：ADB_InProcessNonUi -> 原 ADB 命令请求模块，例如 ADBShow_InProcessUi。
// 作用：通知一条异步 ADB 命令已经成功、失败、取消或超时结束。
// Proto：外层为 Msg，Msg.str 中保存 AdbCommandFinishedEvent。
#define QFRAMEWORK_ADB_COMMAND_FINISHED "QFRAMEWORK_ADB_COMMAND_FINISHED"
// 发送：ADB_InProcessNonUi -> ImageShow_InProcessUi。
// 作用：传递当前设备解码后的 ARGB32 视频帧。
// Proto：外层为 Msg，Msg.str 中保存 AdbVideoEvent，使用其中的 frame 字段。
#define QFRAMEWORK_ADB_VIDEO_EVENT "QFRAMEWORK_ADB_VIDEO_EVENT"
// 发送：ADB_InProcessNonUi -> ImageShow_InProcessUi。
// 作用：传递当前设备解码后的 PCM 音频数据。
// Proto：外层为 Msg，Msg.str 中保存 AdbAudioEvent，使用其中的 pcm 字段。
#define QFRAMEWORK_ADB_AUDIO_EVENT "QFRAMEWORK_ADB_AUDIO_EVENT"
// 发送：ADB_InProcessNonUi -> ImageShow_InProcessUi。
// 作用：通知设备画面的宽高、旋转方向和刷新率。
// Proto：外层为 Msg，Msg.str 中保存 AdbDisplayEvent。
#define QFRAMEWORK_ADB_DISPLAY_EVENT "QFRAMEWORK_ADB_DISPLAY_EVENT"
// 发送：ADB_InProcessNonUi -> ImageShow_InProcessUi。
// 作用：通知媒体会话状态变化或最新媒体统计数据。
// Proto：外层为 Msg，Msg.str 中保存 AdbMediaEvent。
#define QFRAMEWORK_ADB_MEDIA_EVENT "QFRAMEWORK_ADB_MEDIA_EVENT"
// 发送：ADB_InProcessNonUi -> 订阅设备剪贴板事件的模块。
// 作用：通知设备剪贴板中的最新文本。
// Proto：外层为 Msg，Msg.str 中保存 AdbClipboardEvent。
#define QFRAMEWORK_ADB_CLIPBOARD_EVENT "QFRAMEWORK_ADB_CLIPBOARD_EVENT"
// 发送：ADB_InProcessNonUi -> ADBShow_InProcessUi、ImageShow_InProcessUi。
// 作用：广播设备、ADB、媒体或控制流程中的结构化错误。
// Proto：外层为 Msg，Msg.str 中保存 AdbErrorEvent。
#define QFRAMEWORK_ADB_ERROR_EVENT "QFRAMEWORK_ADB_ERROR_EVENT"
