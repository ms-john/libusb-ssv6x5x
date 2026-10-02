/*
 * ssv_dual_link_test.cpp - SSV6x5X 双卡空口通讯测试
 *
 * 单向：卡A 监控接收，卡B 注入。
 * 双向：两卡同时监控 + 注入，按方向魔数过滤，避免听到自己发出的帧。
 *
 * 用法示例：
 *   ./ssv_dual_link_test --list-devices
 *   ./ssv_dual_link_test -c 6 -n 50
 *   ./ssv_dual_link_test --bidir -c 6 -n 40
 *   ./ssv_dual_link_test --bidir --channels 1,6,11 -n 30
 *   ./ssv_dual_link_test -c 6 --tx-port 1.4 --rx-port 1.3
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ssv6xxx_regs.hpp"
#include "ssv6xxx_types.hpp"
#include "ssv6xxx_usb_transport.hpp"
#include <ssv6xxx/ssv6xxx.hpp>

#include <CLI/CLI.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace
{

    /* 方向魔数：A 发给 B / B 发给 A，各自独立过滤，避免回环误计。 */
    constexpr char kMagicAtoB[8] = {'L', 'I', 'M', 'O', 'A', '2', 'B', '!'};
    constexpr char kMagicBtoA[8] = {'L', 'I', 'M', 'O', 'B', '2', 'A', '!'};
    constexpr size_t kMagicLen   = sizeof(kMagicAtoB);
    constexpr int kMinInjectLen  = 10;
    /* 与驱动 MAX_FRAME_SIZE 一致（802.11 注入上限）。 */
    constexpr int kMaxInjectLen = MAX_FRAME_SIZE;

    struct RxMatchState {
        std::atomic<uint64_t> total_rx{0};
        std::atomic<uint64_t> matched{0};
        std::atomic<uint64_t> last_seq{0};
        std::atomic<int8_t> last_rssi{0};
        std::atomic<int> last_len{0};
        std::atomic<uint32_t> last_freq{0};
        const char *expect_magic{nullptr};
        std::atomic<bool> verbose{false};
    };

    struct ChannelResult {
        int channel{0};
        int power_index{-1};
        bool ok{false};
        int a_to_b_injected{0};
        int a_to_b_matched{0};
        int b_to_a_injected{0};
        int b_to_a_matched{0};
        uint64_t a_rx_total{0};
        uint64_t b_rx_total{0};
        int8_t a_last_rssi{0};
        int8_t b_last_rssi{0};
        std::string note;
    };

    bool parse_mac(const std::string &text, uint8_t out[6])
    {
        unsigned m[6];
        if (std::sscanf(text.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x", &m[0], &m[1],
                        &m[2], &m[3], &m[4], &m[5]) != 6) {
            return false;
        }
        for (int i = 0; i < 6; ++i) {
            out[i] = static_cast<uint8_t>(m[i]);
        }
        return true;
    }

    bool parse_int_list(const std::string &text, int min_v, int max_v,
                        std::vector<int> *out)
    {
        std::stringstream ss(text);
        std::string item;
        out->clear();
        while (std::getline(ss, item, ',')) {
            if (item.empty()) {
                continue;
            }
            const int v = std::atoi(item.c_str());
            if (v < min_v || v > max_v) {
                return false;
            }
            out->push_back(v);
        }
        return !out->empty();
    }

    int build_probe_frame(uint8_t *buf, size_t buf_size, const uint8_t src_mac[6],
                          const char magic[8], uint32_t seq, int payload_len)
    {
        const int header_len = 24;
        const int llc_len    = 8;
        const int fixed_tail = static_cast<int>(kMagicLen) + 4;
        int pad              = payload_len;
        if (pad < 16) {
            pad = 16;
        }
        const int min_total = header_len + llc_len + fixed_tail + 16;
        int total           = header_len + llc_len + fixed_tail + pad;
        if (total < min_total) {
            total = min_total;
        }
        if (total > static_cast<int>(buf_size) || total > kMaxInjectLen) {
            return -1;
        }
        pad = total - header_len - llc_len - fixed_tail;
        if (total > static_cast<int>(buf_size) || total > kMaxInjectLen) {
            return -1;
        }

        std::memset(buf, 0, static_cast<size_t>(total));
        buf[0] = 0x08; /* Data */
        buf[1] = 0x00;
        for (int i = 0; i < 6; ++i) {
            buf[4 + i] = 0xFF; /* broadcast */
        }
        std::memcpy(buf + 10, src_mac, 6);
        std::memcpy(buf + 16, src_mac, 6);
        const uint16_t sc = static_cast<uint16_t>((seq & 0x0FFF) << 4);
        buf[22]           = static_cast<uint8_t>(sc & 0xFF);
        buf[23]           = static_cast<uint8_t>((sc >> 8) & 0xFF);

        buf[24] = 0xAA;
        buf[25] = 0xAA;
        buf[26] = 0x03;
        buf[27] = 0x00;
        buf[28] = 0x00;
        buf[29] = 0x00;
        buf[30] = 0x88;
        buf[31] = 0xB5;

        std::memcpy(buf + 32, magic, kMagicLen);
        buf[40] = static_cast<uint8_t>(seq & 0xFF);
        buf[41] = static_cast<uint8_t>((seq >> 8) & 0xFF);
        buf[42] = static_cast<uint8_t>((seq >> 16) & 0xFF);
        buf[43] = static_cast<uint8_t>((seq >> 24) & 0xFF);
        for (int i = 0; i < pad; ++i) {
            buf[44 + i] = static_cast<uint8_t>(0xA0 + (i & 0x0F));
        }
        return total;
    }

    bool frame_matches(const uint8_t *data, int length, const char magic[8],
                       uint32_t *seq_out)
    {
        if (!data || length < 44) {
            return false;
        }
        if (std::memcmp(data + 32, magic, kMagicLen) != 0) {
            return false;
        }
        const uint32_t seq = static_cast<uint32_t>(data[40]) |
                             (static_cast<uint32_t>(data[41]) << 8) |
                             (static_cast<uint32_t>(data[42]) << 16) |
                             (static_cast<uint32_t>(data[43]) << 24);
        if (seq_out) {
            *seq_out = seq;
        }
        return true;
    }

    void print_device_list()
    {
        const auto devices = ssv6xxx::UsbTransport::list_devices();
        if (devices.empty()) {
            std::printf("未发现 SSV6x5x USB 网卡。\n");
            return;
        }
        std::printf("发现 %zu 张 SSV6x5x USB 网卡：\n", devices.size());
        for (size_t i = 0; i < devices.size(); ++i) {
            const auto &d = devices[i];
            std::printf("  [%zu] bus=%u address=%u port=%s", i,
                        static_cast<unsigned>(d.bus),
                        static_cast<unsigned>(d.address),
                        d.port_path.empty() ? "?" : d.port_path.c_str());
            if (!d.product.empty()) {
                std::printf(" product=\"%s\"", d.product.c_str());
            }
            if (!d.serial.empty()) {
                std::printf(" serial=\"%s\"", d.serial.c_str());
            }
            std::printf("\n");
        }
    }

    ssv6xxx::UsbDeviceSelector make_selector(int bus, int address,
                                             const std::string &port)
    {
        ssv6xxx::UsbDeviceSelector sel;
        sel.bus       = bus;
        sel.address   = address;
        sel.port_path = port;
        return sel;
    }

    ssv6xxx::ChannelBandwidth parse_bw(const std::string &bandwidth)
    {
        if (bandwidth == "ht40+") {
            return ssv6xxx::ChannelBandwidth::HT40_PLUS;
        }
        if (bandwidth == "ht40-") {
            return ssv6xxx::ChannelBandwidth::HT40_MINUS;
        }
        return ssv6xxx::ChannelBandwidth::HT20;
    }

    /* 双向：两卡都 monitor + inject，按方向魔数过滤。 */
    ChannelResult run_bidir_case(ssv6xxx::SSV6xxxDriver &card_a,
                                 ssv6xxx::SSV6xxxDriver &card_b,
                                 const uint8_t mac_a[6], const uint8_t mac_b[6],
                                 int channel, ssv6xxx::ChannelBandwidth bw,
                                 int frames, int interval_ms, int poll_ms,
                                 int tx_rate_idx, int payload_len,
                                 int power_index, bool verbose,
                                 const std::shared_ptr<spdlog::logger> &logger)
    {
        ChannelResult result;
        result.channel     = channel;
        result.power_index = power_index;

        RxMatchState rx_a; /* A 期望收到 B 的帧 */
        RxMatchState rx_b; /* B 期望收到 A 的帧 */
        rx_a.expect_magic = kMagicBtoA;
        rx_b.expect_magic = kMagicAtoB;
        rx_a.verbose      = verbose;
        rx_b.verbose      = verbose;

        try {
            card_a.set_channel(static_cast<uint8_t>(channel), bw);
            card_b.set_channel(static_cast<uint8_t>(channel), bw);
            if (power_index >= 0) {
                card_a.set_tx_power_index(static_cast<uint32_t>(power_index));
                card_b.set_tx_power_index(static_cast<uint32_t>(power_index));
            }

            card_a.set_rx_callback(
                [](const ssv6xxx::RxFrameView &frame, void *user) {
                    auto *st = static_cast<RxMatchState *>(user);
                    st->total_rx.fetch_add(1, std::memory_order_relaxed);
                    uint32_t seq = 0;
                    if (!frame.data || !frame_matches(frame.data, frame.length,
                                                      st->expect_magic, &seq)) {
                        return;
                    }
                    st->matched.fetch_add(1, std::memory_order_relaxed);
                    st->last_seq.store(seq, std::memory_order_relaxed);
                    st->last_rssi.store(frame.metadata.rssi_dbm,
                                        std::memory_order_relaxed);
                    st->last_len.store(frame.length, std::memory_order_relaxed);
                    st->last_freq.store(frame.metadata.frequency_mhz,
                                        std::memory_order_relaxed);
                    if (st->verbose.load(std::memory_order_relaxed)) {
                        std::printf("[A-RX] seq=%u rssi=%d\n", seq,
                                    frame.metadata.rssi_dbm);
                    }
                },
                &rx_a);
            card_b.set_rx_callback(
                [](const ssv6xxx::RxFrameView &frame, void *user) {
                    auto *st = static_cast<RxMatchState *>(user);
                    st->total_rx.fetch_add(1, std::memory_order_relaxed);
                    uint32_t seq = 0;
                    if (!frame.data || !frame_matches(frame.data, frame.length,
                                                      st->expect_magic, &seq)) {
                        return;
                    }
                    st->matched.fetch_add(1, std::memory_order_relaxed);
                    st->last_seq.store(seq, std::memory_order_relaxed);
                    st->last_rssi.store(frame.metadata.rssi_dbm,
                                        std::memory_order_relaxed);
                    st->last_len.store(frame.length, std::memory_order_relaxed);
                    st->last_freq.store(frame.metadata.frequency_mhz,
                                        std::memory_order_relaxed);
                    if (st->verbose.load(std::memory_order_relaxed)) {
                        std::printf("[B-RX] seq=%u rssi=%d\n", seq,
                                    frame.metadata.rssi_dbm);
                    }
                },
                &rx_b);

            card_a.start_monitor();
            card_b.start_monitor();

            for (int i = 0; i < 4; ++i) {
                card_a.poll(poll_ms);
                card_b.poll(poll_ms);
            }

            uint8_t frame_ab[512];
            uint8_t frame_ba[512];
            int a_to_b_ok  = 0;
            int b_to_a_ok  = 0;
            int inject_err = 0;

            for (int i = 0; i < frames; ++i) {
                const uint32_t seq = static_cast<uint32_t>(i + 1);

                /* 两个方向必须用独立缓冲区，否则后一次 build 会覆盖前一次。 */
                const int len_ab = build_probe_frame(frame_ab, sizeof(frame_ab),
                                                     mac_a, kMagicAtoB, seq,
                                                     payload_len);
                const int len_ba = build_probe_frame(frame_ba, sizeof(frame_ba),
                                                     mac_b, kMagicBtoA, seq,
                                                     payload_len);
                if (len_ab < kMinInjectLen || len_ba < kMinInjectLen) {
                    inject_err += 2;
                    continue;
                }

                /* 交错注入，降低同刻碰撞。 */
                try {
                    card_a.inject_frame(frame_ab, len_ab, tx_rate_idx);
                    a_to_b_ok++;
                } catch (const std::exception &e) {
                    inject_err++;
                    logger->error("A→B 注入失败 seq={}: {}", seq, e.what());
                }

                const auto mid = std::chrono::steady_clock::now() +
                                 std::chrono::milliseconds(interval_ms / 2);
                while (std::chrono::steady_clock::now() < mid) {
                    card_a.poll(poll_ms);
                    card_b.poll(poll_ms);
                }

                try {
                    card_b.inject_frame(frame_ba, len_ba, tx_rate_idx);
                    b_to_a_ok++;
                } catch (const std::exception &e) {
                    inject_err++;
                    logger->error("B→A 注入失败 seq={}: {}", seq, e.what());
                }

                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(interval_ms / 2);
                while (std::chrono::steady_clock::now() < deadline) {
                    card_a.poll(poll_ms);
                    card_b.poll(poll_ms);
                }

                if ((i + 1) % 10 == 0 || i + 1 == frames) {
                    logger->info(
                        "ch{} 进度 {}/{} | A→B {}/{}  B→A {}/{} | 总RX A={} B={}",
                        channel, i + 1, frames, rx_b.matched.load(), a_to_b_ok,
                        rx_a.matched.load(), b_to_a_ok, rx_a.total_rx.load(),
                        rx_b.total_rx.load());
                }
            }

            const auto drain =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
            while (std::chrono::steady_clock::now() < drain) {
                card_a.poll(poll_ms);
                card_b.poll(poll_ms);
            }

            card_a.stop_monitor();
            card_b.stop_monitor();

            result.a_to_b_injected = a_to_b_ok;
            result.b_to_a_injected = b_to_a_ok;
            result.a_to_b_matched  = static_cast<int>(rx_b.matched.load());
            result.b_to_a_matched  = static_cast<int>(rx_a.matched.load());
            result.a_rx_total      = rx_a.total_rx.load();
            result.b_rx_total      = rx_b.total_rx.load();
            result.a_last_rssi     = rx_a.last_rssi.load();
            result.b_last_rssi     = rx_b.last_rssi.load();

            const int m_ab     = result.a_to_b_matched;
            const int m_ba     = result.b_to_a_matched;
            const bool pass_ab = a_to_b_ok > 0 && m_ab > 0;
            const bool pass_ba = b_to_a_ok > 0 && m_ba > 0;
            result.ok          = pass_ab && pass_ba;
            if (inject_err > 0) {
                result.note = "有注入错误";
            }
        } catch (const std::exception &e) {
            result.ok   = false;
            result.note = e.what();
            try {
                if (card_a.is_monitoring()) {
                    card_a.stop_monitor();
                }
            } catch (...) {
            }
            try {
                if (card_b.is_monitoring()) {
                    card_b.stop_monitor();
                }
            } catch (...) {
            }
        }

        return result;
    }

    /* 单向：仅 card_tx 注入，card_rx 监控。 */
    ChannelResult run_uni_case(ssv6xxx::SSV6xxxDriver &card_rx,
                               ssv6xxx::SSV6xxxDriver &card_tx,
                               const uint8_t mac_tx[6], int channel,
                               ssv6xxx::ChannelBandwidth bw, int frames,
                               int interval_ms, int poll_ms, int tx_rate_idx,
                               int payload_len, bool verbose,
                               const std::shared_ptr<spdlog::logger> &logger)
    {
        ChannelResult result;
        result.channel = channel;

        RxMatchState rx_state;
        rx_state.expect_magic = kMagicAtoB; /* 旧路径：卡B 仍用 A2B 魔数，源 MAC 是 TX 卡 */
        /* 为兼容，单向仍识别 AtoB；TX 卡源 MAC 由调用方传入。 */
        rx_state.verbose = verbose;

        /* 单向模式继续用 LIMOA2B（历史兼容），由 TX 卡发出。 */

        try {
            card_rx.set_channel(static_cast<uint8_t>(channel), bw);
            card_tx.set_channel(static_cast<uint8_t>(channel), bw);

            card_rx.set_rx_callback(
                [](const ssv6xxx::RxFrameView &frame, void *user) {
                    auto *st = static_cast<RxMatchState *>(user);
                    st->total_rx.fetch_add(1, std::memory_order_relaxed);
                    uint32_t seq = 0;
                    if (!frame.data || !frame_matches(frame.data, frame.length,
                                                      kMagicAtoB, &seq)) {
                        return;
                    }
                    st->matched.fetch_add(1, std::memory_order_relaxed);
                    st->last_seq.store(seq, std::memory_order_relaxed);
                    st->last_rssi.store(frame.metadata.rssi_dbm,
                                        std::memory_order_relaxed);
                    st->last_len.store(frame.length, std::memory_order_relaxed);
                    st->last_freq.store(frame.metadata.frequency_mhz,
                                        std::memory_order_relaxed);
                    if (st->verbose.load(std::memory_order_relaxed)) {
                        std::printf("[RX] seq=%u rssi=%d\n", seq,
                                    frame.metadata.rssi_dbm);
                    }
                },
                &rx_state);

            card_rx.start_monitor();
            for (int i = 0; i < 4; ++i) {
                card_rx.poll(poll_ms);
            }

            uint8_t frame_buf[512];
            int injected = 0;
            for (int i = 0; i < frames; ++i) {
                const int len = build_probe_frame(frame_buf, sizeof(frame_buf),
                                                  mac_tx, kMagicAtoB,
                                                  static_cast<uint32_t>(i + 1),
                                                  payload_len);
                if (len < kMinInjectLen) {
                    continue;
                }
                try {
                    card_tx.inject_frame(frame_buf, len, tx_rate_idx);
                    injected++;
                } catch (const std::exception &e) {
                    logger->error("注入失败: {}", e.what());
                }
                const auto deadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(interval_ms);
                while (std::chrono::steady_clock::now() < deadline) {
                    card_rx.poll(poll_ms);
                }
            }

            const auto drain =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
            while (std::chrono::steady_clock::now() < drain) {
                card_rx.poll(poll_ms);
            }
            card_rx.stop_monitor();

            result.a_to_b_injected = injected;
            result.a_to_b_matched  = static_cast<int>(rx_state.matched.load());
            result.a_rx_total      = rx_state.total_rx.load();
            result.a_last_rssi     = rx_state.last_rssi.load();
            result.ok              = injected > 0 && result.a_to_b_matched > 0;
        } catch (const std::exception &e) {
            result.ok   = false;
            result.note = e.what();
            try {
                if (card_rx.is_monitoring()) {
                    card_rx.stop_monitor();
                }
            } catch (...) {
            }
        }

        return result;
    }

    /**
     * 连续大包注入：按 wall-clock 时长尽可能背靠背发帧（interval 不再使用）。
     * 802.11 单帧上限为 MAX_FRAME_SIZE（2432），无法真正 1MB 单包。
     */
    ChannelResult run_continuous_burst(ssv6xxx::SSV6xxxDriver &card_rx,
                                       ssv6xxx::SSV6xxxDriver &card_tx,
                                       const uint8_t mac_tx[6], int channel,
                                       ssv6xxx::ChannelBandwidth bw,
                                       int duration_s, int frame_bytes,
                                       int power_index, int tx_rate_idx,
                                       int poll_ms, bool verbose,
                                       const std::shared_ptr<spdlog::logger> &logger)
    {
        ChannelResult result;
        result.channel     = channel;
        result.power_index = power_index;

        int flen = frame_bytes;
        if (flen <= 0) {
            flen = kMaxInjectLen;
        }
        if (flen < kMinInjectLen) {
            flen = kMinInjectLen;
        }
        if (flen > kMaxInjectLen) {
            flen = kMaxInjectLen;
        }
        const int pad = flen - 24 - 8 - static_cast<int>(kMagicLen) - 4;
        if (pad < 16) {
            result.note = "frame too small";
            return result;
        }

        RxMatchState rx_state;
        rx_state.verbose = verbose;

        try {
            card_rx.set_channel(static_cast<uint8_t>(channel), bw);
            card_tx.set_channel(static_cast<uint8_t>(channel), bw);
            if (power_index >= 0) {
                card_rx.set_tx_power_index(static_cast<uint32_t>(power_index));
                card_tx.set_tx_power_index(static_cast<uint32_t>(power_index));
            }

            card_rx.set_rx_callback(
                [](const ssv6xxx::RxFrameView &frame, void *user) {
                    auto *st = static_cast<RxMatchState *>(user);
                    st->total_rx.fetch_add(1, std::memory_order_relaxed);
                    uint32_t seq = 0;
                    if (!frame.data || !frame_matches(frame.data, frame.length,
                                                      kMagicAtoB, &seq)) {
                        return;
                    }
                    st->matched.fetch_add(1, std::memory_order_relaxed);
                    st->last_seq.store(seq, std::memory_order_relaxed);
                    st->last_rssi.store(frame.metadata.rssi_dbm,
                                        std::memory_order_relaxed);
                    st->last_len.store(frame.length, std::memory_order_relaxed);
                },
                &rx_state);

            card_rx.start_monitor();
            for (int i = 0; i < 3; ++i) {
                card_rx.poll(1);
            }

            std::vector<uint8_t> frame_buf(static_cast<size_t>(flen));
            const auto t0     = std::chrono::steady_clock::now();
            const auto t_end  = t0 + std::chrono::seconds(duration_s);
            const auto t_log  = t0 + std::chrono::seconds(1);
            auto next_log     = t_log;
            uint32_t seq      = 0;
            uint64_t injected = 0;
            uint64_t errors   = 0;

            logger->info("连续发射: ch={} 帧长={}B 时长={}s 功率={}", channel, flen,
                         duration_s, power_index < 0 ? "def" : std::to_string(power_index));
            std::printf("TX_START ch=%d frame=%dB duration=%ds freq~=%dMHz\n", channel,
                        flen, duration_s, channel <= 14 ? 2407 + channel * 5 : 5000 + channel * 5);
            std::fflush(stdout);

            while (std::chrono::steady_clock::now() < t_end) {
                ++seq;
                if (build_probe_frame(frame_buf.data(), frame_buf.size(), mac_tx,
                                      kMagicAtoB, seq, pad) < kMinInjectLen) {
                    ++errors;
                    continue;
                }
                try {
                    card_tx.inject_frame(frame_buf.data(), flen, tx_rate_idx);
                    ++injected;
                } catch (const std::exception &e) {
                    ++errors;
                    if (errors <= 5) {
                        logger->error("注入失败: {}", e.what());
                    }
                }
                /* 偶尔驱动 RX，避免 URB 饿死；不按 interval 节流。 */
                if ((injected & 0x0Fu) == 0) {
                    card_rx.poll(0);
                }
                const auto now = std::chrono::steady_clock::now();
                if (now >= next_log) {
                    const auto el = std::chrono::duration_cast<std::chrono::seconds>(now - t0).count();
                    std::printf("TX_PROGRESS t=%llus frames=%llu matched=%llu\n",
                                static_cast<unsigned long long>(el),
                                static_cast<unsigned long long>(injected),
                                static_cast<unsigned long long>(rx_state.matched.load()));
                    std::fflush(stdout);
                    next_log = now + std::chrono::seconds(1);
                }
            }

            std::printf("TX_STOP frames=%llu errors=%llu\n",
                        static_cast<unsigned long long>(injected),
                        static_cast<unsigned long long>(errors));
            std::fflush(stdout);

            const auto drain =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
            while (std::chrono::steady_clock::now() < drain) {
                card_rx.poll(1);
            }
            card_rx.stop_monitor();

            result.a_to_b_injected = static_cast<int>(injected > 0x7fffffff
                                                          ? 0x7fffffff
                                                          : injected);
            result.a_to_b_matched =
                static_cast<int>(rx_state.matched.load() > 0x7fffffff
                                     ? 0x7fffffff
                                     : rx_state.matched.load());
            result.a_rx_total  = rx_state.total_rx.load();
            result.a_last_rssi = rx_state.last_rssi.load();
            result.ok          = injected > 0;
            if (errors > 0) {
                result.note = "errors=" + std::to_string(errors);
            }
        } catch (const std::exception &e) {
            result.ok   = false;
            result.note = e.what();
            try {
                if (card_rx.is_monitoring()) {
                    card_rx.stop_monitor();
                }
            } catch (...) {
            }
        }
        return result;
    }

    void print_case_result(const ChannelResult &r, bool bidir)
    {
        char pwr[16];
        if (r.power_index < 0) {
            std::snprintf(pwr, sizeof(pwr), "def");
        } else {
            std::snprintf(pwr, sizeof(pwr), "%d", r.power_index);
        }
        if (bidir) {
            const double rate_ab =
                r.a_to_b_injected > 0
                    ? 100.0 * r.a_to_b_matched / r.a_to_b_injected
                    : 0.0;
            const double rate_ba =
                r.b_to_a_injected > 0
                    ? 100.0 * r.b_to_a_matched / r.b_to_a_injected
                    : 0.0;
            std::printf(
                "ch%-3d pwr=%-4s A→B %3d/%-3d (%5.1f%%)  B→A %3d/%-3d (%5.1f%%)  RSSI A=%d B=%d  %s%s\n",
                r.channel, pwr, r.a_to_b_matched, r.a_to_b_injected, rate_ab,
                r.b_to_a_matched, r.b_to_a_injected, rate_ba,
                static_cast<int>(r.a_last_rssi), static_cast<int>(r.b_last_rssi),
                r.ok ? "PASS" : "FAIL",
                r.note.empty() ? "" : (" (" + r.note + ")").c_str());
        } else {
            const double rate = r.a_to_b_injected > 0
                                    ? 100.0 * r.a_to_b_matched / r.a_to_b_injected
                                    : 0.0;
            std::printf("ch%-3d pwr=%-4s TX→RX %3d/%-3d (%5.1f%%)  RSSI=%d  %s%s\n",
                        r.channel, pwr, r.a_to_b_matched, r.a_to_b_injected, rate,
                        static_cast<int>(r.a_last_rssi),
                        r.ok ? "PASS" : "FAIL",
                        r.note.empty() ? "" : (" (" + r.note + ")").c_str());
        }
    }

} // namespace

