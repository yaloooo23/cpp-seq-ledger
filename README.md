# seq_ledger — Asynchronous Request Lifecycle Tracker

[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![CMake](https://img.shields.io/badge/CMake-%3E%3D3.10-blue)](https://cmake.org/)

A lightweight, zero-dependency C++17 library for tracking async request/response lifecycles by `seqId`.

**What it does:**
- Allocates unique `seqId` for each outgoing request
- Tracks inflight requests and their timeout deadlines
- Fires a single callback when a bill closes (response received, timeout, cancellation, eviction)
- Auto-poll background thread for timeout scanning
- `GlobalCallback` for centralized logging/monitoring

**What it does NOT do:**
- No JSON parsing or serialization
- No network/transport layer
- No UI or threading framework (pure STL)

## Quick Start

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

See `examples/basic_usage.cpp` for a runnable demo.

## Building

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

### CMake Options

| Option                      | Default | Description                        |
| --------------------------- | ------- | ---------------------------------- |
| `SEQ_LEDGER_BUILD_SHARED`   | ON      | Build as shared library (.so/.dll) |
| `SEQ_LEDGER_BUILD_TESTS`    | OFF     | Build unit tests                   |
| `SEQ_LEDGER_BUILD_EXAMPLES` | OFF     | Build example programs             |

### Integrating via CMake

```cmake
# After installing or adding as subdirectory:
find_package(seq_ledger REQUIRED)
target_link_libraries(your_target PRIVATE seq_ledger::seq_ledger)
```

Or use `add_subdirectory`:

```cmake
add_subdirectory(path/to/cpp-seq-ledger)
target_link_libraries(your_target PRIVATE seq_ledger)
```

## Design

### No external dependencies

`seq_ledger` only uses C++17 STL + `<pthread>`. It does not depend on Qt, Boost, nlohmann/json, or any networking library. You can drop it into any C++ project regardless of your existing stack.

### State machine

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

Every bill transitions through exactly one terminal state. The `BillCallback` fires **once** — when the bill closes — and never again.

### Thread safety model: lock-inside, callback-outside

All state mutation happens under `std::mutex`. Callbacks are collected into a local vector during the locked phase and executed after the lock is released. This prevents:
- Deadlocks from re-entering the ledger inside a callback
- Holding the lock during user-provided callback execution

### Auto-poll

An optional background thread scans inflight bills every 100ms (configurable) and closes timed-out bills. Uses its own mutex/condition_variable, separate from the ledger lock, to avoid deadlocks.

```cpp
ledger.setAutoPollIntervalMs(200);  // scan every 200ms
ledger.setAutoPollIntervalMs(0);    // disable background thread
```

You can also poll manually:

```cpp
int timedOut = ledger.pollTimeouts();  // returns count of timed-out bills
```

### Callback execution

By default, callbacks run on the calling thread (the thread that calls `completeByResponse` / `pollTimeouts` / `cancel`). This means **timeout callbacks may execute on the background scanner thread**.

To control which thread runs your callbacks, pass a `CallbackExecutor` to `book()`:

```cpp
ledger.book(
    input,
    myCallback,
    [](CallbackTask task) {
        // Post task to your own thread/event loop
        myEventLoop.post(std::move(task));
    });
```

When an executor is provided, both the per-bill `BillCallback` and the global `GlobalCallback` are merged into a single task for that bill.

## API Reference

### Types

| Type             | Description                                              |
| ---------------- | -------------------------------------------------------- |
| `BookInput`      | Input to `book()`: `comment` + optional `timeoutMs`      |
| `Receipt`        | Returned by `book()`: contains `Status` + `Ticket`       |
| `Ticket`         | The allocated `seqId`, echoed `comment`, and `timeoutMs` |
| `BillSnapshot`   | Immutable snapshot of a bill's state (used in callbacks) |
| `BillCallback`   | Per-bill callback: `std::function<void(const BillSnapshot&)>` |
| `CallbackTask`   | Callable task for executor: `std::function<void()>`      |
| `CallbackExecutor` | Task dispatcher: `std::function<void(CallbackTask)>`  |
| `GlobalCallback` | Fires on every bill closure, for logging/statistics      |

### Status enum

| Value             | Meaning                   |
| ----------------- | ------------------------- |
| `Ok`              | Operation succeeded       |
| `SeqIdNotFound`   | `seqId` not inflight      |
| `InvalidArgument` | Invalid parameter         |
| `DuplicateSeqId`  | Reserved, not yet used    |

### BillState enum

| State              | Trigger                              |
| ------------------ | ------------------------------------ |
| `PendingSend`      | Initial state after `book()`         |
| `Sent`             | After `markSent()`                   |
| `Completed`        | `completeByResponse()`               |
| `TimedOut`         | Auto-poll or `pollTimeouts()`        |
| `Evicted`          | Capacity exceeded, oldest evicted    |
| `Cancelled`        | `cancel()` or `cancelAll()`          |
| `TransportFailed`  | `markTransportFailed()`              |

### SeqLedger class

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

### BillSnapshot fields (by terminal state)

| Terminal          | `state`           | `code`          | `errMsg`                         |
| ----------------- | ----------------- | --------------- | -------------------------------- |
| Response          | `Completed`       | response `code` | response `errMsg`                |
| Timeout           | `TimedOut`        | `-1`            | `"request timed out"`            |
| Evicted           | `Evicted`         | `0`             | `"evicted by max inflight limit"` |
| Cancelled         | `Cancelled`       | `0`             | `cancel()` reason                |
| Transport failure | `TransportFailed` | `-1`            | `markTransportFailed()` reason   |

## Typical Integration Pattern

`seq_ledger` is designed to be wrapped by a "request manager" layer that owns the ledger and bridges it to your transport:

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

The ledger itself never touches the wire — it only tracks lifecycle, enforces timeouts, and dispatches callbacks.

## FAQ

**Q: Why not use `std::future` / `std::promise`?**
A: Futures require blocking (`get()`) or polling (`wait_for()`). `seq_ledger` gives you callback-based notification with timeout management, capacity eviction, and a single-close guarantee — all without threads parked on futures.

**Q: How does it handle late responses (response arrives after timeout)?**
A: `completeByResponse()` returns `SeqIdNotFound`. The callback will NOT fire a second time. This is guaranteed by erasing the bill from the inflight map before executing callbacks.

**Q: Can I reuse seqId after a bill closes?**
A: No. Each request gets a new, monotonically increasing seqId. If you need to retry, use a fresh `book()` call.

**Q: How many inflight bills can it handle?**
A: Default cap is 10. Call `setMaxInflight(n)` to increase. When full, the oldest bill is evicted (with `BillState::Evicted` callback) rather than rejecting the new `book()`.

## License

MIT — see [LICENSE](LICENSE) for full text.
