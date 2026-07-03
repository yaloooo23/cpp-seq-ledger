/**
 * seq_ledger.h — 发起方 seqId 请求账本
 *
 * 功能：登记 request、按 seqId 闭账、超时扫描、状态查询；不依赖 Qt/JSON/传输层。
 */
#ifndef SEQ_LEDGER_H
#define SEQ_LEDGER_H

#include "seq_ledger_export.h"
#include "seq_ledger_types.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace seq_ledger {

class SEQ_LEDGER_API SeqLedger
{
public:
    SeqLedger(); // 默认构造
    ~SeqLedger(); // 默认析构

    SeqLedger(const SeqLedger &) = delete; // 禁止拷贝
    SeqLedger &operator=(const SeqLedger &) = delete; // 禁止赋值

    Receipt book(BookInput input,
                 BillCallback callback = {},
                 CallbackExecutor executor = {}); // 入账，返回回执

    Status markSent(uint32_t seqId); // 标记已发送
    Status completeByResponse(uint32_t seqId,
                              int code,
                              std::string errMsg = {}); // response 闭账，执行入账时的回调函数
    Status markTransportFailed(uint32_t seqId,
                               std::string reason = {}); // 标记传输失败
    Status cancel(uint32_t seqId,
                  std::string reason = {}); // 主动取消
    int pollTimeouts(); // 扫描超时账单（可与内置扫描并存）
    void cancelAll(std::string reason = {}); // 取消所有在途账单

    void setAutoPollIntervalMs(uint32_t intervalMs); // 内置超时扫描间隔，0=关闭
    uint32_t autoPollIntervalMs() const;             // 当前扫描间隔 ms
    bool isAutoPollEnabled() const;                    // 是否已启动扫描线程

    BillSnapshot get(uint32_t seqId) const; // 查询单笔快照
    std::vector<BillSnapshot> listInflight() const; // 查询在途账单
    std::vector<BillSnapshot> recentClosed(int limit) const; // 查询最近闭账

    void setGlobalCallback(GlobalCallback callback); // 设置全局闭账回调
    void setMaxInflight(uint32_t max); // 设置最大在途数
    void setRecentClosedMax(uint32_t max); // 设置最近闭账保留数

private:
    struct Impl; // 私有实现

    std::unique_ptr<Impl> m_impl; // 实现指针
};

} // namespace seq_ledger

#endif // SEQ_LEDGER_H
