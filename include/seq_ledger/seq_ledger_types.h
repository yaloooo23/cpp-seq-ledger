/**
 * seq_ledger_types.h — seq_ledger 公共类型
 *
 * 功能：定义账本状态、返回码、开账参数、快照与回调类型。
 */
#ifndef SEQ_LEDGER_TYPES_H
#define SEQ_LEDGER_TYPES_H

#include <cstdint>
#include <functional>
#include <string>

namespace seq_ledger {

enum class Status : int {   // 操作状态
    Ok = 0,             // 操作成功
    InvalidArgument,    // 参数无效
    SeqIdNotFound,      // seqId 不存在
    DuplicateSeqId      // seqId 冲突
};

enum class BillState : int {  // 账单状态
    PendingSend = 0,    // 已入账，尚未标记发送
    Sent,               // 已调用发送接口
    Completed,          // 收到 response 闭账
    TimedOut,           // 超时闭账
    Evicted,            // 容量超限被踢出
    Cancelled,          // 主动取消
    TransportFailed     // 明确传输失败
};

struct BookInput {     // 入账参数
    std::string comment;          // 账单备注
    uint32_t    timeoutMs = 3000; // 单笔设置的超时时长，默认 3000ms
};

struct Ticket {       // 小票
    uint32_t    seqId = 0;        // 分配的业务 seqId
    std::string comment;          // 回显备注
    uint32_t    timeoutMs = 3000; // 单笔设置的超时时长
};

struct Receipt {        // 收据
    Status status = Status::Ok; // 入账是否成功
    Ticket ticket;              // 回执小票
};

struct BillSnapshot { // 查询快照
    bool         found = false;                       // 是否查到该账单
    uint32_t     seqId = 0;                         // 业务 seqId
    BillState state = BillState::PendingSend; // 当前状态
    std::string  comment;                           // 账单备注
    uint32_t     timeoutMs = 3000;                  // 实际超时
    uint32_t     elapsedMs = 0;                     // 已耗时
    int          code = 0;                          // response.code 或本地错误码
    std::string  errMsg;                            // 错误说明
};

using BillCallback = std::function<void(const BillSnapshot &)>; // 单笔闭账回调
using CallbackTask = std::function<void()>; // 待投递的回调任务，没有 CallbackTask 也能写成 std::function<void()>，但起个名字更清楚。
using CallbackExecutor = std::function<void(CallbackTask)>; // 单笔回调执行器
using GlobalCallback = std::function<void(const BillSnapshot &)>; // 全局闭账回调

} // namespace seq_ledger

#endif // SEQ_LEDGER_TYPES_H
