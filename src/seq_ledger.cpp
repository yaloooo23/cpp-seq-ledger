#include "seq_ledger/seq_ledger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <thread>
#include <utility>

namespace seq_ledger {

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kDefaultMaxInflight = 10;
constexpr uint32_t kDefaultRecentClosedMax = 50;
constexpr uint32_t kDefaultTimeoutMs = 3000;
constexpr uint32_t kDefaultAutoPollIntervalMs = 100;

// 内置超时扫描：独立控制锁，与账本 mutex 分离，避免与 pollTimeouts 死锁
class AutoPollController {
public:
    explicit AutoPollController(SeqLedger *owner) // 绑定所属账本
        : m_owner(owner)
    {
    }

    ~AutoPollController() // 停止并回收扫描线程
    {
        stopAndJoin();
    }

    AutoPollController(const AutoPollController &) = delete;
    AutoPollController &operator=(const AutoPollController &) = delete;

    void setIntervalMs(uint32_t intervalMs) // 设置扫描间隔，0 表示关闭
    {
        std::unique_lock<std::mutex> lock(m_controlMutex);
        if (intervalMs == 0) {
            m_intervalMs.store(0, std::memory_order_release);
            signalStopLocked(lock);
            return;
        }

        m_intervalMs.store(intervalMs, std::memory_order_release);
        if (!m_thread.joinable()) {
            m_stop.store(false, std::memory_order_release);
            m_thread = std::thread([this]() { pollLoop(); });
            return;
        }

        m_cv.notify_one();
    }

    uint32_t intervalMs() const // 当前扫描间隔 ms
    {
        return m_intervalMs.load(std::memory_order_acquire);
    }

    bool enabled() const // 扫描线程是否在运行
    {
        std::lock_guard<std::mutex> lock(m_controlMutex);
        return m_thread.joinable() && !m_stop.load(std::memory_order_acquire);
    }

private:
    void signalStopLocked(std::unique_lock<std::mutex> &lock) // 持锁发停止信号并 join
    {
        m_stop.store(true, std::memory_order_release);
        m_cv.notify_all();
        if (!m_thread.joinable()) {
            return;
        }

        std::thread worker = std::move(m_thread);
        lock.unlock();
        worker.join();
    }

    void stopAndJoin() // 析构时停止扫描线程
    {
        std::unique_lock<std::mutex> lock(m_controlMutex);
        signalStopLocked(lock);
    }

    void pollLoop() // 后台循环：等待间隔后调用 pollTimeouts
    {
        while (!m_stop.load(std::memory_order_acquire)) {
            {
                std::unique_lock<std::mutex> lock(m_controlMutex);
                const uint32_t intervalMs = m_intervalMs.load(std::memory_order_acquire);
                if (intervalMs == 0 || m_stop.load(std::memory_order_acquire)) {
                    break;
                }

                m_cv.wait_for(lock,
                              std::chrono::milliseconds(intervalMs),
                              [this]() {
                                  return m_stop.load(std::memory_order_acquire);
                              });
            }

            if (m_stop.load(std::memory_order_acquire)) {
                break;
            }

            if (m_owner != nullptr) {
                m_owner->pollTimeouts();
            }
        }
    }

    SeqLedger *m_owner = nullptr;              // 所属账本
    mutable std::mutex m_controlMutex;         // 扫描线程启停/间隔
    std::condition_variable m_cv;              // 间隔变更或退出唤醒
    std::thread m_thread;                      // 扫描线程
    std::atomic<uint32_t> m_intervalMs{0};     // 扫描间隔 ms，0=关闭
    std::atomic<bool> m_stop{true};            // 退出标记
};

// 单笔在途账单内部记录
struct BillRecord {
    uint32_t seqId = 0;                              // 业务 seqId
    BillState state = BillState::PendingSend;  // 当前状态
    std::string comment;                             // 账单备注
    uint32_t timeoutMs = kDefaultTimeoutMs;          // 超时阈值 ms
    int code = 0;                                    // response.code 或本地错误码
    std::string errMsg;                              // 错误说明
    Clock::time_point createdAt;                     // 开账时刻
    Clock::time_point closedAt;                      // 闭账时刻
    Clock::time_point deadline;                      // 超时截止时刻
    BillCallback callback;                           // 单笔闭账回调
    CallbackExecutor executor;                       // 单笔回调执行器
};

// 锁外待投递的回调事件
struct CallbackEvent {
    BillCallback callback;           // 单笔闭账回调
    CallbackExecutor executor;       // 单笔回调执行器
    GlobalCallback globalCallback;   // 全局闭账回调
    BillSnapshot snapshot;           // 闭账快照
};

// 计算两点间耗时，结果钳制到 uint32_t
uint32_t elapsedMs(Clock::time_point from, Clock::time_point to)
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
    if (ms <= 0) {
        return 0;
    }
    if (ms > static_cast<long long>(std::numeric_limits<uint32_t>::max())) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(ms);
}

