# 操作指南：构建、运行、调试与故障处理

本文按具体任务组织，适用于已经完成项目配置的开发者。

## 构建一个组合

1. 在 Visual Studio 工具栏选择 `Debug` 或 `Release`。
2. 选择 `Win32` 或 `x64`。
3. 执行“生成 > 重新生成解决方案”。
4. 在“生成”输出中确认 8 个项目均参与，失败数为 0。
5. 检查 `Bin\v141\<x86|x64>\<Debug|Release>` 和 `Build\Lib\v141\<x86|x64>\<Debug|Release>`。

最终验收必须分别在 VS2017 和 VS2026 中完成四个组合，合计八次。命令行构建可以帮助定位问题，但不能替代这八次 Visual Studio GUI 验收。

## 准备运行目录

每个运行目录至少应包含：

- `QFrameworkApp.exe`、`QFramework.dll`、`QFrameworkProtocols.dll`；
- Qt 运行库和 `platforms\qwindows.dll`；
- 对应配置的 `libprotobuf.dll` 或 `libprotobufd.dll`；
- `Plugins\<ModuleId>` 下的四个示例模块；
- 手工复制的 `config\QFramework.ini` 和 `config\Styles\Default.qss`；
- Debug 目录中的 EXE、DLL PDB；
- 运行后创建的 `Logs` 目录。

Release 使用 `/MD`，适合作为便携目录；目标机器仍需要匹配架构的 VC++ 运行库。Debug 使用 `/MDd`，只保证在装有 Visual Studio 调试环境的开发机运行。

## 显示与隐藏 UI 模块

1. 启动后主窗口保持空白，这是正常状态。
2. 打开“模块”菜单，选择主进程 UI 或子进程 UI 模块；也可以点击左侧工具栏的模块管理按钮。
3. 第一次显示时 Dock 位于左侧；多个 Dock 可以叠放为标签页。
4. 关闭 Dock 只隐藏模块，不调用 `onStop()`。
5. 拖动时可以看到浮动预览，松开后 Dock 会回到主窗口，不能长期浮动。

## 保存和加载布局

- “文件 > 布局另存为...”创建新的 `.qflayout`。
- “文件 > 保存当前布局”覆盖当前活动布局；尚无活动文件时会转到“另存为”。
- “文件 > 加载布局...”可以在运行中切换布局。
- 布局保存主窗口几何、Dock 位置、标签、大小和显示状态。
- 缺失模块会被跳过；布局中没有的 Dock 会隐藏。
- 退出不会自动保存布局，也不会写入 `QFramework.ini`。

若要启动时加载布局，把 `[Layout]/StartupFile` 设为相对 `config` 的路径或绝对路径，然后重启程序。损坏或缺失的启动布局只产生警告，主窗口继续保持空白。

## 切换和重新加载 QSS

1. 选择“样式 > 选择 QSS...”，打开任意 `.qss`。
2. 当前主进程界面和子进程 UI 会同步应用样式。
3. 修改磁盘上的当前 QSS 后，选择“样式 > 重新加载当前 QSS”。
4. 运行中选择的路径只保存在内存，下次启动仍读取 INI 中的 `[Style]/File`。

QSS 文件无法读取或大括号结构不完整时，框架保留上一次成功样式，同时记录日志并弹出警告。

## 手动重启子进程

1. 打开“模块 > 模块管理”。
2. 找到 `ProcessUi` 或 `ProcessNonUi` 模块。
3. 点击重启图标。手动重启会清空该模块的自动重启计数和未处理消息。
4. `ProcessUi` 重启期间原 Dock 保持位置并显示占位内容，重新注册并提供窗口句柄后恢复嵌入。

注册超时、心跳超时和异常退出都会触发自动重启。在 `RestartWindowMs` 内达到 `MaxRestartCount` 后，框架停止自动重启并弹窗；此时仍可手动重启。

## 调试主进程 DLL 模块

1. 把 `QFrameworkApp` 设为启动项目。
2. 在 DLL 模块的 `onStart()`、`onMessage()` 或业务代码中设置断点。
3. 使用 Debug 组合启动调试。`QPluginLoader` 加载 DLL 后断点会绑定。
4. `onStart()` 和 `onStop()` 在主进程 GUI 线程运行；`onMessage()` 在模块消息线程运行。查看线程窗口时不要把这两类调用混淆。

## 调试子进程模块

1. 仅在 Debug 运行目录的 `config\QFramework.ini` 中，把目标子进程的 `WaitForDebugger` 改为 `true`。
2. 设置足够的 `DebuggerWaitTimeoutMs`，例如 `30000`。
3. 启动 `QFrameworkApp`。子进程在执行 `onStart()` 前等待。
4. 在 Visual Studio 中选择“调试 > 附加到进程”，附加到目标 `ProcessUiExample.exe` 或 `ProcessNonUiExample.exe`，代码类型使用本机代码。
5. 在超时前完成附加；随后 `onStart()` 断点可以命中。超时后子进程继续运行并记录警告。

Release 忽略调试等待。完成调试后手工把 `WaitForDebugger` 恢复为 `false`；程序本身永远不会修改 INI。

## 验证消息与界面线程

- 两个 UI 示例启动时发布 `QFRAMEWORK_STATUS`。
- 两个非 UI 示例订阅状态并发布 `QFRAMEWORK_LOG_DISPLAY`。
- UI 示例接收日志消息后发射信号，再通过 `Qt::QueuedConnection` 更新标签。
- UI 示例的按钮分别打开模态和非模态对话框；子进程模态对话框只阻塞该子进程。
- 日志应显示模块 ID、线程 ID、毫秒时间和级别。

## 常见故障

### VS 报告重复项目项

同一头文件不能同时作为 `QtMoc` 和 `ClInclude`。保留 `QtMoc`，删除重复的 `ClInclude`，重新加载项目。

### 模块未加载

依次检查 `[Modules]/Names`、`Enabled`、`Type`、文件路径、架构和 Debug/Release 是否一致。主进程 DLL 还要检查插件 JSON 中的 `ModuleId` 与 `ModuleType`。

### 程序提示“程序已在运行”

同一程序目录只允许一个 `QFrameworkApp.exe`。关闭原实例后再启动；不同完整目录可以各运行一个实例。

### 子进程独立运行后立即退出

这是预期行为。子进程需要框架生成的服务器名和随机令牌，只能由 `QFrameworkApp` 启动。

### 运行时找不到 DLL

先确认目标架构，再检查 Qt、Protobuf、QFramework、QFrameworkProtocols 和插件 DLL 是否来自同一配置。不要把 Win32 与 x64、Debug 与 Release 文件混用。