int main(int argc, char *argv[])
{
#ifdef _WIN32
    /* Windows 控制台默认 GBK，中文 UTF-8 日志会乱码。 */
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    std::string fw_path   = "firmware/ssv6x5x-sw.bin";
    std::string bandwidth = "ht20";
    std::string tx_port;
    std::string rx_port;
    std::string channels_text;
    std::string mac_a_text = "02:00:00:00:00:01";
    std::string mac_b_text = "02:00:00:00:00:02";
    int channel            = 6;
    int count              = 30;
    int interval_ms        = 40;
    int poll_ms            = 5;
    int tx_rate_idx        = 0;
    int payload_len        = 48;
    int frame_bytes        = 0;
    int duration_s         = 0;
    int switch_iter        = 0;
    int switch_to          = 157;
    int tx_power_index     = -1;
    std::string tx_power_list_text;
    int tx_bus        = -1;
    int tx_address    = -1;
    int rx_bus        = -1;
    int rx_address    = -1;
    int list_devices  = 0;
    int verbose       = 0;
    int bidir         = 0;
    int reverse_roles = 0;

    CLI::App app{"SSV6x5X 双卡空口通讯测试（支持单向/双向）"};
    app.add_option("-f,--firmware", fw_path, "固件路径")->capture_default_str();
    app.add_option("-c,--channel", channel, "单信道（与 --channels 二选一）")
        ->capture_default_str()
        ->check(CLI::Range(1, 165));
    app.add_option("--channels", channels_text,
                   "信道列表，逗号分隔，例如 1,6,11,149");
    app.add_option("-b,--bandwidth", bandwidth, "带宽：ht20, ht40+, ht40-")
        ->capture_default_str()
        ->check([](const std::string &s) {
            if (s == "ht20" || s == "ht40+" || s == "ht40-") {
                return std::string();
            }
            return std::string("带宽必须是 ht20, ht40+, 或 ht40-");
        });
    app.add_option("-n,--count", count, "每个信道注入帧数（每方向）")
        ->capture_default_str()
        ->check(CLI::PositiveNumber);
    app.add_option("-i,--interval", interval_ms,
                   "每帧总时隙（毫秒）；0=无间隔连续注入")
        ->capture_default_str()
        ->check(CLI::NonNegativeNumber);
    app.add_option("--duration", duration_s,
                   "连续发射时长（秒）；>0 时按时间发满，-n 仅作上限")
        ->capture_default_str()
        ->check(CLI::NonNegativeNumber);
    app.add_option("--frame-bytes", frame_bytes,
                   "整帧长度（字节，含 802.11 头）；0=按 payload 计算；最大 2432")
        ->capture_default_str()
        ->check(CLI::Range(0, MAX_FRAME_SIZE));
    app.add_option("--poll", poll_ms, "RX poll 超时（毫秒）")
        ->capture_default_str()
        ->check(CLI::PositiveNumber);
    app.add_option("--payload", payload_len, "额外填充长度（16-200）")
        ->capture_default_str()
        ->check(CLI::Range(16, 200));
    app.add_option("--tx-rate-index", tx_rate_idx, "TX 速率索引（0=安全默认）")
        ->capture_default_str()
        ->check(CLI::Range(0, 255));
    app.add_option("--tx-power-index", tx_power_index,
                   "TX 功率索引 0-127；-1=工厂校准默认")
        ->capture_default_str()
        ->check(CLI::Range(-1, 127));
    app.add_option("--tx-power-list", tx_power_list_text,
                   "功率索引列表，逗号分隔，例如 0,32,64,96,127（两卡同功率扫）");
    app.add_option("--mac-a", mac_a_text, "卡A 源 MAC")->capture_default_str();
    app.add_option("--mac-b", mac_b_text, "卡B 源 MAC")->capture_default_str();
    app.add_option("--tx-port", tx_port, "卡B/TX USB 物理端口（例如 1.1.1.1）");
    app.add_option("--rx-port", rx_port, "卡A/RX USB 物理端口（例如 1.1.1.3）");
    app.add_option("--tx-bus", tx_bus, "卡B/TX USB 总线号")->check(CLI::Range(0, 255));
    app.add_option("--tx-address", tx_address, "卡B/TX USB 地址")->check(CLI::Range(1, 255));
    app.add_option("--rx-bus", rx_bus, "卡A/RX USB 总线号")->check(CLI::Range(0, 255));
    app.add_option("--rx-address", rx_address, "卡A/RX USB 地址")->check(CLI::Range(1, 255));
    app.add_option("--switch-iter", switch_iter,
                   "信道切换测时次数：用 -c 与 --switch-to 做往返切换")
        ->capture_default_str()
        ->check(CLI::NonNegativeNumber);
    app.add_option("--switch-to", switch_to, "切换目标信道（与 -c 往返）")
        ->capture_default_str()
        ->check(CLI::Range(1, 165));
    app.add_flag("--bidir", bidir, "双向模式：两卡同时监控+注入");
    app.add_flag("--swap", reverse_roles, "交换默认设备角色");
    app.add_flag("--list-devices", list_devices, "列出网卡后退出");
    app.add_flag("-v,--verbose", verbose, "详细输出");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        return app.exit(e);
    }

    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);
    auto logger = spdlog::default_logger();

    if (list_devices) {
        try {
            print_device_list();
        } catch (const std::exception &e) {
            std::fprintf(stderr, "枚举失败: %s\n", e.what());
            return 1;
        }
        return 0;
    }

    std::vector<int> channels;
    if (!channels_text.empty()) {
        if (!parse_int_list(channels_text, 1, 165, &channels)) {
            std::fprintf(stderr, "信道列表无效: %s\n", channels_text.c_str());
            return 1;
        }
    } else {
        channels.push_back(channel);
    }

    std::vector<int> powers;
    if (!tx_power_list_text.empty()) {
        if (!parse_int_list(tx_power_list_text, 0, 127, &powers)) {
            std::fprintf(stderr, "功率列表无效: %s\n", tx_power_list_text.c_str());
            return 1;
        }
    } else {
        powers.push_back(tx_power_index);
    }

    uint8_t mac_a[6];
    uint8_t mac_b[6];
    if (!parse_mac(mac_a_text, mac_a) || !parse_mac(mac_b_text, mac_b)) {
        std::fprintf(stderr, "MAC 地址格式错误，应为 XX:XX:XX:XX:XX:XX\n");
        return 1;
    }

    ssv6xxx::UsbDeviceSelector sel_a =
        make_selector(rx_bus, rx_address, rx_port);
    ssv6xxx::UsbDeviceSelector sel_b =
        make_selector(tx_bus, tx_address, tx_port);

    if (sel_a.empty() || sel_b.empty()) {
        try {
            const auto devices = ssv6xxx::UsbTransport::list_devices();
            if (devices.size() < 2) {
                std::fprintf(stderr, "需要至少 2 张网卡，当前 %zu 张。\n",
                             devices.size());
                return 1;
            }
            if (reverse_roles) {
                if (sel_a.empty()) {
                    sel_a = make_selector(devices[0].bus, devices[0].address,
                                          devices[0].port_path);
                }
                if (sel_b.empty()) {
                    sel_b = make_selector(devices[1].bus, devices[1].address,
                                          devices[1].port_path);
                }
            } else {
                if (sel_a.empty()) {
                    sel_a = make_selector(devices[0].bus, devices[0].address,
                                          devices[0].port_path);
                }
                if (sel_b.empty()) {
                    sel_b = make_selector(devices[1].bus, devices[1].address,
                                          devices[1].port_path);
                }
            }
            logger->info("设备选择: A(RX/左) port={}  B(TX/右) port={}",
                         sel_a.port_path, sel_b.port_path);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "枚举/选择设备失败: %s\n", e.what());
            return 1;
        }
    }

    if (sel_a.port_path == sel_b.port_path && sel_a.bus == sel_b.bus &&
        sel_a.address == sel_b.address) {
        std::fprintf(stderr, "A/B 选择了同一张卡。\n");
        return 1;
    }

    const ssv6xxx::ChannelBandwidth bw = parse_bw(bandwidth);

    auto make_config = [&](const ssv6xxx::UsbDeviceSelector &sel,
                           const uint8_t mac[6]) {
        ssv6xxx::DriverConfig cfg;
        cfg.firmware_path = fw_path;
        /* 始终先在 2.4G 信道 6 完成初始化；扫描 5G 时再 set_channel 切换。
           部分板卡在 5G 上直接跑 7 阶段 init 会卡在 INIT_PLL_PHY_RF。 */
        cfg.default_channel = 6;
        cfg.bandwidth       = ssv6xxx::ChannelBandwidth::HT20;
        cfg.usb_device      = sel;
        cfg.verbose         = verbose != 0;
        std::memcpy(cfg.mac_addr, mac, 6);
        return cfg;
    };

    int ret = 0;
    try {
        logger->info("========================================");
        logger->info("  SSV6x5X 双卡通讯测试  模式={}",
                     bidir ? "双向" : "单向");
        logger->info("  信道 {}  每信道帧数 {}  时隙 {} ms",
                     channels.size() == 1
                         ? std::to_string(channels[0])
                         : channels_text,
                     count, interval_ms);
        logger->info("========================================");

        logger->info("初始化卡A...");
        ssv6xxx::SSV6xxxDriver card_a(logger);
        card_a.init(make_config(sel_a, mac_a));
        logger->info("卡A就绪 chip={} port={}", card_a.get_chip_id(),
                     sel_a.port_path);

        logger->info("初始化卡B...");
        ssv6xxx::SSV6xxxDriver card_b(logger);
        card_b.init(make_config(sel_b, mac_b));
        logger->info("卡B就绪 chip={} port={}", card_b.get_chip_id(),
                     sel_b.port_path);

        /* 信道切换测时：只在卡A上做，避免双卡 USB 互相干扰。 */
        if (switch_iter > 0) {
            const int ch_a = channels.empty() ? channel : channels.front();
            const int ch_b = switch_to;
            logger->info("信道切换测时: {} <-> {}  各 {} 次", ch_a, ch_b,
                         switch_iter);
            card_a.set_channel(static_cast<uint8_t>(ch_a), bw);
            std::vector<double> to_b_ms;
            std::vector<double> to_a_ms;
            to_b_ms.reserve(static_cast<size_t>(switch_iter));
            to_a_ms.reserve(static_cast<size_t>(switch_iter));
            for (int i = 0; i < switch_iter; ++i) {
                auto t0 = std::chrono::steady_clock::now();
                card_a.set_channel(static_cast<uint8_t>(ch_b), bw);
                auto t1 = std::chrono::steady_clock::now();
                to_b_ms.push_back(
                    std::chrono::duration<double, std::milli>(t1 - t0).count());

                t0 = std::chrono::steady_clock::now();
                card_a.set_channel(static_cast<uint8_t>(ch_a), bw);
                t1 = std::chrono::steady_clock::now();
                to_a_ms.push_back(
                    std::chrono::duration<double, std::milli>(t1 - t0).count());
            }
            auto stats = [](const std::vector<double> &v, const char *label) {
                double sum = 0, mn = v[0], mx = v[0];
                for (double x : v) {
                    sum += x;
                    if (x < mn) mn = x;
                    if (x > mx) mx = x;
                }
                const double avg = sum / static_cast<double>(v.size());
                std::printf("SWITCH %s  n=%zu  avg=%.2f ms  min=%.2f  max=%.2f\n",
                            label, v.size(), avg, mn, mx);
                std::fflush(stdout);
                return avg;
            };
            std::printf("\n========== 信道切换耗时 ==========\n");
            std::printf("路径: ch%d -> ch%d -> ch%d  (%d 往返)\n", ch_a, ch_b, ch_a,
                        switch_iter);
            const double avg_ab = stats(to_b_ms, (std::to_string(ch_a) + "->" +
                                                  std::to_string(ch_b))
                                                     .c_str());
            const double avg_ba = stats(to_a_ms, (std::to_string(ch_b) + "->" +
                                                  std::to_string(ch_a))
                                                     .c_str());
            const double avg_rt = avg_ab + avg_ba;
            std::printf("往返平均: %.2f ms (切过去+切回来)\n", avg_rt);
            std::printf("RESULT: PASS\n");
            return 0;
        }

        std::vector<ChannelResult> results;

        /* 连续大包模式：按 --duration 背靠背发满时长（默认用最大帧）。 */
        if (duration_s > 0) {
            const int pwr = tx_power_index;
            for (int ch : channels) {
                ChannelResult r = run_continuous_burst(
                    card_a, card_b, mac_b, ch, bw, duration_s, frame_bytes, pwr,
                    tx_rate_idx, poll_ms, verbose != 0, logger);
                results.push_back(r);
                print_case_result(r, false);
            }
            int cpass = 0;
            for (const auto &r : results) {
                if (r.ok) {
                    cpass++;
                }
            }
            std::printf("连续发射完成: %d/%d\n", cpass,
                        static_cast<int>(results.size()));
            std::printf("RESULT: %s\n",
                        cpass == static_cast<int>(results.size()) && !results.empty()
                            ? "PASS"
                            : "FAIL");
            return cpass == static_cast<int>(results.size()) && !results.empty() ? 0 : 3;
        }

        std::printf("\n========== 测试结果 ==========\n");
        for (int pwr : powers) {
            for (int ch : channels) {
                ChannelResult r;
                if (bidir) {
                    r = run_bidir_case(card_a, card_b, mac_a, mac_b, ch, bw,
                                       count, interval_ms, poll_ms, tx_rate_idx,
                                       payload_len, pwr, verbose != 0, logger);
                } else {
                    r             = run_uni_case(card_a, card_b, mac_b, ch, bw, count,
                                                 interval_ms, poll_ms, tx_rate_idx,
                                                 payload_len, verbose != 0, logger);
                    r.power_index = pwr;
                }
                results.push_back(r);
                print_case_result(r, bidir != 0);
            }
        }

        int pass = 0;
        for (const auto &r : results) {
            if (r.ok) {
                pass++;
            }
        }
        std::printf("----------------------------------\n");
        std::printf("汇总: %d/%d 个信道 PASS\n", pass,
                    static_cast<int>(results.size()));
        if (bidir) {
            std::printf("说明: A→B 表示卡A注入、卡B匹配；B→A 表示卡B注入、卡A匹配\n");
        }
        std::printf("==================================\n");

        if (pass == static_cast<int>(results.size()) && !results.empty()) {
            std::printf("RESULT: PASS\n");
            ret = 0;
        } else if (pass > 0) {
            std::printf("RESULT: PARTIAL\n");
            ret = 2;
        } else {
            std::printf("RESULT: FAIL\n");
            ret = 3;
        }
    } catch (const ssv6xxx::UsbException &e) {
        std::fprintf(stderr, "USB 错误: %s\n", e.what());
        ret = 1;
    } catch (const ssv6xxx::InitException &e) {
        std::fprintf(stderr, "初始化错误: %s\n", e.what());
        ret = 1;
    } catch (const std::exception &e) {
        std::fprintf(stderr, "错误: %s\n", e.what());
        ret = 1;
    }

    return ret;
}