// 由内部记录生成对外快照
BillSnapshot makeSnapshot(const BillRecord &record, Clock::time_point now)
{
    BillSnapshot snapshot;
    snapshot.found = true;
    snapshot.seqId = record.seqId;
    snapshot.state = record.state;
    snapshot.comment = record.comment;
    snapshot.timeoutMs = record.timeoutMs;
    snapshot.elapsedMs = elapsedMs(record.createdAt, now);
    snapshot.code = record.code;
    snapshot.errMsg = record.errMsg;
    return snapshot;
}

// 在锁外投递闭账回调；有 executor 时单笔与全局回调同任务、同线程执行
void runCallbackEvents(const std::vector<CallbackEvent> &events)
{
    for (const CallbackEvent &event : events) {
        const bool hasBill = static_cast<bool>(event.callback);
        const bool hasGlobal = static_cast<bool>(event.globalCallback);
        if (!hasBill && !hasGlobal) {
            continue;
        }

        CallbackTask task = [callback = event.callback,
                             globalCallback = event.globalCallback,
                             snapshot = event.snapshot]() {
            if (callback) {   // 执行单笔闭账回调
                callback(snapshot);
            }
            if (globalCallback) {  // 执行全局闭账回调
                globalCallback(snapshot);
            }
        };  // 组lambda表达式给闭账回调任务

        if (event.executor) {
            event.executor(std::move(task));  // 通过executor投递闭账回调任务
        } else {
            task();  // 直接执行闭账回调任务
        }
    }
}

} // namespace

// SeqLedger 私有实现：账本状态与在途/历史数据
struct SeqLedger::Impl {
    mutable std::mutex mutex;                              // 保护下方账本数据
    uint32_t nextSeqId = 1;                                // 下一个待分配 seqId
    uint32_t maxInflight = kDefaultMaxInflight;            // 最大在途笔数
    uint32_t recentClosedMax = kDefaultRecentClosedMax;    // 最近闭账保留数
    GlobalCallback globalCallback;                           // 全局闭账事件回调
    std::unordered_map<uint32_t, BillRecord> inflight;  // 在途账单表
    std::deque<uint32_t> order;                            // 开账顺序，用于踢最老
    std::deque<BillSnapshot> recentClosed;              // 最近闭账快照
    AutoPollController autoPoll;                           // 内置超时扫描，最后析构

    explicit Impl(SeqLedger *owner) // 构造并绑定超时扫描
        : autoPoll(owner)
    {
    }

    uint32_t allocateSeqIdLocked(); // 分配不冲突的 seqId（调用方已持锁）
    Ticket makeTicketLocked(const BillRecord &record) const; // 生成入账小票
    void pushRecentClosedLocked(const BillSnapshot &snapshot); // 写入最近闭账队列
    void closeRecordLocked(std::unordered_map<uint32_t, BillRecord>::iterator it,
                           BillState state,
                           int code,
                           const std::string &errMsg,
                           Clock::time_point now,
                           std::vector<CallbackEvent> *events); // 闭账并收集回调事件
    void evictOldestLocked(Clock::time_point now, std::vector<CallbackEvent> *events); // 踢最老在途账
};

uint32_t SeqLedger::Impl::allocateSeqIdLocked()
{
    // 跳过 0 与已在途占用的 seqId
    while (nextSeqId == 0 || inflight.find(nextSeqId) != inflight.end()) {
        ++nextSeqId;
        if (nextSeqId == 0) {
            nextSeqId = 1;
        }
    }

    const uint32_t allocated = nextSeqId;
    ++nextSeqId;
    if (nextSeqId == 0) {
        nextSeqId = 1;
    }
    return allocated;
}

Ticket SeqLedger::Impl::makeTicketLocked(const BillRecord &record) const
{
    Ticket ticket;
    ticket.seqId = record.seqId;
    ticket.comment = record.comment;
    ticket.timeoutMs = record.timeoutMs;
    return ticket;
}

void SeqLedger::Impl::pushRecentClosedLocked(const BillSnapshot &snapshot)
{
    if (recentClosedMax == 0) {
        return;
    }

    recentClosed.push_back(snapshot);
    while (recentClosed.size() > recentClosedMax) {
        recentClosed.pop_front();
    }
}

