#include "seq_ledger/seq_ledger.h"
#include <cassert>
#include <chrono>
#include <thread>
#include <utility>
#include <vector>

using namespace seq_ledger;

namespace {

// 关闭内置超时扫描线程，避免与手动 pollTimeouts 抢时序（单测要可控）
void disableAutoPoll(SeqLedger &ledger)
{
    ledger.setAutoPollIntervalMs(0);
}

// 用例 1：正常收到 response 的成功闭账路径
void testComplete()
{
    SeqLedger ledger;                    // 每个用例独立账本，互不影响
    disableAutoPoll(ledger);             // 本用例手动驱动，不用后台扫描
    std::vector<BillSnapshot> callbacks; // 收集 BillCallback 收到的快照

    // 入账：登记 seqId、超时、闭账时要执行的回调（此时回调还不会跑）
    Receipt ret = ledger.book({"normal request", 3000},
                              [&callbacks](const BillSnapshot &snapshot) {
                                  callbacks.push_back(snapshot);
                              });
    assert(ret.status == Status::Ok);              // book 当前实现恒为 Ok
    assert(ret.ticket.seqId == 1);                 // 第一笔 seqId 从 1 开始
    assert(ledger.markSent(ret.ticket.seqId) == Status::Ok); // 标记已发送 → Sent
    // 模拟收到对端 response，闭账为 Completed
    assert(ledger.completeByResponse(ret.ticket.seqId, 0, {}) == Status::Ok);
    assert(callbacks.size() == 1);                 // 闭账后回调应执行一次
    assert(callbacks[0].state == BillState::Completed);
    assert(callbacks[0].code == 0);                // response code 原样带回
    assert(ledger.listInflight().empty());         // 已闭账，在途应为空
    assert(ledger.recentClosed(10).size() == 1);   // 归档进最近闭账列表
}

// 用例 2：超时闭账，且晚到 response 不再生效
void testTimeout()
{
    SeqLedger ledger;
    disableAutoPoll(ledger);
    std::vector<BillSnapshot> callbacks;

    // 超时设为 1ms，便于快速测超时
    Receipt ret = ledger.book({"timeout request", 1},
                              [&callbacks](const BillSnapshot &snapshot) {
                                  callbacks.push_back(snapshot);
                              });
    assert(ret.status == Status::Ok);
    assert(ledger.markSent(ret.ticket.seqId) == Status::Ok);
    std::this_thread::sleep_for(std::chrono::milliseconds(3)); // 超过 1ms 阈值
    assert(ledger.pollTimeouts() == 1);          // 手动扫一次，应闭 1 笔超时账
    assert(callbacks.size() == 1);
    assert(callbacks[0].state == BillState::TimedOut);
    // 超时已闭账，再 completeByResponse 应找不到在途 seqId
    assert(ledger.completeByResponse(ret.ticket.seqId, 0, {}) == Status::SeqIdNotFound);
}

// 用例 3：在途满时踢最老一笔（Evicted）
void testEvictOldest()
{
    SeqLedger ledger;
    disableAutoPoll(ledger);
    ledger.setMaxInflight(2);            // 最多 2 笔在途
    std::vector<BillSnapshot> callbacks;

    Receipt a = ledger.book({"first", 3000},
                            [&callbacks](const BillSnapshot &snapshot) {
                                callbacks.push_back(snapshot);
                            });
    Receipt b = ledger.book({"second", 3000},
                            [&callbacks](const BillSnapshot &snapshot) {
                                callbacks.push_back(snapshot);
                            });
    // 第 3 笔入账时，最老的 a 应被踢出
    Receipt c = ledger.book({"third", 3000},
                            [&callbacks](const BillSnapshot &snapshot) {
                                callbacks.push_back(snapshot);
                            });

    assert(a.status == Status::Ok);
    assert(b.status == Status::Ok);
    assert(c.status == Status::Ok);
    assert(callbacks.size() == 1);               // 只有被踢的 a 会立刻回调
    assert(callbacks[0].seqId == a.ticket.seqId);
    assert(callbacks[0].state == BillState::Evicted);
    assert(ledger.get(a.ticket.seqId).state == BillState::Evicted); // 历史可查
    assert(ledger.listInflight().size() == 2);   // b、c 仍在途
}

// 用例 4：主动取消 + 发送失败两种终态
void testCancelAndTransportFailed()
{
    SeqLedger ledger;
    disableAutoPoll(ledger);
    std::vector<BillSnapshot> callbacks;

    Receipt cancelRet = ledger.book({"cancel", 3000},
                                    [&callbacks](const BillSnapshot &snapshot) {
                                        callbacks.push_back(snapshot);
                                    });
    Receipt failRet = ledger.book({"transport fail", 3000},
                                  [&callbacks](const BillSnapshot &snapshot) {
                                      callbacks.push_back(snapshot);
                                  });

    assert(ledger.cancel(cancelRet.ticket.seqId, "user closed") == Status::Ok);
    assert(ledger.markTransportFailed(failRet.ticket.seqId, "send failed") == Status::Ok);
    assert(callbacks.size() == 2);               // 两笔各闭账一次
    assert(callbacks[0].state == BillState::Cancelled);
    assert(callbacks[1].state == BillState::TransportFailed);
}

// 用例 5：全局闭账回调 GlobalCallback
void testGlobalCallback()
{
    SeqLedger ledger;
    disableAutoPoll(ledger);
    int eventCount = 0;

    // 全局回调：每一笔闭账都会触发（与单笔 BillCallback 独立）
    ledger.setGlobalCallback([&eventCount](const BillSnapshot &snapshot) {
        assert(snapshot.found);
        ++eventCount;
    });

    Receipt ret = ledger.book({"event", 3000});  // 本笔未传 BillCallback
    assert(ledger.completeByResponse(ret.ticket.seqId, 0, {}) == Status::Ok);
    assert(eventCount == 1);                   // 仅 GlobalCallback 被调一次
}

// 用例 6：CallbackExecutor 延迟执行单笔回调
void testCallbackExecutor()
{
    SeqLedger ledger;
    disableAutoPoll(ledger);
    std::vector<CallbackTask> tasks; // 模拟外部线程队列，先存任务再执行
    int callbackCount = 0;

    Receipt ret = ledger.book({"executor", 3000},
                              [&callbackCount](const BillSnapshot &snapshot) {
                                  assert(snapshot.state == BillState::Completed);
                                  ++callbackCount;
                              },
                              [&tasks](CallbackTask task) {
                                  tasks.push_back(std::move(task)); // 不立刻跑，只投递
                              });

    assert(ledger.completeByResponse(ret.ticket.seqId, 0, {}) == Status::Ok);
    assert(callbackCount == 0);                  // complete 返回时回调尚未执行
    assert(tasks.size() == 1);                   // executor 收到 1 个任务

    tasks[0]();                                  // 手动“投递到目标线程”
    assert(callbackCount == 1);                  // 此时 BillCallback 才执行
}

// 用例 7：有 executor 时 BillCallback 与 GlobalCallback 打进同一任务
void testGlobalCallbackWithExecutor()
{
    SeqLedger ledger;
    disableAutoPoll(ledger);
    std::vector<CallbackTask> tasks;
    int billCount = 0;
    int globalCount = 0;

    ledger.setGlobalCallback([&globalCount](const BillSnapshot &snapshot) {
        assert(snapshot.state == BillState::Completed);
        ++globalCount;
    });

    Receipt ret = ledger.book({"global executor", 3000},
                              [&billCount](const BillSnapshot &snapshot) {
                                  assert(snapshot.state == BillState::Completed);
                                  ++billCount;
                              },
                              [&tasks](CallbackTask task) {
                                  tasks.push_back(std::move(task));
                              });

    assert(ledger.completeByResponse(ret.ticket.seqId, 0, {}) == Status::Ok);
    assert(billCount == 0);                      // 闭账瞬间都还没跑
    assert(globalCount == 0);
    assert(tasks.size() == 1);                   // 单笔+全局合并为 1 个 CallbackTask

    tasks[0]();                                  // 先 bill，后 global，同线程顺序执行
    assert(billCount == 1);
    assert(globalCount == 1);
}

// 用例 8：内置 AutoPoll 后台线程自动超时（不关手动扫描）
void testAutoPoll()
{
    SeqLedger ledger;
    ledger.setAutoPollIntervalMs(20);            // 每 20ms 扫一次超时
    assert(ledger.isAutoPollEnabled());

    std::vector<BillSnapshot> callbacks;
    Receipt ret = ledger.book({"auto poll", 1},  // 1ms 超时
                              [&callbacks](const BillSnapshot &snapshot) {
                                  callbacks.push_back(snapshot);
                              });
    assert(ret.status == Status::Ok);
    assert(ledger.markSent(ret.ticket.seqId) == Status::Ok);

    std::this_thread::sleep_for(std::chrono::milliseconds(80)); // 等扫描线程闭账
    assert(callbacks.size() == 1);
    assert(callbacks[0].state == BillState::TimedOut);

    ledger.setAutoPollIntervalMs(0);             // 关闭扫描线程
    assert(!ledger.isAutoPollEnabled());
}

} // namespace

int main()
{
    testComplete();                  // 成功 response 闭账
    testTimeout();                   // 超时 + 晚到 response
    testEvictOldest();               // 容量踢最老
    testCancelAndTransportFailed();  // 取消 / 传输失败
    testGlobalCallback();            // 全局回调
    testCallbackExecutor();          // executor 投递单笔回调
    testGlobalCallbackWithExecutor();// executor 同时投递单笔+全局
    testAutoPoll();                  // 内置扫描线程超时
    return 0;                        // 全部 assert 通过则 exit 0
}
