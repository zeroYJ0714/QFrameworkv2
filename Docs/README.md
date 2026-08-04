# QFramework 文档

本目录面向使用 Visual Studio 2017 或 Visual Studio 2026 开发 QFramework 模块的中文读者。文档描述当前仓库中的实际接口、项目设置和运行行为；架构边界仍以仓库根目录的 `开发计划.md` 为准。

## 从这里开始

- [教程：用 Visual Studio 配置四类模块](Tutorials/CreateFourModuleTypes.md)：从新建 Qt 项目到写入模块配置并完成第一次运行。
- [操作指南：构建、运行、调试与故障处理](Guides/OperateAndDebug.md)：完成日常构建、Dock、布局、QSS、重启和调试操作。
- [模块 SDK 参考](Reference/ModuleSdk.md)：模块基类、生命周期、消息、日志、主题和 Protobuf 接口。
- [QFramework.ini 参考](Reference/QFrameworkIni.md)：全部分组、键、默认值和路径解析规则。
- [架构说明](Explanation/Architecture.md)：理解中央消息总线、进程边界、队列、IPC、Dock 和关闭顺序。

## 固定开发条件

| 项目 | 要求 |
| --- | --- |
| Qt | 5.15.2，Win32 使用 `msvc2019`，x64 使用 `msvc2019_64` |
| Visual Studio | VS2017、VS2026 |
| 编译工具集 | `v141` |
| C++ | C++17 |
| Protobuf | 3.21.12 |
| 运行库 | Debug `/MDd`，Release `/MD` |
| 工程系统 | 唯一 `QFramework.sln`，只维护 `.sln` 和 `.vcxproj` |

`config\QFramework.ini` 是只读运行配置。程序不会写回它；所有相对路径均以该 INI 所在的 `config` 目录为基准。

## 示例位置

四种模块类型均有可构建示例：

- `Modules\InProcessUiExample`
- `Modules\InProcessNonUiExample`
- `Modules\ProcessUiExample`
- `Modules\ProcessNonUiExample`

新模块应从 Visual Studio 的 Qt 默认项目开始配置，不要把这些示例复制成仓库自定义模板。
