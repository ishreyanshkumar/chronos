// ─────────────────────────────────────────────────────────────────────────────
// Chronos :: DPDKReceiver
//
// Hardware-level kernel bypass ingestion for UDP multicast market data.
// Reads raw ethernet frames directly from the NIC DMA buffer.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "order_book.hpp"
#include "itch_parser.hpp" // For ITCH payload structures
#include <cstdint>
#include <string>

// DPDK includes are protected by CMake configuration.
// If not built with DPDK, this class will act as a mock or simply fail to compile
// if attempted to be used directly without the USE_DPDK flag.
#ifdef USE_DPDK
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#endif

namespace chronos {

class DPDKReceiver {
public:
    // Initializes the DPDK Environment Abstraction Layer (EAL) and configures the NIC port.
    DPDKReceiver(int argc, char** argv, uint16_t port_id, OrderBook& book);
    ~DPDKReceiver();

    // Starts the aggressive zero-allocation polling loop on the pinned core.
    void Run();

    // Cleanly signals the polling loop to exit.
    void Stop() { stop_ = true; }

private:
#ifdef USE_DPDK
    void ProcessPacket(struct rte_mbuf* mbuf);
#endif

    uint16_t port_id_;
    OrderBook& book_;
    volatile bool stop_{false};
};

} // namespace chronos
