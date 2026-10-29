// 【阅读导引】规定消息的主题名称和载荷约定。
// 阅读顺序：发送方和接收方必须使用同一主题；主题负责路由，Msg.str 内的 Protobuf 才承载具体字段。
// 需要分清：新增主题还需订阅声明和队列策略。仅定义字符串不会自动创建订阅者，也不能证明相应业务模块已经实现。

#pragma once

// 本文件中每个主题只说明三项：发送模块 -> 接收模块、作用、Msg.str 对应的 Proto 结构。
// 标注“规划”的模块尚未接入，标注“外部服务”的模块另行实现；以下收发关系同时说明现有实现和协议约定。

// 发送：图像采集模块 -> 图像处理模块、图像显示模块。
// 作用：传递未经算法处理的原始图像帧。
// Proto：外层为 Msg，Msg.str 中保存 ImageFrame。
#define QFRAMEWORK_IMAGE_RAW "QFRAMEWORK_IMAGE_RAW"
// 发送：图像处理模块 -> 图像显示模块、图像存储模块。
// 作用：传递算法处理后的图像及其来源帧序号。
// Proto：外层为 Msg，Msg.str 中保存 ProcessedImage。
#define QFRAMEWORK_IMAGE_PROCESSED "QFRAMEWORK_IMAGE_PROCESSED"
// 发送：业务模块（含 ExecuteStep_InProcessNonUi） -> 日志显示 UI 模块（执行日志目标为规划中的 UseCaseLog_InProcessUi）。
// 作用：把结构化日志内容发送到界面显示。
// Proto：外层为 Msg，Msg.str 中保存 LogDisplayMessage。
#define QFRAMEWORK_LOG_DISPLAY "QFRAMEWORK_LOG_DISPLAY"
// 发送：业务模块或进程监督模块 -> 状态显示/监控模块。
// 作用：通知模块当前的生命周期状态和状态说明。
// Proto：外层为 Msg，Msg.str 中保存 ModuleStatus。
#define QFRAMEWORK_STATUS "QFRAMEWORK_STATUS"

// 发送：ADBShow_InProcessUi、ImageShow_InProcessUi、ExecuteStep_InProcessNonUi 或其他调用模块 -> SQL_InProcessNonUi。
// 作用：请求执行 SQL 语句、查询、事务批次或同一读事务内的一致快照查询。
// Proto：外层为 Msg，Msg.str 中保存 SqlRequest。
#define QFRAMEWORK_SQL_REQUEST "QFRAMEWORK_SQL_REQUEST"
// 发送：SQL_InProcessNonUi -> 原 SQL 请求模块。
// 作用：返回 SQL 执行结果、查询数据或错误信息。
// Proto：外层为 Msg，Msg.str 中保存 SqlResponse。
#define QFRAMEWORK_SQL_RESPONSE "QFRAMEWORK_SQL_RESPONSE"
// 发送：ImageShow_InProcessUi、ExecuteStep_InProcessNonUi -> ADBShow_InProcessUi。
// 作用：查询 ADBShow 当前选择的设备。
// Proto：外层为 Msg，Msg.str 中保存 CurrentDeviceQuery。
#define QFRAMEWORK_CURRENT_DEVICE_QUERY "QFRAMEWORK_CURRENT_DEVICE_QUERY"
// 发送：ADBShow_InProcessUi -> 原设备查询模块（ImageShow_InProcessUi、ExecuteStep_InProcessNonUi）。
// 作用：返回当前选择设备的 android_id，供调用方确定本次操作的设备。
// Proto：外层为 Msg，Msg.str 中保存 CurrentDeviceResponse。
#define QFRAMEWORK_CURRENT_DEVICE_RESPONSE "QFRAMEWORK_CURRENT_DEVICE_RESPONSE"
// 发送：ADBShow_InProcessUi -> ImageShow_InProcessUi、ExecuteStep_InProcessNonUi。
// 作用：当前设备发生切换时通知新的 android_id。
// Proto：外层为 Msg，Msg.str 中保存 CurrentDeviceChanged。
#define QFRAMEWORK_CURRENT_DEVICE_CHANGED "QFRAMEWORK_CURRENT_DEVICE_CHANGED"