void SeqLedger::Impl::closeRecordLocked(std::unordered_map<uint32_t, BillRecord>::iterator it,
                                        BillState state,
                                        int code,
                                        const std::string &errMsg,
                                        Clock::time_point now,
                                        std::vector<CallbackEvent> *events)
{
    // 从在途表移除，更新终态并归档到 recentClosed
    BillRecord record = std::move(it->second);
    inflight.erase(it);
    order.erase(std::remove(order.begin(), order.end(), record.seqId), order.end());

    record.state = state;
    record.code = code;
    record.errMsg = errMsg;
    record.closedAt = now;

    const BillSnapshot snapshot = makeSnapshot(record, now);
    pushRecentClosedLocked(snapshot);

    if (events) {
        events->push_back(CallbackEvent{record.callback,
                                        record.executor,
                                        globalCallback,
                                        snapshot});
    }
}

void SeqLedger::Impl::evictOldestLocked(Clock::time_point now, std::vector<CallbackEvent> *events)
{
    // 在途满时按开账顺序踢最老一笔
    while (inflight.size() >= maxInflight && !order.empty()) {
        const uint32_t oldestSeqId = order.front();
        auto it = inflight.find(oldestSeqId);
        if (it == inflight.end()) {
            order.pop_front();
            continue;
        }

        closeRecordLocked(it,
                          BillState::Evicted,
                          0,
                          std::string("evicted by max inflight limit"),
                          now,
                          events);
        break;
    }
}

SeqLedger::SeqLedger()
    : m_impl(new Impl(this))
{
    m_impl->autoPoll.setIntervalMs(kDefaultAutoPollIntervalMs);
}

SeqLedger::~SeqLedger() = default;

Receipt SeqLedger::book(BookInput input, BillCallback callback, CallbackExecutor executor)
{
    std::vector<CallbackEvent> events;
    Receipt receipt;

    // Mutate all ledger state under lock, then notify evicted records after unlock.
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const Clock::time_point now = Clock::now();

        if (input.timeoutMs == 0) {
            input.timeoutMs = kDefaultTimeoutMs;
        }
        if (m_impl->maxInflight == 0) {
            m_impl->maxInflight = 1;
        }

        m_impl->evictOldestLocked(now, &events);

        BillRecord record;
        record.seqId = m_impl->allocateSeqIdLocked();
        record.state = BillState::PendingSend;
        record.comment = std::move(input.comment);
        record.timeoutMs = input.timeoutMs;
        record.createdAt = now;
        record.closedAt = now;
        record.deadline = now + std::chrono::milliseconds(record.timeoutMs);
        record.callback = std::move(callback);
        record.executor = std::move(executor);

        receipt.status = Status::Ok;
        receipt.ticket = m_impl->makeTicketLocked(record);

        m_impl->order.push_back(record.seqId);
        m_impl->inflight.emplace(record.seqId, std::move(record));
    }

    runCallbackEvents(events);
    return receipt;
}

Status SeqLedger::markSent(uint32_t seqId)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    auto it = m_impl->inflight.find(seqId);
    if (it == m_impl->inflight.end()) {
        return Status::SeqIdNotFound;
    }

    it->second.state = BillState::Sent;
    return Status::Ok;
}

Status SeqLedger::completeByResponse(uint32_t seqId, int code, std::string errMsg)
{
    std::vector<CallbackEvent> events;
    Status status = Status::Ok;

    // Copy callback/snapshot under lock, run user code after unlock.
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        auto it = m_impl->inflight.find(seqId);
        if (it == m_impl->inflight.end()) {
            status = Status::SeqIdNotFound;
        } else {
            m_impl->closeRecordLocked(it,
                                      BillState::Completed,
                                      code,
                                      errMsg,
                                      Clock::now(),
                                      &events);
        }
    }

    runCallbackEvents(events);
    return status;
}

Status SeqLedger::markTransportFailed(uint32_t seqId, std::string reason)
{
    std::vector<CallbackEvent> events;
    Status status = Status::Ok;

    // Close the bill deterministically so callers do not keep waiting.
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        auto it = m_impl->inflight.find(seqId);
        if (it == m_impl->inflight.end()) {
            status = Status::SeqIdNotFound;
        } else {
            m_impl->closeRecordLocked(it,
                                      BillState::TransportFailed,
                                      -1,
                                      reason,
                                      Clock::now(),
                                      &events);
        }
    }

    runCallbackEvents(events);
    return status;
}

