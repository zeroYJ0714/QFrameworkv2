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
    explicit PluginManager(MessageBus* messageBus, QObject* parent = nullptr);
    ~PluginManager() override;

    // 只筛选 InProcessUi/InProcessNonUi；单个模块失败会写入 errors，
    // 其他模块仍继续加载。返回值表示是否全部成功。
    bool loadAndStart(const QVector<ModuleConfig>& modules,
                      QStringList* errors = nullptr,
                      bool enableDeliveryAfterStart = true);
    // 队列已停止的插件才调用 onStop/卸载；超时 ID 保留在 quarantine。
    QStringList shutdown(const QStringList& timedOutModuleIds = QStringList());
    // 测试替代 fail-fast 或协作回调稍后返回时，可再次回收已经安全停止的隔离项。
    QStringList retryQuarantinedShutdown();

    // 查询结果是快照；uiModule 返回借用指针，所有权仍在 QPluginLoader。
    QStringList runningModuleIds() const;
    QStringList quarantinedModuleIds() const;
    InProcessUiModule* uiModule(const QString& moduleId) const;

signals:
    void moduleStateChanged(const QString& moduleId,
                            const QString& state,
                            const QString& detail);

private:
    struct LoadedPlugin;

    bool loadOne(const ModuleConfig& config, QString* errorMessage);
    bool startOne(LoadedPlugin* plugin, QString* errorMessage);
    void removeFailed(LoadedPlugin* plugin);
    bool releasePlugin(LoadedPlugin* plugin);

    // loaded_ 中的记录和 QPluginLoader 由本类拥有；endpoint/instance 只是视图。
    MessageBus* messageBus_;
    QVector<LoadedPlugin*> loaded_;
    bool shutdownComplete_;
};
}
