#pragma once
// 两个 GUI 模块分别拥有此值状态，仅 GUI 线程调用。共享规则，绝不共享控制权对象。
// 已 RELEASE 的 UUID 保留到模块停止，防止 Reliable 队列中的迟到 ACQUIRE 重新加锁。
#include <QSet>
#include <QString>
#include <QRegularExpression>
#include "Generated/usecase_message.pb.h"

class ExecutionControlState final
{
public:
    /// @brief 查询当前是否已有执行持有界面操作锁。
    /// @return 存在持锁 UUID 时返回 true，空闲时返回 false。
    bool locked() const { return !owner_.isEmpty(); }
    /// @brief 清空持锁者与已释放集合，用于模块停止时重置本状态。
    /// @return 无。
    void clear() { owner_.clear(); released_.clear(); }
    /// @brief 处理一次 ACQUIRE/RELEASE 请求，按当前持锁情况返回成功或忙。
    /// @param request 含 request_id、execution_id 和动作的请求。
    /// @param busy 调用方已知界面正在执行其他操作，用于拒绝新的加锁。
    /// @return 已回填关联字段与 success 或 error_code 的响应。
    qframework::protocols::ExecutionControlResponse handle(
        const qframework::protocols::ExecutionControlRequest& request, bool busy)
    {
        using namespace qframework::protocols;
        ExecutionControlResponse response; ///< 本次要回填的响应，复用请求中的关联字段。
        response.set_request_id(request.request_id());
        response.set_execution_id(request.execution_id());
        response.set_action(request.action());
        const QString id = QString::fromStdString(request.execution_id()); ///< 请求携带的 execution UUID，作为持锁身份键。
        const bool valid = !request.request_id().empty()
            && QRegularExpression("^[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}$").match(id).hasMatch(); ///< 校验 request_id 非空且 execution_id 是标准 UUID 文本。
        if (!valid || (request.action() != EXECUTION_CONTROL_ACTION_ACQUIRE && request.action() != EXECUTION_CONTROL_ACTION_RELEASE)) {
            response.set_error_code("INVALID_PARAMETER");
        } else if (request.action() == EXECUTION_CONTROL_ACTION_RELEASE) {
            released_.insert(id);
            if (owner_ == id) owner_.clear();
            response.set_success(true);
        } else if (released_.contains(id) || (locked() && owner_ != id) || (owner_ != id && busy)) {
            response.set_error_code("EXECUTION_BUSY");
        } else {
            owner_ = id;
            response.set_success(true);
        }
        return response;
    }
private:
    QString owner_; ///< 当前持锁的 execution UUID，与 request_id 无关。
    QSet<QString> released_; ///< 生命周期内已 RELEASE 的 UUID 精确集合，不淘汰、不用概率过滤器。
};