// 发送：ADBShow_InProcessUi、ImageShow_InProcessUi、ExecuteStep_InProcessNonUi 或其他调用模块 -> ADB_InProcessNonUi。
// 作用：请求执行设备枚举、启停、ADB 命令、媒体或控制操作。
// Proto：外层为 Msg，Msg.str 中保存 AdbRequest。
#define QFRAMEWORK_ADB_REQUEST "QFRAMEWORK_ADB_REQUEST"
// 发送：ADB_InProcessNonUi -> 原 ADB 请求模块。
// 作用：确认请求是否被接受，并返回同步结果或错误；控制请求的 accepted 仅表示控制队列准入，不表示 Android 已完成动作。
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
// 发送：ADB_InProcessNonUi -> ImageShow_InProcessUi、ExecuteStep_InProcessNonUi。
// 作用：传递设备解码后的 ARGB32 视频帧，供界面显示或执行器冻结图像进行检测。
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

// 发送：ImageShow_InProcessUi -> 公共步骤目录订阅模块（规划中的 StepList_InProcessUi 等）。
// 作用：保存录制步骤并提交数据库后通知公共步骤目录刷新；接收方允许重复通知并重新查询 SQL。
// Proto：外层为 Msg，Msg.str 中保存 StepCatalogChanged。
#define QFRAMEWORK_USECASE_STEP_CATALOG_CHANGED "QFRAMEWORK_USECASE_STEP_CATALOG_CHANGED"

// 发送：UseCase_InProcessUi -> StepList_InProcessUi（点对点）。
// 作用：按当前用例类型定位公共步骤目录；携带 usecase_type 与回显用 usecase_id。
// Proto：外层为 Msg，Msg.str 中保存 StepListContext。
#define QFRAMEWORK_USECASE_STEPLIST_CONTEXT "QFRAMEWORK_USECASE_STEPLIST_CONTEXT"

// 发送：StepList_InProcessUi -> UseCaseStep_InProcessUi（点对点）。
// 作用：双击公共步骤时请求把它添加为指定用例的新实例。
// Proto：外层为 Msg，Msg.str 中保存 StepListAddRequest。
#define QFRAMEWORK_USECASE_STEPLIST_ADD "QFRAMEWORK_USECASE_STEPLIST_ADD"

// 发送：执行调用模块（规划中的 UseCase_InProcessUi、UseCaseStep_InProcessUi、TaskList_InProcessUi） -> ExecuteStep_InProcessNonUi。
// 作用：以 execution_id 请求执行测试组、完整用例或指定步骤实例，并按请求目标建立独立执行流程。
// Proto：外层为 Msg，Msg.str 中保存 ExecutionRequest。
#define QFRAMEWORK_USECASE_EXECUTION_REQUEST "QFRAMEWORK_USECASE_EXECUTION_REQUEST"
// 发送：执行调用模块（规划中的 UseCase_InProcessUi、UseCaseStep_InProcessUi、TaskList_InProcessUi） -> ExecuteStep_InProcessNonUi。
// 作用：停止匹配 execution_id 的执行，完成当前动作必要收尾并释放界面控制权；未知或已结束的执行只记录诊断。
// Proto：外层为 Msg，Msg.str 中保存 StopExecutionRequest。
#define QFRAMEWORK_USECASE_STOP_EXECUTION "QFRAMEWORK_USECASE_STOP_EXECUTION"
// 发送：ExecuteStep_InProcessNonUi -> 执行事件订阅模块（规划中的 UseCase_InProcessUi、UseCaseStep_InProcessUi、TaskList_InProcessUi）。
// 作用：广播执行、用例和步骤的状态、逻辑结果与错误；接收方按 execution_id 筛选，并按 event_sequence 排序去重。
// Proto：外层为 Msg，Msg.str 中保存 ExecutionEvent。
#define QFRAMEWORK_USECASE_EXECUTION_EVENT "QFRAMEWORK_USECASE_EXECUTION_EVENT"
// 发送：ExecuteStep_InProcessNonUi -> ADBShow_InProcessUi、ImageShow_InProcessUi（分别发送）。
// 作用：申请或释放指定 execution_id 的界面操作锁，防止手动设备操作与自动执行相互干扰。
// Proto：外层为 Msg，Msg.str 中保存 ExecutionControlRequest。
#define QFRAMEWORK_EXECUTION_CONTROL_REQUEST "QFRAMEWORK_EXECUTION_CONTROL_REQUEST"
// 发送：ADBShow_InProcessUi、ImageShow_InProcessUi -> ExecuteStep_InProcessNonUi。
// 作用：分别确认操作锁的申请或释放结果，返回对应请求、执行 ID、动作及失败原因。
// Proto：外层为 Msg，Msg.str 中保存 ExecutionControlResponse。
#define QFRAMEWORK_EXECUTION_CONTROL_RESPONSE "QFRAMEWORK_EXECUTION_CONTROL_RESPONSE"

