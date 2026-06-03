// ─────────────────────────────────────────────────────────────────────────────
// Chronos :: DPDKReceiver Implementation
// ─────────────────────────────────────────────────────────────────────────────
#include "dpdk_receiver.hpp"
#include <stdexcept>
#include <iostream>

#ifdef USE_DPDK
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_mbuf.h>

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32
#endif

namespace chronos {

DPDKReceiver::DPDKReceiver(int argc, char** argv, uint16_t port_id, OrderBook& book)
    : port_id_(port_id), book_(book) {
#ifdef USE_DPDK
    // Initialize the Environment Abstraction Layer (EAL)
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        throw std::runtime_error("Error with EAL initialization");
    }

    uint16_t nb_ports = rte_eth_dev_count_avail();
    if (port_id_ >= nb_ports) {
        throw std::runtime_error("Invalid DPDK port ID");
    }

    // Allocate memory pool for the NIC DMA buffers
    struct rte_mempool* mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
        MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

    if (mbuf_pool == NULL) {
        throw std::runtime_error("Cannot create mbuf pool");
    }

    // Configure the ethernet device
    struct rte_eth_conf port_conf = {};
    if (rte_eth_dev_configure(port_id_, 1, 1, &port_conf) < 0) {
        throw std::runtime_error("Cannot configure ethernet device");
    }

    if (rte_eth_rx_queue_setup(port_id_, 0, 1024, rte_eth_dev_socket_id(port_id_), NULL, mbuf_pool) < 0) {
        throw std::runtime_error("Cannot setup RX queue");
    }

    if (rte_eth_dev_start(port_id_) < 0) {
        throw std::runtime_error("Cannot start ethernet device");
    }
#else
    (void)argc; (void)argv;
    throw std::runtime_error("Chronos was not built with DPDK support (USE_DPDK=OFF).");
#endif
}

DPDKReceiver::~DPDKReceiver() {
#ifdef USE_DPDK
    rte_eth_dev_stop(port_id_);
    rte_eth_dev_close(port_id_);
    rte_eal_cleanup();
#endif
}

void DPDKReceiver::Run() {
#ifdef USE_DPDK
    struct rte_mbuf* bufs[BURST_SIZE];

    // Aggressive lock-free polling loop (Kernel Bypass)
    while (!stop_) {
        // Read directly from the NIC hardware DMA buffer. No context switches.
        const uint16_t nb_rx = rte_eth_rx_burst(port_id_, 0, bufs, BURST_SIZE);

        if (unlikely(nb_rx == 0)) {
            continue;
        }

        for (uint16_t i = 0; i < nb_rx; i++) {
            ProcessPacket(bufs[i]);
            // Free the buffer back to the NIC pool immediately
            rte_pktmbuf_free(bufs[i]);
        }
    }
#else
    throw std::runtime_error("Chronos was not built with DPDK support.");
#endif
}

#ifdef USE_DPDK
void DPDKReceiver::ProcessPacket(struct rte_mbuf* mbuf) {
    // 1. Strip Ethernet Header (14 bytes)
    struct rte_ether_hdr* eth_hdr = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr*);
    if (rte_be_to_cpu_16(eth_hdr->ether_type) != RTE_ETHER_TYPE_IPV4) return;

    // 2. Strip IPv4 Header (20 bytes)
    struct rte_ipv4_hdr* ipv4_hdr = (struct rte_ipv4_hdr*)(eth_hdr + 1);
    if (ipv4_hdr->next_proto_id != IPPROTO_UDP) return;

    // 3. Strip UDP Header (8 bytes)
    struct rte_udp_hdr* udp_hdr = (struct rte_udp_hdr*)((unsigned char*)ipv4_hdr + sizeof(struct rte_ipv4_hdr));
    
    // 4. Extract ITCH 5.0 Payload
    uint8_t* payload = (uint8_t*)(udp_hdr + 1);
    uint16_t payload_len = rte_be_to_cpu_16(udp_hdr->dgram_len) - sizeof(struct rte_udp_hdr);

    // Normally ITCH payload contains a sequence number and message count header (SoupBinTCP/MoldUDP64)
    // For pure hardware speed demonstration, we assume raw ITCH messages.
    ITCHCallbacks cbs;
    cbs.on_add = [&](const MsgAdd& m) {
        book_.AddOrder(m.order_ref, m.side, OrderType::Limit, m.price, m.shares, m.ts_ns);
    };
    cbs.on_executed = [&](const MsgExecuted& m) {
        // Match logic...
    };
    cbs.on_cancelled = [&](const MsgCancelled& m) {
        book_.CancelOrder(m.order_ref);
    };
    cbs.on_deleted = [&](const MsgDeleted& m) {
        book_.CancelOrder(m.order_ref);
    };

    std::size_t pos = 0;
    while (pos + 2 <= payload_len) {
        uint16_t msg_len = (uint16_t(payload[pos]) << 8) | payload[pos + 1];
        pos += 2;
        if (pos + msg_len > payload_len) break;
        
        ITCHParser::ParseMessage(payload + pos, msg_len, cbs);
        pos += msg_len;
    }
}
#endif

} // namespace chronos
