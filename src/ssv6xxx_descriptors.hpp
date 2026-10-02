/*
 * SSV6X5X HW Descriptors + IEEE 802.11 Frame Helpers (C++14)
 * Derived from the Linux kernel driver (ssv6200_common.h)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <cstdint>
#include <cstring>

/* ===================== FW Rate Report ===================== */
struct FwRcRetryParams {
    uint32_t count : 4;
    uint32_t drate : 6;
    uint32_t crate : 6;
    uint32_t rts_cts_nav : 16;
    uint32_t frame_consume_time : 10;
    uint32_t dl_length : 12;
    uint32_t rsvd : 10;
} __attribute__((packed));

/* ===================== TX Descriptor =====================
 * Total size: 80 bytes (TXPB_OFFSET)
 * Prepended to every 802.11 frame sent to the chip
 */
struct Ssv6200TxDesc {
    /* DW0 */
    uint32_t len : 16;
    uint32_t c_type : 3;
    uint32_t f80211 : 1;
    uint32_t qos : 1;
    uint32_t ht : 1;
    uint32_t use_4addr : 1;
    uint32_t rsvd_0 : 3;
    uint32_t bc_que : 1;
    uint32_t security : 1;
    uint32_t more_data : 1;
    uint32_t stype_b5b4 : 2;
    uint32_t extra_info : 1;
    /* DW1 */
    uint32_t fCmd;
    /* DW2 */
    uint32_t hdr_offset : 8;
    uint32_t frag : 1;
    uint32_t unicast : 1;
    uint32_t hdr_len : 6;
    uint32_t tx_report : 1;
    uint32_t tx_burst : 1;
    uint32_t ack_policy : 2;
    uint32_t aggregation : 1;
    uint32_t rsvd_1 : 3;
    uint32_t do_rts_cts : 2;
    uint32_t reason : 6;
    /* DW3 */
    uint32_t payload_offset : 8;
    uint32_t rsvd_4 : 7;
    uint32_t rsvd_2 : 1;
    uint32_t fCmdIdx : 3;
    uint32_t wsid : 4;
    uint32_t txq_idx : 3;
    uint32_t TxF_ID : 6;
    /* DW4 */
    uint32_t rts_cts_nav : 16;
    uint32_t frame_consume_time : 10;
    uint32_t crate_idx : 6;
    /* DW5 */
    uint32_t drate_idx : 6;
    uint32_t dl_length : 12;
    uint32_t rsvd_3 : 14;
    /* Reserved + Rate params */
    uint32_t reserved[8];
    FwRcRetryParams rc_params[3];
} __attribute__((packed));

constexpr int SSV62XX_TX_MAX_RATES = 3;

/* TX descriptor c_type values */
constexpr uint8_t TX_CTYPE_DATA = 0;
constexpr uint8_t TX_CTYPE_MGMT = 5;

/* ===================== RX Descriptor ===================== */
struct Ssv6200RxDesc {
    /* WORD 1 */
    uint32_t len : 16;
    uint32_t c_type : 3;
    uint32_t f80211 : 1;
    uint32_t qos : 1;
    uint32_t ht : 1;
    uint32_t use_4addr : 1;
    uint32_t rsvdrx0_1 : 1;
    uint32_t running_no : 4;
    uint32_t psm : 1;
    uint32_t stype_b5b4 : 2;
    uint32_t decrypted : 1;

    /* WORD 2 */
    union {
        uint32_t fCmd;
        struct {
            uint32_t edca0_used : 4;
            uint32_t edca1_used : 5;
            uint32_t edca2_used : 5;
            uint32_t edca3_used : 5;
            uint32_t mng_used : 4;
            uint32_t tx_page_used : 9;
        };
    };

    /* WORD 3 */
    uint32_t hdr_offset : 8;
    uint32_t frag : 1;
    uint32_t unicast : 1;
    uint32_t hdr_len : 6;
    uint32_t RxResult : 8;
    uint32_t bssid : 2;
    uint32_t reason : 6;

