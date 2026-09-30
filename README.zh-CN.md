# seq_ledger — 异步请求生命周期跟踪器

[English](README.md) | [简体中文](README.zh-CN.md)

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![CMake](https://img.shields.io/badge/CMake-%3E%3D3.10-blue)](https://cmake.org/)

轻量、零依赖的 C++17 库，按 `seqId` 跟踪异步 request/response 生命周期。

**它做什么：**
- 为每笔发出的 request 分配唯一 `seqId`
- 跟踪在途请求及其超时截止时间
- 账单闭账时触发一次回调（收到 response、超时、取消、踢出）
- 后台自动扫描线程处理超时
- `GlobalCallback` 用于集中日志/监控

**它不做什么：**
- 不解析、不序列化 JSON
- 不包含网络/传输层
- 不绑定 UI 或线程框架（纯 STL）

## 快速上手

```cpp
#include "seq_ledger/seq_ledger.h"
using namespace seq_ledger;

SeqLedger ledger;

// 1. Book a request
auto receipt = ledger.book(
    BookInput{"my request", 3000},
    [](const BillSnapshot &snap) {
        if (snap.state == BillState::Completed) {
            // Response arrived successfully
        }
    });

// 2. Send your payload (your own code), then mark sent
ledger.markSent(receipt.ticket.seqId);

// 3. When response arrives, close the bill
ledger.completeByResponse(receipt.ticket.seqId, 0, "ok");
```

可运行示例见 `examples/basic_usage.cpp`。

## 构建

```bash
# Clone
git clone https://github.com/yaloooo23/cpp-seq-ledger.git
cd cpp-seq-ledger

# Configure and build
mkdir build && cd build
cmake .. -DSEQ_LEDGER_BUILD_TESTS=ON
cmake --build . -- -j$(nproc)

# Run tests
ctest --output-on-failure
```

### CMake 选项

| Option                      | Default | Description                        |
| --------------------------- | ------- | ---------------------------------- |
| `SEQ_LEDGER_BUILD_SHARED`   | ON      | 编译为共享库 (.so/.dll)            |
| `SEQ_LEDGER_BUILD_TESTS`    | OFF     | 编译单元测试                       |
| `SEQ_LEDGER_BUILD_EXAMPLES` | OFF     | 编译示例程序                       |

### 通过 CMake 集成

```cmake
# After installing or adding as subdirectory:
find_package(seq_ledger REQUIRED)
target_link_libraries(your_target PRIVATE seq_ledger::seq_ledger)
```

或使用 `add_subdirectory`：

```cmake
add_subdirectory(path/to/cpp-seq-ledger)
target_link_libraries(your_target PRIVATE seq_ledger)
```

## 设计

### 无外部依赖

`seq_ledger` 只用 C++17 STL + `<pthread>`。不依赖 Qt、Boost、nlohmann/json 或任何网络库。可以放进任意 C++ 工程，与现有技术栈无关。

### 状态机

```
PendingSend  ── markSent() ──► Sent
     │                              │
     └───────── terminal ───────────┘
              Completed       (response received)
              TimedOut        (deadline exceeded)
              Evicted         (pushed out by newer bills)
              Cancelled       (explicit cancel)
              TransportFailed (send failure detected)
```

每笔账单只进入一个终态。`BillCallback` 在闭账时触发 **一次**，之后不会再触发。

### 线程安全模型：锁内改状态，锁外跑回调

所有状态变更都在 `std::mutex` 下完成。回调在持锁阶段收集到局部 vector，解锁后再执行。这样可以避免：
- 回调里再进入账本导致死锁
- 持锁执行用户回调

### 自动扫描

可选后台线程每隔 100ms（可配置）扫描在途账单并关闭超时项。使用独立的 mutex/condition_variable，与账本锁分离，避免死锁。

```cpp
ledger.setAutoPollIntervalMs(200);  // scan every 200ms
ledger.setAutoPollIntervalMs(0);    // disable background thread
```

也可以手动扫描：

```cpp
int timedOut = ledger.pollTimeouts();  // returns count of timed-out bills
```

### 回调执行

默认情况下，回调跑在调用方线程（调用 `completeByResponse` / `pollTimeouts` / `cancel` 的线程）。因此 **超时回调可能在后台扫描线程上执行**。

要指定回调跑在哪条线程，向 `book()` 传入 `CallbackExecutor`：

```cpp
ledger.book(
    input,
    myCallback,
    [](CallbackTask task) {
        // Post task to your own thread/event loop
        myEventLoop.post(std::move(task));
    });
```

提供 executor 后，该笔的 `BillCallback` 与全局 `GlobalCallback` 会合并为同一个任务。

## API 参考

### 类型

| Type             | Description                                              |
| ---------------- | -------------------------------------------------------- |
| `BookInput`      | `book()` 入参：`comment` + 可选 `timeoutMs`              |
| `Receipt`        | `book()` 返回：包含 `Status` + `Ticket`                  |
| `Ticket`         | 分配的 `seqId`、回显的 `comment` 和 `timeoutMs`          |
| `BillSnapshot`   | 账单状态的不可变快照（用于回调）                         |
| `BillCallback`   | 单笔回调：`std::function<void(const BillSnapshot&)>`     |
| `CallbackTask`   | 交给 executor 的可调用任务：`std::function<void()>`      |
| `CallbackExecutor` | 任务分发器：`std::function<void(CallbackTask)>`        |
| `GlobalCallback` | 每笔账单闭账时触发，用于日志/统计                        |

### Status 枚举

| Value             | Meaning                   |
| ----------------- | ------------------------- |
| `Ok`              | 操作成功                  |
| `SeqIdNotFound`   | `seqId` 不在在途表中      |
| `InvalidArgument` | 参数无效                  |
| `DuplicateSeqId`  | 保留，尚未使用            |

### BillState 枚举

| State              | Trigger                              |
| ------------------ | ------------------------------------ |
| `PendingSend`      | `book()` 之后的初始状态              |
| `Sent`             | `markSent()` 之后                    |
| `Completed`        | `completeByResponse()`               |
| `TimedOut`         | 自动扫描或 `pollTimeouts()`          |
| `Evicted`          | 容量超限，最老一笔被踢出             |
| `Cancelled`        | `cancel()` 或 `cancelAll()`          |
| `TransportFailed`  | `markTransportFailed()`              |

### SeqLedger 类

```cpp
class SeqLedger {
public:
    // Lifecycle
    Receipt book(BookInput input,
                 BillCallback callback = {},
                 CallbackExecutor executor = {});
    Status markSent(uint32_t seqId);
    Status completeByResponse(uint32_t seqId, int code,
                              std::string errMsg = {});
    Status markTransportFailed(uint32_t seqId,
                               std::string reason = {});
    Status cancel(uint32_t seqId, std::string reason = {});
    void cancelAll(std::string reason = {});
    int  pollTimeouts();

    // Auto-poll control
    void setAutoPollIntervalMs(uint32_t intervalMs);
    uint32_t autoPollIntervalMs() const;
    bool isAutoPollEnabled() const;

    // Query
    BillSnapshot get(uint32_t seqId) const;
    std::vector<BillSnapshot> listInflight() const;
    std::vector<BillSnapshot> recentClosed(int limit) const;

    // Configuration
    void setGlobalCallback(GlobalCallback callback);
    void setMaxInflight(uint32_t max);      // default 10
    void setRecentClosedMax(uint32_t max);  // default 50
};
```

### 各终态下的 BillSnapshot 字段

| Terminal          | `state`           | `code`          | `errMsg`                         |
| ----------------- | ----------------- | --------------- | -------------------------------- |
| Response          | `Completed`       | response `code` | response `errMsg`                |
| Timeout           | `TimedOut`        | `-1`            | `"request timed out"`            |
| Evicted           | `Evicted`         | `0`             | `"evicted by max inflight limit"` |
| Cancelled         | `Cancelled`       | `0`             | `cancel()` reason                |
| Transport failure | `TransportFailed` | `-1`            | `markTransportFailed()` reason   |

## 典型集成方式

`seq_ledger` 适合被一层 “request manager” 包装：由该层持有账本，并接到你的传输上：

```
YourApp / Business Logic
  └── RequestManager (your code)
        ├── SeqLedger (this library)
        │     ├── book() / markSent() / completeByResponse()
        │     ├── pollTimeouts() or auto-poll
        │     └── setGlobalCallback() for logging
        │
        └── YourTransport (IPC, TCP, HTTP, etc.)
              ├── send(payload)
              └── onResponse(payload) → completeByResponse()
```

账本本身不碰线路 —— 只跟踪生命周期、执行超时，并分发回调。

## FAQ

**Q: 为什么不用 `std::future` / `std::promise`？**
A: Future 需要阻塞（`get()`）或轮询（`wait_for()`）。`seq_ledger` 提供基于回调的通知，并带超时管理、容量踢出和单次闭账保证 —— 不必把线程停在 future 上。

**Q: 超时之后才收到 response 怎么办？**
A: `completeByResponse()` 返回 `SeqIdNotFound`。回调不会第二次触发。保证方式是：先从在途表删除账单，再执行回调。

**Q: 账单关闭后能否复用 seqId？**
A: 不能。每笔请求分配新的、单调递增的 seqId。若需重试，重新调用 `book()`。

**Q: 最多能处理多少笔在途账单？**
A: 默认上限是 10。调用 `setMaxInflight(n)` 提高上限。满时踢出最老一笔（触发 `BillState::Evicted` 回调），而不是拒绝新的 `book()`。

## 许可

MIT — 全文见 [LICENSE](LICENSE)。