// 发送：ExecuteStep_InProcessNonUi -> LocateAnything_ProcessNonUi（独立进程模块，内部访问 Python 网关）。
// 作用：提交冻结的视频帧 PNG、检测提示和来源帧信息，请求定位目标。
// Proto：外层为 Msg，Msg.str 中保存 ImageDetectionRequest。
#define QFRAMEWORK_LOCATEANYTHING_DETECTION_REQUEST "QFRAMEWORK_LOCATEANYTHING_DETECTION_REQUEST"
// 发送：LocateAnything_ProcessNonUi -> ExecuteStep_InProcessNonUi。
// 作用：返回目标检测框、来源帧关联信息或错误，供执行器判断是否匹配及计算点击位置。
// Proto：外层为 Msg，Msg.str 中保存 ImageDetectionResponse。
#define QFRAMEWORK_LOCATEANYTHING_DETECTION_RESPONSE "QFRAMEWORK_LOCATEANYTHING_DETECTION_RESPONSE"
// 发送：训练 UI -> NonUi；Msg.str=LocateAnythingTrainingRequest。request_id 关联单次操作。
// validate/start/stop/query/checkpoints/load/recover；恢复基础模型显式清空 adapter，恢复前配置只在当前 worker 生命周期有效。
#define QFRAMEWORK_LOCATEANYTHING_TRAINING_REQUEST "QFRAMEWORK_LOCATEANYTHING_TRAINING_REQUEST"
// 发送：NonUi -> 请求方；Msg.str=LocateAnythingTrainingResponse，每请求一个终态。
// 训练失败及推理恢复结果分别表达；start 超时先核实 run，不重试 start，不盲目回滚。
#define QFRAMEWORK_LOCATEANYTHING_TRAINING_RESPONSE "QFRAMEWORK_LOCATEANYTHING_TRAINING_RESPONSE"
// 发送：NonUi -> 订阅者；Msg.str=LocateAnythingTrainingEvent，按 run_id/log_sequence 去重。
// 时间为 Unix 毫秒；loss 仅在 has_loss=true 时有效，模拟 checkpoint 不可用于推理。
#define QFRAMEWORK_LOCATEANYTHING_TRAINING_EVENT "QFRAMEWORK_LOCATEANYTHING_TRAINING_EVENT"
// 发送：NonUi -> 订阅者；Msg.str=LocateAnythingRuntimeStatus。独立探测通道约每秒更新。
// backend_running 是网关身份验证结果，READY 才表示模式就绪；CUDA 不等于真实训练能力。
// timestamp_ms 为发布时 Unix 毫秒；UI 按本地单调时钟判定 15 秒快照过期。
#define QFRAMEWORK_LOCATEANYTHING_RUNTIME_STATUS "QFRAMEWORK_LOCATEANYTHING_RUNTIME_STATUS"

// 发送：ExecuteStep_InProcessNonUi -> VoiceEngine_ProcessNonUi（外部服务）。
// 作用：请求按指定文本和音色完成语音合成与播放，并指定本次请求的超时时间。
// Proto：外层为 Msg，Msg.str 中保存 VoicePlayRequest。
#define QFRAMEWORK_VOICE_PLAY_REQUEST "QFRAMEWORK_VOICE_PLAY_REQUEST"
// 发送：VoiceEngine_ProcessNonUi（外部服务） -> ExecuteStep_InProcessNonUi。
// 作用：返回语音合成与播放是否完成、耗时或失败原因。
// Proto：外层为 Msg，Msg.str 中保存 VoicePlayResponse。
#define QFRAMEWORK_VOICE_PLAY_RESPONSE "QFRAMEWORK_VOICE_PLAY_RESPONSE"
// 发送：ExecuteStep_InProcessNonUi -> VoiceEngine_ProcessNonUi（外部服务）。
// 作用：请求在指定时间内采集并识别语音，判断识别文本是否命中期望内容。
// Proto：外层为 Msg，Msg.str 中保存 VoiceExpectRequest。
#define QFRAMEWORK_VOICE_EXPECT_REQUEST "QFRAMEWORK_VOICE_EXPECT_REQUEST"
// 发送：VoiceEngine_ProcessNonUi（外部服务） -> ExecuteStep_InProcessNonUi。
// 作用：返回识别流程是否成功、是否匹配、实际文本及置信度，区分正常未匹配与服务错误。
// Proto：外层为 Msg，Msg.str 中保存 VoiceExpectResponse。
#define QFRAMEWORK_VOICE_EXPECT_RESPONSE "QFRAMEWORK_VOICE_EXPECT_RESPONSE"