    /* WORD 4 */
    uint32_t payload_offset : 8;
    uint32_t rx_pkt_run_no : 8;
    uint32_t fCmdIdx : 3;
    uint32_t wsid : 4;
    uint32_t tkip_mmic_err : 1;
    uint32_t channel : 8;

    /* WORD 5 */
    uint32_t phy_packet_length : 16;
    uint32_t phy_rate : 8;
    uint32_t phy_smoothing : 1;
    uint32_t phy_no_sounding : 1;
    uint32_t phy_aggregation : 1;
    uint32_t phy_stbc : 2;
    uint32_t phy_fec : 1;
    uint32_t phy_n_ess : 2;

    /* WORD 6 */
    uint32_t phy_l_length : 12;
    uint32_t phy_l_rate : 3;
    uint32_t phy_mrx_seqn : 1;
    uint32_t phy_rssi : 8;
    uint32_t phy_snr : 8;

    /* WORD 7 */
    uint32_t phy_rx_freq_offset : 16;
    uint32_t phy_service : 6;
    uint32_t rsvd_7 : 10;

    /* WORD 8-20 */
    uint32_t rx_timestamp_1;
    uint32_t rx_timestamp_2;
    uint32_t rx_len : 16;
    uint32_t dummy10 : 16;
    uint32_t dummy11;
    uint32_t dummy12;
    uint32_t dummy13;
    uint32_t dummy14;
    uint32_t dummy15;
    uint32_t dummy16;
    uint32_t dummy17;
    uint32_t rx_pn_0 : 8;
    uint32_t rx_pn_1 : 8;
    uint32_t rx_pn_2 : 8;
    uint32_t rx_pn_3 : 8;
    uint32_t rx_pn_4 : 8;
    uint32_t rx_pn_5 : 8;
    uint32_t key_id : 2;
    uint32_t rsvd_19 : 14;
    uint32_t seqnum : 16;
    uint32_t dummy20 : 16;
} __attribute__((packed));

/* HCI RX aggregation header */
struct HciRxAggrInfo {
    uint32_t jmp_mpdu_len : 16;
    uint32_t accu_rx_len : 16;
    uint32_t rsvd0 : 15;
    uint32_t tx_page_remain : 9;
    uint32_t tx_id_remain : 8;
    uint32_t edca0 : 4;
    uint32_t edca1 : 5;
    uint32_t edca2 : 5;
    uint32_t edca3 : 5;
    uint32_t edca4 : 4;
    uint32_t edca5 : 5;
    uint32_t rsvd1 : 4;
} __attribute__((packed));

/* ===================== TX Descriptor Builder ===================== */
inline void ssv6xxx_build_tx_desc(Ssv6200TxDesc &desc, uint32_t frame_len,
                                  uint32_t hdr_offset, uint32_t hdr_len,
                                  uint32_t payload_offset, uint32_t txq_idx,
                                  uint32_t wsid)
{
    std::memset(&desc, 0, sizeof(desc));
    desc.len            = frame_len;
    desc.c_type         = TX_CTYPE_DATA;
    desc.f80211         = 1;
    desc.hdr_offset     = hdr_offset;
    desc.unicast        = 1;
    desc.hdr_len        = hdr_len;
    desc.payload_offset = payload_offset;
    desc.wsid           = wsid;
    desc.txq_idx        = txq_idx;
    desc.reason         = 0;
    desc.crate_idx      = 0;
    desc.drate_idx      = 0;
}

/* ===================== MAC Address Helper ===================== */
#define MAC_FMT        "%02x:%02x:%02x:%02x:%02x:%02x"
#define MAC_ARG(mac)   (mac)[0], (mac)[1], (mac)[2], (mac)[3], (mac)[4], (mac)[5]
// spdlog/fmt-compatible format using {} style
#define MAC_FMT_SPDLOG "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}"
