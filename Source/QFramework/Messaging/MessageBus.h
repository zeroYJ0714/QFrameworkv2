#pragma once

// 文件职责：定义中央消息总线的公开管理接口。
// MessageBus 不直接调用订阅者，而是为每个注册模块创建独立 ModuleQueue，
// 这样一个慢模块只影响自己的容量和统计。

#include <QHash>
#include <QMutex>
#include <QStringList>

#include "FrameworkConfig.h"
#include "ModuleEndpoint.h"
#include "QFrameworkGlobal.h"

namespace qframework
{
struct ModuleQueueStats
{
    // delivered 是已调用 onMessage 的数量；dropped 是 Latest/停止丢弃数量；
    // rejected 是 Reliable 满、停止或非法请求拒绝数量。
    quint64 delivered = 0;
    quint64 dropped = 0;
    quint64 rejected = 0;
};

class QFRAMEWORK_EXPORT MessageBus : public ModuleHost
{
public:
    // config 按值保存，保证总线运行期间配置快照稳定。
    explicit MessageBus(const MessageBusConfig& config);
    ~MessageBus() override;

    // 注册主题声明并启动模块专属队列；重复 ID 或空主题会失败。
    bool registerModule(const QString& moduleId,
                        ModuleEndpoint* endpoint,
                        QString* errorMessage = nullptr);
    // 设置 publish 是否可用，不等于是否已经收到消息。
    bool setModuleRunning(const QString& moduleId, bool running);
    // 启动阶段先暂停队列，所有模块注册完成后一次性开启投递。
    void setDeliveryEnabled(bool enabled);
    // 关闭发布入口并唤醒所有暂停队列。
    void beginShutdown();
    // 在给定预算内停止所有模块队列，返回是否全部排空。
    bool stopQueues(int drainTimeoutMs);
    // 移除一个模块；drainPendingMessages 决定停止前是否保留待处理消息。
    bool unregisterModule(const QString& moduleId, bool drainPendingMessages);

    // 以下查询只返回快照，不把内部锁或队列所有权交给调用方。
    QStringList moduleIds() const;
    ModuleQueueStats queueStats(const QString& moduleId) const;

    // ModuleEndpoint::publish 的宿主实现：校验发布权限和大小后，
    // 把同一消息复制到所有订阅者各自的有界队列。
    bool publishFromModule(const QString& moduleId,
                           const QString& topic,
                           const QByteArray& data) override;
    // ModuleEndpoint 的日志宿主实现，转交单例 Logger。
    void logFromModule(LogLevel level,
                       const QString& moduleId,
                       const QString& text) override;

private:
    struct Registration;

    TopicConfig topicConfig(const QString& topic) const;
    void logRejected(const QString& moduleId, const QString& reason) const;

    // mutex_ 保护模块注册表和 accepting/deliveryEnabled 状态。
    MessageBusConfig config_;
    mutable QMutex mutex_;
    QHash<QString, Registration*> modules_;
    bool accepting_;
    bool deliveryEnabled_;
};
}
