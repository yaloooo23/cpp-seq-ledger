/**
 * basic_usage.cpp -- Minimal example demonstrating the full request lifecycle
 *
 * Compile: cmake -DSEQ_LEDGER_BUILD_EXAMPLES=ON .. && make example_basic
 * Run:     ./example_basic
 */
#include "seq_ledger/seq_ledger.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace seq_ledger;

int main()
{
    SeqLedger ledger;

    // Optional: turn off auto-poll for manual control
    ledger.setAutoPollIntervalMs(0);

    // Step 1: book a request -- registers a seqId with timeout and completion callback
    bool received = false;
    Receipt receipt = ledger.book(
        BookInput{"example request", 3000},
        [&received](const BillSnapshot &snap) {
            if (snap.state == BillState::Completed) {
                std::cout << "Request seqId=" << snap.seqId
                          << " completed, code=" << snap.code
                          << ", elapsed=" << snap.elapsedMs << "ms\n";
                received = true;
            } else {
                std::cout << "Request seqId=" << snap.seqId
                          << " terminated: state=" << static_cast<int>(snap.state)
                          << ", errMsg=" << snap.errMsg << "\n";
            }
        });

    uint32_t seqId = receipt.ticket.seqId;
    std::cout << "Booked seqId=" << seqId
              << " comment=\"" << receipt.ticket.comment << "\"\n";

    // Step 2: mark as sent (do this after your actual send call succeeds)
    assert(ledger.markSent(seqId) == Status::Ok);

    // Step 3: when the response arrives, close the bill
    assert(ledger.completeByResponse(seqId, 0, "success") == Status::Ok);

    // Callback fired synchronously here
    assert(received);

    // Verify the bill is now in recentClosed, not inflight
    BillSnapshot snap = ledger.get(seqId);
    assert(snap.found && snap.state == BillState::Completed);

    std::cout << "\nBasic example passed.\n";
    return 0;
}
