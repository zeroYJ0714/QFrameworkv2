#pragma once

// 文件职责：声明主进程 DLL 插件的加载、校验、启动和反向关闭管理器。
// 子进程 EXE 不经过这里，而是由 ProcessSupervisor 管理。

#include <QObject>
#include <QVector>

#include "FrameworkConfig.h"
#include "QFrameworkGlobal.h"

namespace qframework
{
class InProcessUiModule;
class MessageBus;

class QFRAMEWORK_EXPORT PluginManager : public QObject
{
    Q_OBJECT

public:
    // messageBus 是借用指针，必须比 PluginManager 活得更久。
    /// @brief 创建 PluginManager 对象并初始化其基础状态。
    /// @param messageBus 传给该操作的 `messageBus` 参数；取值应符合声明类型和函数用途。
    /// @param parent Qt 父对象或父控件；用于建立所有权关系，允许为空。
    /// @return 无（构造函数不返回值）。
    explicit PluginManager(MessageBus* messageBus, QObject* parent = nullptr);
    /// @brief 销毁 PluginManager 对象并释放其拥有的资源。
    /// @return 无（析构函数不返回值）。
    ~PluginManager() override;

    // 只筛选 InProcessUi/InProcessNonUi；单个模块失败会写入 errors，
    // 其他模块仍继续加载。返回值表示是否全部成功。
    /// @brief 加载 `loadAndStart` 对应的框架操作。
    /// @param modules 模块配置列表。
    /// @param errors 可选错误输出列表。
    /// @param enableDeliveryAfterStart 传给该操作的 `enableDeliveryAfterStart` 参数；取值应符合声明类型和函数用途。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool loadAndStart(const QVector<ModuleConfig>& modules,
                      QStringList* errors = nullptr,
                      bool enableDeliveryAfterStart = true); ///< `enableDeliveryAfterStart` 对应的布尔状态标志。
    // 队列已停止的插件才调用 onStop/卸载；超时 ID 保留在 quarantine。
    /// @brief 关闭 `shutdown` 对应的框架操作。
    /// @param timedOutModuleIds 传给该操作的 `timedOutModuleIds` 参数；取值应符合声明类型和函数用途。
    /// @return 返回对应 Qt 值或容器的安全副本。
    QStringList shutdown(const QStringList& timedOutModuleIds = QStringList());
    // 测试替代 fail-fast 或协作回调稍后返回时，可再次回收已经安全停止的隔离项。
    /// @brief 执行 `retryQuarantinedShutdown` 所定义的类职责。
    /// @return 返回对应 Qt 值或容器的安全副本。
    QStringList retryQuarantinedShutdown();

    // 查询结果是快照；uiModule 返回借用指针，所有权仍在 QPluginLoader。
    /// @brief 执行 `runningModuleIds` 所定义的类职责。
    /// @return 返回对应 Qt 值或容器的安全副本。
    QStringList runningModuleIds() const;
    /// @brief 执行 `quarantinedModuleIds` 所定义的类职责。
    /// @return 返回对应 Qt 值或容器的安全副本。
    QStringList quarantinedModuleIds() const;
    /// @brief 执行 `uiModule` 所定义的类职责。
    /// @param moduleId 稳定模块 ID。
    /// @return 返回借用指针；找不到目标时返回 nullptr。
    InProcessUiModule* uiModule(const QString& moduleId) const;

signals:
    /// @brief 执行 `moduleStateChanged` 所定义的类职责。
    /// @param moduleId 稳定模块 ID。
    /// @param state 状态值。
    /// @param detail 状态或故障详情。
    /// @return 无。
    void moduleStateChanged(const QString& moduleId,
                            const QString& state,
                            const QString& detail);

private:
    struct LoadedPlugin;

    /// @brief 加载 `loadOne` 对应的框架操作。
    /// @param config 运行配置值。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool loadOne(const ModuleConfig& config, QString* errorMessage);
    /// @brief 启动 `startOne` 对应的框架操作。
    /// @param plugin 传给该操作的 `plugin` 参数；取值应符合声明类型和函数用途。
    /// @param errorMessage 可选错误说明输出参数。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool startOne(LoadedPlugin* plugin, QString* errorMessage);
    /// @brief 执行 `removeFailed` 所定义的类职责。
    /// @param plugin 传给该操作的 `plugin` 参数；取值应符合声明类型和函数用途。
    /// @return 无。
    void removeFailed(LoadedPlugin* plugin);
    /// @brief 释放 `releasePlugin` 对应的框架操作。
    /// @param plugin 传给该操作的 `plugin` 参数；取值应符合声明类型和函数用途。
    /// @return 条件或操作成功时返回 true，否则返回 false。
    bool releasePlugin(LoadedPlugin* plugin);

    // loaded_ 中的记录和 QPluginLoader 由本类拥有；endpoint/instance 只是视图。
    MessageBus* messageBus_; ///< `messageBus_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    QVector<LoadedPlugin*> loaded_; ///< `loaded_` 对应的对象指针；所有权和线程归属见本类文件级说明。
    bool shutdownComplete_; ///< `shutdownComplete_` 对应的布尔状态标志。
};
}
