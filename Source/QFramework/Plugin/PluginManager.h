// 【阅读导引】管理加载到主进程的 DLL 模块。
// 阅读顺序：loadAndStart 分为加载注册和业务启动两轮，保证前面的模块发消息时后面的订阅者已经登记。
// 需要分清：loadOne 核对文件名、元数据、接口类型；uiModule 可能返回 nullptr。关闭时等消息回调结束再卸载 DLL，否则虚函数代码可能已经不存在。
// 这是声明/类型入口：先看 public 接口和字段注释；复杂执行过程再到同名 .cpp 或指定实现文件阅读。

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
    /// @brief 创建插件管理器并保存中央消息总线借用指针；此时还没有加载任何插件 DLL。
    /// @param messageBus 中央消息总线借用指针，所有权在 FrameworkRuntime，必须比本对象活得更久。
    /// @param parent Qt 父对象，允许为空；只决定对象树归属，与插件 DLL 的所有权无关。
    /// @return 无（构造函数不返回值）。
    explicit PluginManager(MessageBus* messageBus, QObject* parent = nullptr);
    /// @brief 析构兜底：没有显式 shutdown 过时，先停总线队列再按超时名单回收插件，避免 DLL 残留在内存中。
    /// @return 无（析构函数不返回值）。
    ~PluginManager() override;

    /// @brief 按配置顺序分两轮执行：先 loadOne 注册全部插件，再 startOne 逐个 onStart；单个失败不回滚其他模块。
    /// @param modules 模块配置列表；enabled 为 false 或类型是 ProcessUi/ProcessNonUi 的项被跳过。
    /// @param errors 失败原因收集列表，允许为空；非空时按失败先后追加每个失败模块的说明。
    /// @param enableDeliveryAfterStart 启动轮结束后是否解除总线的投递暂停，一次性放行消息投递。
    /// @return 所有被选中的模块都加载并启动成功返回 true，任一模块失败返回 false。
    bool loadAndStart(const QVector<ModuleConfig>& modules,
                      QStringList* errors = nullptr,
                      bool enableDeliveryAfterStart = true);
    /// @brief 按加载逆序关闭插件，只回收消息队列已停止者；未停止或超时的标记为隔离并保留其 DLL。
    /// @param timedOutModuleIds 上游判定消息回调未在退出期限内结束的模块 ID；命中的模块不做 onStop 直接隔离。
    /// @return 关闭结束后仍需隔离保留的模块 ID；全部正常回收时为空。已关闭过时直接返回当前隔离名单。
    QStringList shutdown(const QStringList& timedOutModuleIds = QStringList());
    /// @brief 再次回收已经安全停止的隔离项；用于测试替代 fail-fast 或协作回调稍后才返回的场景。
    /// @return 本轮之后仍需隔离保留的模块 ID；成功回收的模块从此不再出现在名单里。
    QStringList retryQuarantinedShutdown();
    /// @brief 广播进入退出准备阶段，让已启动模块保存草稿；只转调端点契约，不停止消息队列。
    /// @return 无；未启动或已隔离的插件不通知。
    void beginPrepareForExit();
    /// @brief 查询是否所有已启动模块都完成了退出准备，供关闭窗口轮询。
    /// @return 所有已启动且未隔离的模块都已就绪返回 true；任一未就绪返回 false。
    bool isReadyForExit() const;
    /// @brief 列出尚未完成退出准备的已启动模块。
    QStringList pendingExitModules() const;
    /// @brief 用户放弃退出时撤销退出准备状态，让已启动模块恢复接收用户操作。
    /// @return 无；未启动或已隔离的插件不通知。
    void cancelPrepareForExit();

    /// @brief 列出已成功执行 onStart 的模块 ID。
    /// @return 按记录顺序（即配置加载顺序）排列的快照列表；未启动或已隔离的模块不在其中。
    QStringList runningModuleIds() const;
    /// @brief 列出因停止超时被隔离保留的模块 ID，这些模块的 DLL 仍然驻留。
    /// @return 隔离模块 ID 的快照列表；正常关闭后为空。
    QStringList quarantinedModuleIds() const;
    /// @brief 查询模块的界面对象视图，供主窗口询问草稿状态或嵌入 Dock。
    /// @param moduleId 模块唯一标识，等于该插件 DLL 的文件名。
    /// @return 插件实例的 InProcessUiModule 借用指针，所有权仍在 QPluginLoader；非 UI、未知或加载失败的模块返回 nullptr。
    InProcessUiModule* uiModule(const QString& moduleId) const;

signals:
    /// @brief 模块状态发生变化（加载完成、启动成功、失败、被隔离）时发射，供日志和界面展示。
    /// @param moduleId 发生状态变化的模块唯一标识。
    /// @param state 状态文本，取值为 Loaded、Running、Failed、Quarantined。
    /// @param detail 失败或隔离的原因说明；Loaded 和 Running 时为空字符串。
    /// @return 无（信号不返回值）。
    void moduleStateChanged(const QString& moduleId,
                            const QString& state,
                            const QString& detail);

private:
    // LoadedPlugin 是单个插件的所有权记录，定义在 PluginManager.cpp 中，对外不可见。
    struct LoadedPlugin;

    /// @brief 加载并注册单个插件 DLL，依次校验文件存在、文件名等于模块 ID、插件元数据和基类类型。
    /// @param config 该模块的配置；filePath 指向 DLL，id 必须等于 DLL 文件名（忽略大小写）。
    /// @param errorMessage 失败原因输出参数，不能为空；失败时写入可读的中文说明。
    /// @return 校验、实例化和总线注册都成功返回 true；任一步失败返回 false，且不留下 loaded_ 记录。
    bool loadOne(const ModuleConfig& config, QString* errorMessage);
    /// @brief 调用插件的 onStart 并捕获异常；返回 false 或抛异常时撤销该模块的发布权限。
    /// @param plugin 已加载的插件记录，必须来自 loaded_。
    /// @param errorMessage 失败原因输出参数，不能为空；异常或返回 false 时写入说明。
    /// @return onStart 返回 true 且未抛异常返回 true；否则返回 false，且模块保持未启动状态。
    bool startOne(LoadedPlugin* plugin, QString* errorMessage);
    /// @brief 回收启动失败的插件：补做一次 onStop、注销总线、从 loaded_ 移除并卸载 DLL，不影响其他模块。
    /// @param plugin 启动失败且仍在 loaded_ 中的插件记录。
    /// @return 无；onStop 抛出的异常被吞掉并记入日志。
    void removeFailed(LoadedPlugin* plugin);
    /// @brief 停止并卸载单个插件；前置条件是它的消息队列已经停止，否则会与回调并发执行 onStop。
    /// @param plugin 待回收的插件记录，允许为空。
    /// @return 已回收（含 plugin 为空）返回 true；总线注销失败导致对象被保留时返回 false。
    bool releasePlugin(LoadedPlugin* plugin);

    // loaded_ 中的记录和 QPluginLoader 由本类拥有；endpoint/instance 只是视图。
    MessageBus* messageBus_; ///< 中央消息总线借用指针，所有权在 FrameworkRuntime，本类不负责释放。
    QVector<LoadedPlugin*> loaded_; ///< 已加载插件记录，含被隔离的项；记录及其 QPluginLoader 都由本类释放。
    bool shutdownComplete_; ///< 是否已执行过 shutdown；为 true 后 shutdown 只返回隔离名单，析构也不再兜底回收。
};
}