// 发送：ExecuteStep_InProcessNonUi -> VehicleBus_ProcessNonUi（外部服务）。
// 作用：请求向指定通道发送一帧 CAN、CAN FD 或 LIN 数据，携带帧 ID、原始负载和帧格式标志。
// Proto：外层为 Msg，Msg.str 中保存 VehicleBusSendRequest。
#define QFRAMEWORK_VEHICLEBUS_SEND_REQUEST "QFRAMEWORK_VEHICLEBUS_SEND_REQUEST"
// 发送：VehicleBus_ProcessNonUi（外部服务） -> ExecuteStep_InProcessNonUi。
// 作用：返回总线帧是否发送成功及失败原因。
// Proto：外层为 Msg，Msg.str 中保存 VehicleBusSendResponse。
#define QFRAMEWORK_VEHICLEBUS_SEND_RESPONSE "QFRAMEWORK_VEHICLEBUS_SEND_RESPONSE"
// 发送：ExecuteStep_InProcessNonUi -> VehicleBus_ProcessNonUi（外部服务）。
// 作用：请求在指定时间内监听通道中的目标信号，按声明的值类型判断是否等于期望值。
// Proto：外层为 Msg，Msg.str 中保存 VehicleBusSignalRequest。
#define QFRAMEWORK_VEHICLEBUS_SIGNAL_REQUEST "QFRAMEWORK_VEHICLEBUS_SIGNAL_REQUEST"
// 发送：VehicleBus_ProcessNonUi（外部服务） -> ExecuteStep_InProcessNonUi。
// 作用：返回监听流程是否正常结束、是否匹配及实际信号值；正常到期未命中为 success=true、matched=false。
// Proto：外层为 Msg，Msg.str 中保存 VehicleBusSignalResponse。
#define QFRAMEWORK_VEHICLEBUS_SIGNAL_RESPONSE "QFRAMEWORK_VEHICLEBUS_SIGNAL_RESPONSE"
// 发送：UseCaseStep_InProcessUi 等执行调用模块 -> ExecuteStep_InProcessNonUi。
// 作用：以 execution_id 请求执行完整用例或指定步骤实例，并按请求目标建立独立执行流程。
// Proto：外层为 Msg，Msg.str 中保存 ExecutionRequest。
#define QFRAMEWORK_USECASE_EXECUTION_REQUEST "QFRAMEWORK_USECASE_EXECUTION_REQUEST"
// 发送：UseCaseStep_InProcessUi 等执行调用模块 -> ExecuteStep_InProcessNonUi。
// 作用：停止匹配 execution_id 的执行，完成当前动作必要收尾并释放界面控制权。
// Proto：外层为 Msg，Msg.str 中保存 StopExecutionRequest。
#define QFRAMEWORK_USECASE_STOP_EXECUTION "QFRAMEWORK_USECASE_STOP_EXECUTION"
// 发送：ExecuteStep_InProcessNonUi -> UseCaseStep_InProcessUi 等执行事件订阅模块。
// 作用：广播执行、用例和步骤的状态、逻辑结果与错误；接收方按 execution_id 筛选，按 event_sequence 排序去重。
// Proto：外层为 Msg，Msg.str 中保存 ExecutionEvent。
#define QFRAMEWORK_USECASE_EXECUTION_EVENT "QFRAMEWORK_USECASE_EXECUTION_EVENT"

// 发送：TaskList_InProcessUi -> TaskCase_InProcessUi、TaskLog_InProcessUi（广播）。
// 作用：任务定义（tasks / task_cases）事务提交成功后通知定义已变化，接收方重新查询 SQL 刷新。
// Proto：外层为 Msg，Msg.str 中保存 TaskDefinitionChanged。
#define QFRAMEWORK_TASK_DEFINITION_CHANGED "QFRAMEWORK_TASK_DEFINITION_CHANGED"

// 发送：TaskList_InProcessUi -> TaskCase_InProcessUi（点对点）。
// 作用：告知当前选中的任务/批次/用例/尝试，TaskCase 据此定位 task_<id>/ 批次目录并读取历史文件。
// 编码：直接发送 JSON 对象（字段名与 TaskSelectionContext 一致），不套 Msg 信封。
#define QFRAMEWORK_TASK_SELECTION_CONTEXT "QFRAMEWORK_TASK_SELECTION_CONTEXT"

// 发送：TaskCase_InProcessUi -> TaskLog_InProcessUi（点对点）。
// 作用：TaskCase 定位到具体任务/批次/用例/尝试后，通知 TaskLog 切换日志视图。
// 编码：直接发送 JSON 对象（字段名与 TaskLogContext 一致），不套 Msg 信封。
#define QFRAMEWORK_TASK_LOG_CONTEXT "QFRAMEWORK_TASK_LOG_CONTEXT"