Status SeqLedger::cancel(uint32_t seqId, std::string reason)
{
    std::vector<CallbackEvent> events;
    Status status = Status::Ok;

    // Cancel 是终态，通知调用方
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        auto it = m_impl->inflight.find(seqId);
        if (it == m_impl->inflight.end()) {
            status = Status::SeqIdNotFound;
        } else {
            m_impl->closeRecordLocked(it,
                                      BillState::Cancelled,
                                      0,
                                      reason,
                                      Clock::now(),
                                      &events);
        }
    }

    runCallbackEvents(events);
    return status;
}

int SeqLedger::pollTimeouts()
{
    std::vector<CallbackEvent> events;

    // 先收集超时账单 ID，避免迭代时 mutate
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const Clock::time_point now = Clock::now();
        std::vector<uint32_t> expired;

        for (const auto &entry : m_impl->inflight) {
            if (now >= entry.second.deadline) {
                expired.push_back(entry.first);
            }
        }

        for (uint32_t seqId : expired) {
            auto it = m_impl->inflight.find(seqId);
            if (it == m_impl->inflight.end()) {
                continue;
            }
            m_impl->closeRecordLocked(it,
                                      BillState::TimedOut,
                                      -1,
                                      std::string("bill timed out"),
                                      now,
                                      &events);
        }
    }

    runCallbackEvents(events);
    return static_cast<int>(events.size());
}

void SeqLedger::cancelAll(std::string reason)
{
    std::vector<CallbackEvent> events;

    // Cancel each inflight record once, preserving callback notifications.
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        const Clock::time_point now = Clock::now();
        std::vector<uint32_t> ids(m_impl->order.begin(), m_impl->order.end());

        for (uint32_t seqId : ids) {
            auto it = m_impl->inflight.find(seqId);
            if (it == m_impl->inflight.end()) {
                continue;
            }
            m_impl->closeRecordLocked(it,
                                      BillState::Cancelled,
                                      0,
                                      reason,
                                      now,
                                      &events);
        }
    }

    runCallbackEvents(events);
}

BillSnapshot SeqLedger::get(uint32_t seqId) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const Clock::time_point now = Clock::now();

    auto it = m_impl->inflight.find(seqId);
    if (it != m_impl->inflight.end()) {
        return makeSnapshot(it->second, now);
    }

    for (auto rit = m_impl->recentClosed.rbegin(); rit != m_impl->recentClosed.rend(); ++rit) {
        if (rit->seqId == seqId) {
            return *rit;
        }
    }

    BillSnapshot missing;
    missing.seqId = seqId;
    return missing;
}

std::vector<BillSnapshot> SeqLedger::listInflight() const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    const Clock::time_point now = Clock::now();
    std::vector<BillSnapshot> snapshots;
    snapshots.reserve(m_impl->inflight.size());

    for (uint32_t seqId : m_impl->order) {
        auto it = m_impl->inflight.find(seqId);
        if (it != m_impl->inflight.end()) {
            snapshots.push_back(makeSnapshot(it->second, now));
        }
    }

    return snapshots;
}

std::vector<BillSnapshot> SeqLedger::recentClosed(int limit) const
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (limit <= 0) {
        limit = static_cast<int>(m_impl->recentClosed.size());
    }

    std::vector<BillSnapshot> snapshots;
    const int count = std::min(limit, static_cast<int>(m_impl->recentClosed.size()));
    snapshots.reserve(static_cast<size_t>(count));

    auto rit = m_impl->recentClosed.rbegin();
    for (int i = 0; i < count && rit != m_impl->recentClosed.rend(); ++i, ++rit) {
        snapshots.push_back(*rit);
    }

    return snapshots;
}

void SeqLedger::setGlobalCallback(GlobalCallback callback)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->globalCallback = std::move(callback);
}

void SeqLedger::setMaxInflight(uint32_t max)
{
    std::vector<CallbackEvent> events;

    // Shrink immediately if the new cap is below current inflight count.
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        m_impl->maxInflight = max == 0 ? 1 : max;
        const Clock::time_point now = Clock::now();
        while (m_impl->inflight.size() > m_impl->maxInflight) {
            m_impl->evictOldestLocked(now, &events);
        }
    }

    runCallbackEvents(events);
}

void SeqLedger::setRecentClosedMax(uint32_t max)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->recentClosedMax = max;
    while (m_impl->recentClosed.size() > m_impl->recentClosedMax) {
        m_impl->recentClosed.pop_front();
    }
}

void SeqLedger::setAutoPollIntervalMs(uint32_t intervalMs)
{
    m_impl->autoPoll.setIntervalMs(intervalMs);
}

uint32_t SeqLedger::autoPollIntervalMs() const
{
    return m_impl->autoPoll.intervalMs();
}

bool SeqLedger::isAutoPollEnabled() const
{
    return m_impl->autoPoll.enabled();
}

} // namespace seq_ledger
