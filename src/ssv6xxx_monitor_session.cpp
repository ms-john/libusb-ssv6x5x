/*
 * ssv6xxx_monitor_session.cpp - SSV6X5X 监控/注入会话实现 (C++14)
 *
 * 实现帧接收和注入功能：
 *   - 异步 RX URB 提交与回调处理
 *   - 802.11 帧解析（剥离硬件描述符）
 *   - TX 帧注入（预置 80 字节 TX 描述符）
 *   - PCap 文件输出（radiotap 格式）
 *   - 统计信息跟踪
 *
 * 源代码来源：
 *   - 内核驱动的 RX/TX 路径
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <new>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "ssv6xxx_descriptors.hpp"
#include "ieee80211_radiotap.h"
#include "ssv6xxx_monitor_session.hpp"
#include "ssv6xxx_regs.hpp"
#include "ssv6xxx_usb_transport.hpp"

namespace ssv6xxx
{

    /* ==================== 常量定义 ==================== */

    constexpr int MON_RX_BUF_SIZE = USB_RX_BUF_SIZE; ///< RX 缓冲区大小
    constexpr int TX_BUF_SIZE =
        SSV6XXX_TX_DESC_LEN + MAX_FRAME_SIZE + 4; ///< TX 缓冲区大小

    /* PCap 格式常量 */
    constexpr uint32_t PCAP_MAGIC                        = 0xA1B2C3D4;
    constexpr uint32_t PCAP_LINKTYPE_IEEE802_11_RADIOTAP = 127;

    /* Radiotap 存在位标志 */
    enum class RadiotapPresence : uint32_t {
        TSFT          = 1U << 0,
        FLAGS         = 1U << 1,
        RATE          = 1U << 2,
        CHANNEL       = 1U << 3,
        FHSS          = 1U << 4,
        DBM_ANTSIGNAL = 1U << 5,
        DBM_ANTNOISE  = 1U << 6,
        MCS           = 1U << 19,
    };

    /* Radiotap 标志位 */
    enum class RadiotapFlags : uint16_t {
        SHORTGI = 1U << 8, ///< Short Guard Interval
    };

    /* Radiotap 信道标志 */
    enum class RadiotapChanFlags : uint16_t {
        CCK      = 0x0020,
        OFDM     = 0x0040,
        GHZ      = 0x0080,
        FIVE_GHZ = 0x0100,
    };

    /* ==================== 内部辅助函数 ==================== */

    namespace
    {

        static_assert(sizeof(Ssv6200RxDesc) == SSV6XXX_RX_HW_DESC_LEN,
                      "ssv6006_rx_desc ABI must remain 80 bytes");

        bool is_valid_channel(uint8_t channel)
        {
            return (channel >= 1 && channel <= 14) || (channel >= 36 && channel <= 165);
        }

        uint32_t read_le_u32(const uint8_t *p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) |
                   (static_cast<uint32_t>(p[3]) << 24);
        }

        uint16_t read_le_u16(const uint8_t *p)
        {
            return static_cast<uint16_t>(p[0]) |
                   static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
        }

        void write_le_u16(uint8_t *p, uint16_t value)
        {
            p[0] = static_cast<uint8_t>(value & 0xFFu);
            p[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
        }

        void write_le_u32(uint8_t *p, uint32_t value)
        {
            p[0] = static_cast<uint8_t>(value & 0xFFu);
            p[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
            p[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
            p[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
        }

        std::string mac_to_string(const uint8_t *mac)
        {
            char buf[18];
            std::snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0],
                          mac[1], mac[2], mac[3], mac[4], mac[5]);
            return std::string(buf);
        }

        const char *frame_type_name(uint8_t type)
        {
            switch (type & 0x3u) {
                case 0:
                    return "mgmt";
                case 1:
                    return "ctrl";
                case 2:
                    return "data";
                default:
                    return "ext";
            }
        }

        const char *mgmt_subtype_name(uint8_t subtype)
        {
            switch (subtype & 0xFu) {
                case 0:
                    return "assoc-req";
                case 1:
                    return "assoc-resp";
                case 4:
                    return "probe-req";
                case 5:
                    return "probe-resp";
                case 8:
                    return "beacon";
                case 10:
                    return "disassoc";
                case 11:
                    return "auth";
                case 12:
                    return "deauth";
                default:
                    return "mgmt-other";
            }
        }

        struct RxDescView {
            uint32_t word1{0};
            uint32_t word2{0};
            uint32_t word3{0};
            uint32_t word4{0};
            uint32_t word5{0};
            uint32_t word6{0};
            uint32_t word7{0};
            uint16_t len{0};
            uint8_t c_type{0};
            uint8_t f80211{0};
            uint8_t channel{0};
            uint8_t phy_rate{0};
            uint8_t phy_rssi{0};
            uint8_t phy_snr{0};
            uint8_t phy_stbc{0};
            uint8_t phy_fec{0};
            uint8_t phy_n_ess{0};
            uint8_t phy_l_rate{0};
            uint16_t phy_packet_length{0};
            int16_t phy_frequency_offset{0};
            uint32_t rx_timestamp_1{0};
            uint32_t rx_timestamp_2{0};
            uint16_t sequence{0};
            uint8_t h_event{0};
        };

        struct FrameView {
            uint16_t frame_control{0};
            uint8_t type{0};
            uint8_t subtype{0};
            const uint8_t *addr1{nullptr};
            const uint8_t *addr2{nullptr};
            const uint8_t *addr3{nullptr};
        };

        bool parse_frame_view(const uint8_t *frame, int len, FrameView *out)
        {
            if (!frame || !out || len < 24) {
                return false;
            }

            out->frame_control = read_le_u16(frame);
            out->type          = static_cast<uint8_t>((out->frame_control >> 2) & 0x3u);
            out->subtype       = static_cast<uint8_t>((out->frame_control >> 4) & 0xFu);
            out->addr1         = frame + 4;
            out->addr2         = frame + 10;
            out->addr3         = frame + 16;
            return true;
        }

        bool parse_rx_desc_view(const uint8_t *data, int len, RxDescView *out)
        {
            if (!data || !out || len < SSV6XXX_RX_HW_DESC_LEN) {
                return false;
            }

            out->word1  = read_le_u32(data + 0);
            out->word2  = read_le_u32(data + 4);
            out->word3  = read_le_u32(data + 8);
            out->word4  = read_le_u32(data + 12);
            out->word5  = read_le_u32(data + 16);
            out->word6  = read_le_u32(data + 20);
            out->word7  = read_le_u32(data + 24);
            out->len    = static_cast<uint16_t>(out->word1 & 0xFFFFu);
            out->c_type = static_cast<uint8_t>((out->word1 >> 16) & 0x7u);
            out->f80211 = static_cast<uint8_t>((out->word1 >> 19) & 0x1u);
            /* ssv6006_rx_desc WORD_4: payload_offset is [7:0], channel is [31:24]. */
            out->channel = static_cast<uint8_t>((out->word4 >> 24) & 0xFFu);
            out->h_event = static_cast<uint8_t>((out->word1 >> 24) & 0xFFu);

            /* ssv6006_rx_desc WORD5-WORD7 contain the PHY report.  The 4-byte
             * rx_pinfo_pad is appended after the MPDU and is not part of this
             * descriptor. */
            out->phy_packet_length    = static_cast<uint16_t>(out->word5 & 0xFFFFu);
            out->phy_rate             = static_cast<uint8_t>((out->word5 >> 16) & 0xFFu);
            out->phy_stbc             = static_cast<uint8_t>((out->word5 >> 27) & 0x3u);
            out->phy_fec              = static_cast<uint8_t>((out->word5 >> 29) & 0x1u);
            out->phy_n_ess            = static_cast<uint8_t>((out->word5 >> 30) & 0x3u);
            out->phy_l_rate           = static_cast<uint8_t>((out->word6 >> 12) & 0x7u);
            out->phy_rssi             = static_cast<uint8_t>((out->word6 >> 16) & 0xFFu);
            out->phy_snr              = static_cast<uint8_t>((out->word6 >> 24) & 0xFFu);
            out->phy_frequency_offset = static_cast<int16_t>(out->word7 & 0xFFFFu);
            out->rx_timestamp_1       = read_le_u32(data + 28);
            out->rx_timestamp_2       = read_le_u32(data + 32);
            out->sequence             = static_cast<uint16_t>(read_le_u32(data + 76) & 0xFFFFu);

            return true;
        }

        bool looks_like_rx_desc(const uint8_t *data, int len)
        {
            RxDescView view;
            if (!parse_rx_desc_view(data, len, &view)) {
                return false;
            }

            if (view.len < 24 || view.len > static_cast<uint16_t>(MAX_FRAME_SIZE + 400)) {
                return false;
            }

            if (view.f80211 != 1) {
                return false;
            }

            return view.c_type == 3 || view.c_type == 4;
        }

        uint8_t select_rx_channel(uint8_t reported, uint8_t configured)
        {
            if (is_valid_channel(reported)) {
                if (is_valid_channel(configured)) {
                    bool reported_5g   = reported > 14;
                    bool configured_5g = configured > 14;
                    if (reported_5g != configured_5g) {
                        return configured;
                    }
                }
                return reported;
            }

            return is_valid_channel(configured) ? configured : 1;
        }

        int8_t decode_rssi_dbm(uint8_t raw_rssi)
        {
            if (raw_rssi == 0) {
                return 0;
            }

            int rssi = -static_cast<int>(raw_rssi);
            if (rssi < -127) {
                return -127;
            }
            if (rssi > 0) {
                return 0;
            }
            return static_cast<int8_t>(rssi);
        }

        /**
         * @brief 解码物理层速率
         *
         * 将硬件报告的 phy_rate 字节解码为 Mbps 速率和模式信息。
         *
         * phy_rate 布局：
         *   [7:6] = PHY_MODE  (0=B, 2=G, 3=N)
         *   [5]   = HT40 flag (1=40MHz, 0=20MHz)
         *   [4]   = Short GI / short preamble
         *   [2:0] = Rate index within mode
         *
         * @param phy_rate 硬件原始速率值
         * @param out_is_ht 输出：是否为 HT 模式
         * @param out_is_40mhz 输出：是否为 40MHz 带宽
         * @param out_short_gi 输出：是否使用短保护间隔
         * @return 速率（Mbps）
         */
        uint8_t decode_phy_rate(uint8_t phy_rate, bool *out_is_ht, bool *out_is_40mhz,
                                bool *out_short_gi)
        {
            uint8_t phy_mode = (phy_rate >> 6) & 0x03;
            bool is_40mhz    = (phy_rate >> 5) & 0x01;
            bool short_gi    = (phy_rate >> 4) & 0x01;
            uint8_t rate_idx = phy_rate & 0x07;

            *out_is_40mhz = is_40mhz;
            *out_short_gi = short_gi;

            if (phy_mode == 3) { // N_MODE (802.11n)
                *out_is_ht = true;
                /* Rounded Mbps values from the kernel driver's rate tables. */
                static const uint8_t ht20_lgi[8] = {7, 13, 20, 26, 39, 52, 59, 65};
                static const uint8_t ht20_sgi[8] = {7, 14, 22, 29, 43, 58, 65, 72};
                static const uint8_t ht40_lgi[8] = {14, 27, 41, 54, 81, 108, 122, 135};
                static const uint8_t ht40_sgi[8] = {15, 30, 45, 60, 90, 120, 135, 150};
                if (is_40mhz) {
                    return (short_gi ? ht40_sgi : ht40_lgi)[rate_idx];
                }
                return (short_gi ? ht20_sgi : ht20_lgi)[rate_idx];
            } else if (phy_mode == 2) { // G_MODE (802.11g)
                *out_is_ht                      = false;
                static const uint8_t g_rates[8] = {6, 9, 12, 18, 24, 36, 48, 54};
                return g_rates[rate_idx & 0x07];
            } else if (phy_mode == 0) {
                *out_is_ht                      = false;
                static const uint8_t b_rates[8] = {1, 2, 5, 11, 0, 0, 0, 0};
                return b_rates[rate_idx & 0x03];
            }
            *out_is_ht = false;
            return 0;
        }

        /* Convert the public rate_idx values to the kernel's 8-bit rate-control
         * word: [7:6] PHY mode, [5] HT40, [4] SGI/short preamble, [2:0] rate. */
        uint8_t encode_tx_rate(int rate_idx, bool five_ghz)
        {
            if (rate_idx == 0) {
                /* The firmware has no useful B-mode rate on 5 GHz. */
                return five_ghz ? 0x80 : 0x00; // G 6M / B 1M
            }

            /* Values with the PHY mode bits already set are accepted as raw
             * kernel rate-control words, e.g. 0xC0..0xC7 = HT20 MCS0..7. */
            if (rate_idx >= 0x80 && rate_idx <= 0xFF) {
                return static_cast<uint8_t>(rate_idx);
            }

            /* Canonical rate indices used by the kernel driver's RF tool:
             *   0..3   B 1/2/5.5/11M (long preamble)
             *   5..6   B 5.5/11M (short preamble)
             *   7..14  G 6/9/12/18/24/36/48/54M
             *   15..22 HT20 MCS0..7 (long GI)
             *   23..30 HT20 MCS0..7 (short GI)
             *   31..38 HT40 MCS0..7 (long GI)
             *   39..46 HT40 MCS0..7 (short GI)
             */
            if (rate_idx >= 1 && rate_idx <= 4) {
                return static_cast<uint8_t>(rate_idx - 1);
            }
            if (rate_idx == 5) {
                return 0x12; /* B 5.5M, short preamble */
            }
            if (rate_idx == 6) {
                return 0x13; /* B 11M, short preamble */
            }
            if (rate_idx >= 7 && rate_idx <= 14) {
                return static_cast<uint8_t>(0x80 | (rate_idx - 7));
            }
            if (rate_idx >= 15 && rate_idx <= 22) {
                return static_cast<uint8_t>(0xC0 | (rate_idx - 15));
            }
            if (rate_idx >= 23 && rate_idx <= 30) {
                return static_cast<uint8_t>(0xD0 | (rate_idx - 23));
            }
            if (rate_idx >= 31 && rate_idx <= 38) {
                return static_cast<uint8_t>(0xE0 | (rate_idx - 31));
            }
            if (rate_idx >= 39 && rate_idx <= 46) {
                return static_cast<uint8_t>(0xF0 | (rate_idx - 39));
            }

            throw std::invalid_argument("rate_idx 超出范围：使用 0、1-46，或 0x80-0xFF 原始 PHY 速率编码");
        }

        /**
         * @brief 信道号转中心频率 (MHz)
         *
         * @param channel 信道号（2.4G: 1-14, 5G: 36-165）
         * @return 中心频率（MHz）
         */
        uint32_t channel_to_freq(uint8_t channel)
        {
            if (channel >= 1 && channel <= 13) {
                return 2412 + (channel - 1) * 5;
            } else if (channel == 14) {
                return 2484;
            } else if (channel >= 36 && channel <= 165) {
                return 5000 + (channel * 5);
            } else {
                return 2412; /* 默认信道 1 */
            }
        }

        /**
         * @brief 构建带 Radiotap 头部的帧数据
         *
         * 在原始 802.11 帧前添加 radiotap 元数据头。
         *
         * @param frame 原始帧数据指针
         * @param frame_len 帧长度
         * @param phy_rate 物理层速率
         * @param rssi RSSI 值 (dBm)
         * @param channel 信道号
         * @param out_buf 输出缓冲区
         * @param buf_cap 缓冲区容量
         * @return 总长度（radiotap 头 + 帧），失败返回负值
         */
        int build_radiotap_frame(const uint8_t *frame, int frame_len, uint8_t phy_rate,
                                 int8_t rssi, uint8_t channel, uint8_t *out_buf,
                                 int buf_cap)
        {
            /* 计算信道频率和标志 */
            uint32_t freq       = channel_to_freq(channel);
            uint16_t chan_flags = static_cast<uint16_t>(
                (channel <= 14) ? (static_cast<uint16_t>(RadiotapChanFlags::GHZ) |
                                   static_cast<uint16_t>(RadiotapChanFlags::CCK))
                                : (static_cast<uint16_t>(RadiotapChanFlags::FIVE_GHZ) |
                                   static_cast<uint16_t>(RadiotapChanFlags::OFDM)));
            const uint8_t phy_mode = static_cast<uint8_t>((phy_rate >> 6) & 0x03u);
            const bool phy_valid   = phy_rate != 0 || channel <= 14;
            const bool is_ht       = phy_valid && phy_mode == 3;
            const bool is_40mhz    = is_ht && ((phy_rate >> 5) & 0x01u) != 0;
            const bool short_gi    = is_ht && ((phy_rate >> 4) & 0x01u) != 0;
            bool decoded_ht        = false;
            bool decoded_40        = false;
            bool decoded_sgi       = false;
            const uint8_t rate_mbps =
                phy_valid ? decode_phy_rate(phy_rate, &decoded_ht, &decoded_40, &decoded_sgi)
                          : 0;
            (void)decoded_ht;
            (void)decoded_40;
            (void)decoded_sgi;

            uint32_t present = static_cast<uint32_t>(RadiotapPresence::FLAGS) |
                               static_cast<uint32_t>(RadiotapPresence::CHANNEL) |
                               static_cast<uint32_t>(RadiotapPresence::DBM_ANTSIGNAL);
            if (is_ht) {
                present |= static_cast<uint32_t>(RadiotapPresence::MCS);
            } else if (rate_mbps > 0) {
                present |= static_cast<uint32_t>(RadiotapPresence::RATE);
            }

            uint8_t rt_buf[64] = {};
            rt_buf[0]          = 0;
            rt_buf[1]          = 0;
            write_le_u32(rt_buf + 4, present);
            size_t offset = 8;

            /* FLAGS */
            rt_buf[offset++] = 0;

            /* RATE is in 500-kbps units and is only present for legacy PHY. */
            if (!is_ht && rate_mbps > 0) {
                rt_buf[offset++] = static_cast<uint8_t>(rate_mbps * 2);
            }

            /* CHANNEL is aligned to 2 bytes. */
            if (offset & 1u) {
                ++offset;
            }
            write_le_u16(rt_buf + offset, static_cast<uint16_t>(freq));
            write_le_u16(rt_buf + offset + 2, chan_flags);
            offset += 4;

            /* DBM_ANTSIGNAL */
            rt_buf[offset++] = static_cast<uint8_t>(rssi);

            if (is_ht) {
                /* MCS: known flags, flags, MCS index. */
                rt_buf[offset++] = IEEE80211_RADIOTAP_MCS_HAVE_BW |
                                   IEEE80211_RADIOTAP_MCS_HAVE_MCS |
                                   IEEE80211_RADIOTAP_MCS_HAVE_GI |
                                   IEEE80211_RADIOTAP_MCS_HAVE_FMT;
                rt_buf[offset++] = static_cast<uint8_t>(
                    (is_40mhz ? IEEE80211_RADIOTAP_MCS_BW_40
                              : IEEE80211_RADIOTAP_MCS_BW_20) |
                    (short_gi ? IEEE80211_RADIOTAP_MCS_SGI : 0) |
                    (((phy_rate >> 3) & 0x01u) ? IEEE80211_RADIOTAP_MCS_FMT_GF : 0));
                rt_buf[offset++] = static_cast<uint8_t>(phy_rate & 0x07u);
            }

            const uint16_t rt_len = static_cast<uint16_t>(offset);
            write_le_u16(rt_buf + 2, rt_len);

            int total = static_cast<int>(rt_len) + frame_len;
            if (total > buf_cap) {
                return -1; /* 缓冲区不足 */
            }

            std::memcpy(out_buf, rt_buf, rt_len);
            std::memcpy(out_buf + rt_len, frame, static_cast<size_t>(frame_len));
            return total;
        }

    } // anonymous namespace

    /* ==================== 构造/析构/移动 ==================== */

    MonitorSession::MonitorSession(UsbTransport &usb,
                                   std::shared_ptr<spdlog::logger> logger)
        : usb_(usb), logger_(std::move(logger)), running_(false), rx_buf_(nullptr),
          rx_buf_size_(0), rx_cb_(nullptr), cb_user_data_(nullptr),
          pcap_file_(nullptr), channel_(6)
    {
        if (!logger_) {
            throw std::invalid_argument("MonitorSession: logger 不能为空");
        }

        logger_->info("[MONITOR] 监控会话已创建");
    }

    MonitorSession::~MonitorSession()
    {
        logger_->info("[MONITOR] 销毁监控会话");

        /* RAII: 自动停止并清理 */
        try {
            stop();
            close_pcap();
            free_rx_buffer();
        } catch (...) {
            logger_->error("[MONITOR] 析构时发生异常（已忽略）");
        }
    }

    /* ==================== 监控控制 ==================== */

    void MonitorSession::start()
    {
        if (running_.load()) {
            logger_->warn("[MONITOR] 监控已在运行中，忽略重复调用");
            return;
        }

        logger_->info("[MONITOR] 启动监控模式...");

        try {
            /* 步骤 1: 配置混杂模式 */
            configure_monitor_mode();

            /* 步骤 2: 配置 HCI RX 聚合 */
            configure_hci_rx_aggr();

            /* 步骤 3: 分配 RX 缓冲区 */
            allocate_rx_buffer();

            /* 步骤 4: 先置运行标志再提交 URB。
             * 若先 arm 再置位，URB 可能在标志置位前完成，完成回调会按停止
             * 路径释放 transfer，造成首包丢失甚至 URB 被拆掉。 */
            running_.store(true);
            arm_rx_transfer();

            /* 初始化统计信息；状态读取可能与接收线程并发，必须使用同一把锁。 */
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                stats_.reset();
                stats_.start_time_ms = current_time_ms();
            }

            logger_->info("[MONITOR] ✅ 监控模式已启动");
            logger_->info("[MONITOR] RX 缓冲区大小: {} bytes", rx_buf_size_);

        } catch (const UsbException &e) {
            logger_->error("[MONITOR] 启动失败: {}", e.what());
            running_.store(false);
            retire_rx_transfer();
            free_rx_buffer();
            throw;
        } catch (...) {
            running_.store(false);
            retire_rx_transfer();
            free_rx_buffer();
            throw;
        }
    }

    void MonitorSession::stop()
    {
        const bool was_running = running_.exchange(false);
        if (!was_running && !rx_xfer_ && !rx_buf_) {
            return; /* 幂等操作 */
        }

        logger_->info("[MONITOR] 停止监控模式...");

        /* 先取消并等待 URB 完成回调退休，再释放缓冲区。
         * 否则 libusb 完成回调可能写入已释放的 rx_buf_（UAF）。 */
        if (!retire_rx_transfer()) {
            logger_->error("[MONITOR] RX URB 未能及时退休，已泄漏缓冲区以避免 UAF");
            logger_->info("[MONITOR] 监控模式已停止（异常路径）");
            return;
        }

        free_rx_buffer();

        logger_->info("[MONITOR] 监控模式已停止");
        logger_->info("[MONITOR] 最终统计:");
        print_stats();
    }

    void MonitorSession::request_stop() noexcept
    {
        // 只发布退出请求，不触碰 libusb 资源；资源回收必须留在拥有线程执行。
        running_.store(false, std::memory_order_release);
    }

    /* ==================== 帧接收 ==================== */

    void MonitorSession::set_callback(RxFrameCallback cb, void *user_data)
    {
        const bool has_callback = static_cast<bool>(cb);
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            rx_cb_        = std::move(cb);
            cb_user_data_ = user_data;
        }
        logger_->info("[MONITOR] RX 回调已设置{}", has_callback ? "" : "（已清除）");
    }

    int MonitorSession::process_rx_aggregate(const uint8_t *data, int len)
    {
        if (looks_like_rx_desc(data, len)) {
            return process_rx_mpdu(data, len);
        }

        int offset = 0;
        int total  = 0;

        while (offset + static_cast<int>(sizeof(HciRxAggrInfo)) +
                   SSV6XXX_RX_DESC_LEN <=
               len) {
            const auto *aggr = reinterpret_cast<const HciRxAggrInfo *>(data + offset);
            int jump         = static_cast<int>(aggr->jmp_mpdu_len);
            int mpdu_len     = jump - static_cast<int>(sizeof(HciRxAggrInfo));

            const int max_jump = static_cast<int>(sizeof(HciRxAggrInfo)) +
                                 SSV6XXX_RX_DESC_LEN + MAX_FRAME_SIZE +
                                 SSV6XXX_RX_PINFO_PAD;
            if (jump == 0 || jump > max_jump || offset + jump > len ||
                mpdu_len < SSV6XXX_RX_DESC_LEN) {
                break;
            }

            const uint8_t *mpdu = data + offset + sizeof(HciRxAggrInfo);
            if (!looks_like_rx_desc(mpdu, mpdu_len)) {
                break;
            }

            total += process_rx_mpdu(data + offset + sizeof(HciRxAggrInfo), mpdu_len);
            offset += jump;
        }

        if (total > 0) {
            return total;
        }

        return process_rx_mpdu(data, len);
    }

    int MonitorSession::process_rx_mpdu(const uint8_t *data, int len)
    {
        const auto record_rx_error = [this]() {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.rx_errors;
        };
        if (!data || len < SSV6XXX_RX_DESC_LEN) {
            record_rx_error();
            return -EINVAL;
        }

        /* 解析 RX 描述符 */
        RxDescView rxd;
        if (!parse_rx_desc_view(data, len, &rxd)) {
            record_rx_error();
            return -EINVAL;
        }

        if (rxd.len > static_cast<uint16_t>(MAX_FRAME_SIZE + 400)) {
            record_rx_error();
            return -EPROTO;
        }

        if (rxd.c_type == 6) {
            /* SOC_EVT_FW_NOTIFY (28) is a periodic firmware heartbeat/status update.
             * Do not flood verbose capture logs with it. */
            if (rxd.h_event != 28 && logger_->should_log(spdlog::level::debug)) {
                logger_->debug("[MONITOR] host event dropped: h_event={}",
                               static_cast<int>(rxd.h_event));
            }
            return 0;
        }

        /* Kernel monitor path: skb_pull(rx_desc_len=80), skb_trim(rx_pinfo_pad=4). */
        int frame_offset = SSV6XXX_RX_HW_DESC_LEN;
        int packet_len   = std::min(len, static_cast<int>(rxd.len));
        int frame_len    = packet_len - SSV6XXX_RX_HW_DESC_LEN - SSV6XXX_RX_PINFO_PAD;

        if (frame_len <= 0 || frame_len > MAX_FRAME_SIZE) {
            record_rx_error();
            return -EINVAL;
        }

        if (frame_offset + frame_len > len) {
            frame_len = len - frame_offset;
        }

        if (frame_len < 2) {
            record_rx_error();
            return -EINVAL;
        }

        /* 提取帧数据 */
        const uint8_t *frame = data + frame_offset;
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.rx_packets;
            stats_.rx_bytes += static_cast<uint64_t>(frame_len);
            stats_.last_rx_time_ms = current_time_ms();
        }

        /* 解析 PHY 信息 */
        uint8_t raw_channel    = rxd.channel;
        uint8_t raw_rssi       = rxd.phy_rssi;
        uint8_t phy_rate       = rxd.phy_rate;
        int8_t rssi            = decode_rssi_dbm(raw_rssi);
        uint8_t rx_channel     = select_rx_channel(raw_channel, channel_);
        bool is_ht             = false;
        bool is_40mhz          = false;
        bool short_gi          = false;
        const bool phy_valid   = phy_rate != 0 || rx_channel <= 14;
        uint8_t rate_mbps      = phy_valid
                                     ? decode_phy_rate(phy_rate, &is_ht, &is_40mhz, &short_gi)
                                     : 0;
        const uint8_t phy_mode = static_cast<uint8_t>((phy_rate >> 6) & 0x03u);
        const int mcs          = (phy_valid && phy_mode == 3)
                                     ? static_cast<int>(phy_rate & 0x07u)
                                     : -1;

        if (logger_->should_log(spdlog::level::debug)) {
            logger_->debug(
                "[MONITOR] RX meta: raw_ch={} sel_ch={} raw_rssi={} rssi={} "
                "phy_rate=0x{:02x} mode={} rate={}Mbps mcs={} bw={} gi={} "
                "snr={} stbc={} fec={} nss={} l_rate={} "
                "w1=0x{:08x} w4=0x{:08x} w5=0x{:08x} w6=0x{:08x} w7=0x{:08x}",
                static_cast<int>(raw_channel), static_cast<int>(rx_channel),
                static_cast<int>(raw_rssi), static_cast<int>(rssi),
                static_cast<unsigned>(phy_rate), static_cast<unsigned>(phy_mode),
                static_cast<unsigned>(rate_mbps), mcs, is_40mhz ? 40 : 20,
                short_gi ? "SGI" : "LGI", static_cast<unsigned>(rxd.phy_snr),
                static_cast<unsigned>(rxd.phy_stbc), static_cast<unsigned>(rxd.phy_fec),
                static_cast<unsigned>(rxd.phy_n_ess), static_cast<unsigned>(rxd.phy_l_rate),
                static_cast<unsigned>(rxd.word1), static_cast<unsigned>(rxd.word4),
                static_cast<unsigned>(rxd.word5), static_cast<unsigned>(rxd.word6),
                static_cast<unsigned>(rxd.word7));
        }

        FrameView fv;
        if (parse_frame_view(frame, frame_len, &fv) &&
            logger_->should_log(spdlog::level::debug)) {
            logger_->debug("[MONITOR] 802.11 {} subtype={} fc=0x{:04x} "
                           "addr1={} addr2={} addr3={}",
                           frame_type_name(fv.type), mgmt_subtype_name(fv.subtype),
                           static_cast<unsigned>(fv.frame_control),
                           mac_to_string(fv.addr1), mac_to_string(fv.addr2),
                           mac_to_string(fv.addr3));
        }

        /* 写入 PCap 文件 */
        write_pcap_packet(frame, frame_len, phy_rate, rssi, rx_channel);

        /* 调用用户回调；保留描述符中的 PHY 信息，避免上层只能依赖 RSSI。 */
        RxFrameCallback callback;
        void *callback_user_data = nullptr;
        {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            callback           = rx_cb_;
            callback_user_data = cb_user_data_;
        }
        if (callback) {
            RxFrameView view;
            view.data                       = frame;
            view.length                     = frame_len;
            view.metadata.valid_fields      = kRxMetadataChannel | kRxMetadataFrequency;
            view.metadata.channel           = rx_channel;
            view.metadata.frequency_mhz     = channel_to_freq(rx_channel);
            view.metadata.phy_mode          = phy_mode;
            view.metadata.phy_rate          = phy_rate;
            view.metadata.rate_mbps         = rate_mbps;
            view.metadata.rssi_dbm          = rssi;
            view.metadata.snr               = rxd.phy_snr;
            view.metadata.mcs               = static_cast<int8_t>(mcs);
            view.metadata.n_ess             = rxd.phy_n_ess;
            view.metadata.bandwidth_mhz     = is_40mhz ? 40 : 20;
            view.metadata.phy_valid         = phy_valid;
            view.metadata.short_gi          = short_gi;
            view.metadata.stbc              = rxd.phy_stbc != 0;
            view.metadata.fec               = rxd.phy_fec != 0;
            view.metadata.phy_packet_length = rxd.phy_packet_length;
            view.metadata.long_rate         = rxd.phy_l_rate;
            view.metadata.frequency_offset  = rxd.phy_frequency_offset;
            view.metadata.rx_timestamp_1    = rxd.rx_timestamp_1;
            view.metadata.rx_timestamp_2    = rxd.rx_timestamp_2;
            view.metadata.sequence          = rxd.sequence;
            if (rxd.rx_timestamp_1 != 0 || rxd.rx_timestamp_2 != 0) {
                view.metadata.valid_fields |= kRxMetadataTiming;
            }
            if (phy_valid) {
                view.metadata.valid_fields |= kRxMetadataPhy | kRxMetadataSnr | kRxMetadataBandwidth;
            }
            if (rate_mbps > 0) {
                view.metadata.valid_fields |= kRxMetadataRate;
            }
            if (mcs >= 0) {
                view.metadata.valid_fields |= kRxMetadataMcs;
            }
            if (raw_rssi != 0) {
                view.metadata.valid_fields |= kRxMetadataRssi;
            }
            callback(view, callback_user_data);
        }

        return frame_len;
    }

    int MonitorSession::process_rx_data(const uint8_t *data, int len)
    {
        return process_rx_aggregate(data, len);
    }

    int MonitorSession::poll(int timeout_ms)
    {
        if (!running_.load()) {
            return -EBADF;
        }

        /* 调用 UsbTransport 的 handle_events() */
        try {
            usb_.handle_events(timeout_ms);
            if (running_.load() && !rx_xfer_) {
                logger_->warn("[MONITOR] RX URB 丢失，正在重新提交...");
                usb_.clear_halt(0x84);
                usleep(5000);
                arm_rx_transfer();
            }
            return 0; /* 返回成功，实际数据包数由回调统计 */
        } catch (const UsbException &e) {
            logger_->error("[MONITOR] poll 错误: {}", e.what());
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.rx_errors;
            return -EIO;
        }
    }

    void LIBUSB_CALL MonitorSession::rx_transfer_cb(libusb_transfer *xfer)
    {
        if (!xfer || !xfer->user_data) {
            return;
        }

        auto *self = static_cast<MonitorSession *>(xfer->user_data);
        self->handle_rx_transfer(xfer);
    }

    void MonitorSession::arm_rx_transfer()
    {
        if (rx_xfer_ || !rx_buf_ || rx_buf_size_ <= 0) {
            return;
        }

        rx_xfer_ = usb_.submit_async_rx(rx_buf_, rx_buf_size_, rx_transfer_cb, this);
        logger_->info("[MONITOR] 异步 RX URB 已提交");
    }

    bool MonitorSession::retire_rx_transfer()
    {
        if (!rx_xfer_) {
            return true;
        }

        /* 先发起取消，再泵事件直到完成回调摘除/释放该 URB。
         * cancel_async_transfer 内部只做一次短超时 handle_events，不能
         * 保证回调已经执行；这里必须继续等待，否则 free_rx_buffer 会 UAF。 */
        libusb_transfer *const xfer = rx_xfer_;
        usb_.cancel_async_transfer(xfer);

        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
        while (rx_xfer_ != nullptr && std::chrono::steady_clock::now() < deadline) {
            try {
                usb_.handle_events(10);
            } catch (const UsbException &e) {
                logger_->warn("[MONITOR] 等待 RX URB 取消时 USB 错误: {}",
                              e.what());
                break;
            }
        }

        if (rx_xfer_ != nullptr) {
            /* 回调仍未触发：放弃跟踪并泄漏缓冲区，避免完成回调访问已释放内存。
             * transfer 指针一并丢弃；libusb 保证 cancel 后最终会调用回调。 */
            logger_->error("[MONITOR] RX URB 取消超时，泄漏缓冲区以避免 UAF");
            rx_xfer_     = nullptr;
            rx_buf_      = nullptr;
            rx_buf_size_ = 0;
            return false;
        }

        return true;
    }

    void MonitorSession::handle_rx_transfer(libusb_transfer *xfer)
    {
        if (!xfer) {
            return;
        }

        if (xfer->status == LIBUSB_TRANSFER_COMPLETED) {
            if (xfer->actual_length > 0) {
                process_rx_data(xfer->buffer, xfer->actual_length);
            }

            if (running_.load()) {
                int ret = libusb_submit_transfer(xfer);
                if (ret < 0) {
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    ++stats_.rx_errors;
                    ++stats_.rx_dropped;
                    if (rx_xfer_ == xfer) {
                        rx_xfer_ = nullptr;
                    }
                    logger_->error("[MONITOR] 重新提交 RX URB 失败: {}",
                                   libusb_error_name(ret));
                    /* 重新提交失败必须释放 transfer，否则泄漏。 */
                    libusb_free_transfer(xfer);
                }
                return;
            }
        } else if (xfer->status != LIBUSB_TRANSFER_CANCELLED) {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.rx_errors;
            logger_->warn("[MONITOR] RX URB 异常完成: status={}",
                          static_cast<int>(xfer->status));
        }

        /* 先摘除跟踪再释放，避免与 retire/stop 竞态时对已 free 指针比较。 */
        if (rx_xfer_ == xfer) {
            rx_xfer_ = nullptr;
        }
        libusb_free_transfer(xfer);
    }

    void MonitorSession::rx_loop(int timeout_ms)
    {
        logger_->info("[MONITOR] 进入阻塞式接收循环 (timeout={}ms)...", timeout_ms);

        while (running_.load()) {
            int ret = poll(timeout_ms);
            if (ret < 0 && ret != -ETIMEDOUT) {
                logger_->error("[MONITOR] 接收循环错误: {}, 退出", ret);
                break;
            }
        }

        logger_->info("[MONITOR] 接收循环已退出");
    }

    /* ==================== 帧注入 ==================== */

    void MonitorSession::inject_frame(const uint8_t *frame, int frame_len,
                                      int rate_idx)
    {
        if (!frame || frame_len < 10 || frame_len > MAX_FRAME_SIZE) {
            throw std::invalid_argument("inject_frame: 参数无效 (frame=" +
                                        std::string(frame ? "valid" : "null") +
                                        ", len=" + std::to_string(frame_len) + ")");
        }

        logger_->debug("[MONITOR] 注入帧: {} bytes, rate_idx={}", frame_len,
                       rate_idx);

        /* 分配 TX 缓冲区 */
        uint8_t tx_buf[TX_BUF_SIZE];
        int total_len = SSV6XXX_TX_DESC_LEN + frame_len;

        /* 构建 TX 描述符 */
        auto *tx_desc = reinterpret_cast<Ssv6200TxDesc *>(tx_buf);
        build_tx_descriptor(tx_desc, frame, frame_len, rate_idx);

        /* 复制帧数据到 TX 描述符后 */
        std::memcpy(tx_buf + SSV6XXX_TX_DESC_LEN, frame,
                    static_cast<size_t>(frame_len));

        /* 通过 EP3 发送。
         * rate_idx != 0 时需要下发固件固定速率命令并等待 RX 端点上的 HOST_EVENT。
         * 若监控 URB 仍占用 EP4，同步 bulk IN 抢不到响应（事件会被监控路径丢弃），
         * 固定速率注入会在监控运行时失败；因此先退休 URB，发完再重新提交。 */
        const bool need_fixed_rate = (rate_idx != 0);
        const bool rearm_after_rx  = need_fixed_rate && (rx_xfer_ != nullptr);
        if (rearm_after_rx) {
            if (!retire_rx_transfer()) {
                throw UsbException("注入前停止 RX URB 失败");
            }
        }

        try {
            if (need_fixed_rate) {
                const uint8_t encoded_rate = encode_tx_rate(rate_idx, channel_ > 14);
                const uint32_t fixed_rate  = encoded_rate;
                const int rc_ret           = usb_.send_host_cmd(
                    SSV6XXX_HOST_CMD_RC_OPS, SSV6XXX_RC_CMD_FIXED_RATE,
                    &fixed_rate, sizeof(fixed_rate), true);
                if (rc_ret < 0) {
                    throw UsbException("下发固定 TX 速率到固件失败");
                }
                logger_->debug(
                    "[MONITOR] 固件固定速率已设置: rate_idx={} phy_rate=0x{:02x}",
                    rate_idx, static_cast<unsigned>(encoded_rate));
            }

            usb_.bulk_write(SSV_EP_TX, tx_buf, total_len);
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.tx_packets;
                stats_.tx_bytes += static_cast<uint64_t>(frame_len);
            }
            logger_->debug("[MONITOR] 帧注入成功");
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.tx_errors;
            }
            if (rearm_after_rx && running_.load()) {
                arm_rx_transfer();
            }
            throw;
        }

        if (rearm_after_rx && running_.load()) {
            arm_rx_transfer();
        }
    }

    /* ==================== PCap 输出 ==================== */

    void MonitorSession::open_pcap(const std::string &filepath)
    {
        if (pcap_file_) {
            close_pcap(); /* 先关闭已有的文件 */
        }

        logger_->info("[MONITOR] 打开 PCap 文件: {}", filepath);

        pcap_file_ = fopen(filepath.c_str(), "wb");
        if (!pcap_file_) {
            throw std::runtime_error("无法创建 PCap 文件: " + filepath + " (" +
                                     std::strerror(errno) + ")");
        }

        pcap_path_ = filepath;

        /* 写入全局头部 */
        write_pcap_global_header();

        logger_->info("[MONITOR] PCap 文件已打开 (radiotap 格式)");
    }

    void MonitorSession::close_pcap()
    {
        if (!pcap_file_) {
            return; /* 幂等操作 */
        }

        logger_->info("[MONITOR] 关闭 PCap 文件: {}", pcap_path_);

        fflush(pcap_file_);
        fclose(pcap_file_);
        pcap_file_ = nullptr;
        pcap_path_.clear();

        logger_->info("[MONITOR] PCap 文件已关闭");
    }

    /* ==================== 统计信息 ==================== */

    MonitorStats MonitorSession::get_stats() const noexcept
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        return stats_;
    }

    void MonitorSession::reset_stats()
    {
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            stats_.reset();
            stats_.start_time_ms = current_time_ms();
        }
        logger_->info("[MONITOR] 统计计数器已重置");
    }

    void MonitorSession::print_stats() const
    {
        const MonitorStats stats = get_stats();
        double duration_sec      = 0.0;
        if (stats.start_time_ms > 0) {
            uint64_t now = current_time_ms();
            duration_sec = static_cast<double>(now - stats.start_time_ms) / 1000.0;
        }

        logger_->info("========================================");
        logger_->info("  [MONITOR] 统计摘要");
        logger_->info("========================================");
        logger_->info("  RX: {} packets ({} bytes), {} errors, {} dropped",
                      stats.rx_packets, stats.rx_bytes, stats.rx_errors,
                      stats.rx_dropped);
        logger_->info("  TX: {} packets ({} bytes), {} errors", stats.tx_packets,
                      stats.tx_bytes, stats.tx_errors);
        logger_->info("  吞吐量: {:.2f} KB/s", stats.rx_throughput_bps() / 1024.0);
        logger_->info("  运行时间: {:.1f} 秒", duration_sec);

        /* Mirror the kernel driver's `mib rx` essentials.  These counters make a
         * zero-packet capture actionable: FCS counters identify RF/PHY reception,
         * while HCI status identifies a routing or USB delivery failure. */
        uint32_t fcs_ok = 0, fcs_err = 0, alc_fail = 0, miss = 0;
        uint32_t hci_len = 0, host_events = 0, flow_data = 0;
        uint32_t phy_rx_en = 0, phy_status = 0, phy_fifo = 0;
        if (usb_.read_reg(ADR_MRX_FCS_SUCC, &fcs_ok) == 0 &&
            usb_.read_reg(ADR_MRX_FCS_ERR, &fcs_err) == 0 &&
            usb_.read_reg(ADR_MRX_ALC_FAIL, &alc_fail) == 0 &&
            usb_.read_reg(ADR_MRX_MISS, &miss) == 0 &&
            usb_.read_reg(ADR_RX_PACKET_LENGTH_STATUS, &hci_len) == 0 &&
            usb_.read_reg(ADR_RX_HOST_EVENT_COUNT, &host_events) == 0 &&
            usb_.read_reg(ADR_RX_FLOW_DATA, &flow_data) == 0 &&
            usb_.read_reg(ADR_WIFI_PHY_COMMON_RX_EN_CNT_REG, &phy_rx_en) == 0 &&
            usb_.read_reg(ADR_WIFI_PHY_COMMON_TOP_STATUS_RO, &phy_status) == 0 &&
            usb_.read_reg(ADR_WIFI_PHY_COMMON_MAC_IF_CNT_RO, &phy_fifo) == 0) {
            logger_->info("  HW RX: FCS_OK={} FCS_ERR={} ALC_FAIL={} MISS={}",
                          fcs_ok & 0xffffu, fcs_err & 0xffffu, alc_fail & 0xffffu,
                          miss & 0xffffu);
            logger_->info("  HCI RX: LEN=0x{:08X} EVENTS={} FLOW=0x{:08X}", hci_len,
                          host_events, flow_data);
            logger_->info(
                "  PHY RX: EN_CNT={} RX_EN={} TRX_SYNC={} FIFO_FULL={} STATUS=0x{:08X}",
                phy_rx_en & 0xffffu, (phy_status >> 4) & 1u, (phy_status >> 28) & 1u,
                phy_fifo & 0xffffu, phy_status);
        }
        logger_->info("========================================");
    }

    /* ==================== 高级配置 ==================== */

    /* set_channel() 和 get_channel() 已在头文件中内联实现 */

    /* ==================== 私有辅助方法 ==================== */

    void MonitorSession::configure_monitor_mode()
    {
        logger_->info("[MONITOR] 配置混杂模式...");

        /* MonitorSession may be restarted after firmware changed the decision table.
         * Reapply the same setting as kernel ssv6006c_set_mrx_mode(). */
        for (int i = 0; i < 9; ++i) {
            if (usb_.write_reg(ADR_MRX_FLT_EN0 + static_cast<uint32_t>(i * 4), 0) < 0)
                throw UsbException("配置 MRX 过滤器失败");
        }
        for (int i = 0; i < 16; ++i) {
            if (usb_.write_reg(ADR_MRX_FLT_TB0 + static_cast<uint32_t>(i * 4),
                               0x0000FFF0) < 0)
                throw UsbException("配置 MRX 决策表失败");
        }
        if (usb_.write_reg(ADR_MRX_FLT_TB13, MRX_MODE_PROMISCUOUS) < 0)
            throw UsbException("启用 MRX 混杂模式失败");

        int ret = usb_.send_host_cmd(SSV6XXX_HOST_CMD_MRX_MODE,
                                     SSV6XXX_MRX_PROMISCUOUS, nullptr, 0, false);
        if (ret < 0)
            throw UsbException("通知固件进入混杂模式失败");

        /* Kernel smartlink/monitor routing bypasses firmware CPU and crypto so HCI
         * receives original 802.11 frames instead of converted Ethernet payloads. */
        if (usb_.write_reg(ADR_RX_FLOW_DATA, RX_HCI) < 0 ||
            usb_.write_reg(ADR_RX_FLOW_MNG, RX_HCI) < 0 ||
            usb_.write_reg(ADR_RX_FLOW_CTRL, RX_HCI) < 0)
            throw UsbException("配置监控 RX 直通流失败");

        logger_->info("[MONITOR] 混杂模式配置完成");
    }

    void MonitorSession::configure_hci_rx_aggr()
    {
        logger_->info("[MONITOR] 配置 HCI RX 聚合...");

        /* TODO: 配置 HCI RX 聚合参数以减少中断开销
         * 例如：
         * usb_.write_reg(ADR_HCI_RX_AGGR_CFG, aggr_value);
         */

        logger_->info("[MONITOR] HCI RX 聚合配置完成");
    }

    void MonitorSession::allocate_rx_buffer()
    {
        free_rx_buffer(); /* 先释放已有缓冲区 */

        rx_buf_size_ = MON_RX_BUF_SIZE;
        rx_buf_      = new (std::nothrow) uint8_t[rx_buf_size_];

        if (!rx_buf_) {
            throw std::bad_alloc();
        }

        logger_->debug("[MONITOR] 已分配 RX 缓冲区: {} bytes", rx_buf_size_);
    }

    void MonitorSession::free_rx_buffer()
    {
        if (rx_buf_) {
            delete[] rx_buf_;
            rx_buf_      = nullptr;
            rx_buf_size_ = 0;
        }
    }

    void MonitorSession::write_pcap_global_header()
    {
        if (!pcap_file_)
            return;

        struct __attribute__((packed)) {
            uint32_t magic;
            uint16_t version_major;
            uint16_t version_minor;
            int32_t thiszone;
            uint32_t sigfigs;
            uint32_t snaplen;
            uint32_t network;
        } header;

        header.magic         = PCAP_MAGIC;
        header.version_major = 2;
        header.version_minor = 4;
        header.thiszone      = 0;
        header.sigfigs       = 0;
        header.snaplen       = MAX_FRAME_SIZE + 64;
        header.network       = PCAP_LINKTYPE_IEEE802_11_RADIOTAP;

        fwrite(&header, sizeof(header), 1, pcap_file_);
        fflush(pcap_file_);
    }

    void MonitorSession::write_pcap_packet(const uint8_t *frame, int len,
                                           uint8_t rate, int8_t rssi,
                                           uint16_t freq)
    {
        if (!pcap_file_ || !frame || len <= 0)
            return;

        /* 构建带 radiotap 头部的帧 */
        uint8_t rt_buf[MAX_FRAME_SIZE + 64];
        int rt_len = build_radiotap_frame(frame, len, rate, rssi, channel_, rt_buf,
                                          sizeof(rt_buf));
        if (rt_len < 0) {
            logger_->warn("[MONITOR] radiotap 帧构建失败");
            return;
        }

        /* 写入 PCap 包头部 */
        struct __attribute__((packed)) {
            uint32_t ts_sec;
            uint32_t ts_usec;
            uint32_t incl_len;
            uint32_t orig_len;
        } phdr;

        timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        phdr.ts_sec   = static_cast<uint32_t>(ts.tv_sec);
        phdr.ts_usec  = static_cast<uint32_t>(ts.tv_nsec / 1000);
        phdr.incl_len = static_cast<uint32_t>(rt_len);
        phdr.orig_len = static_cast<uint32_t>(rt_len);

        fwrite(&phdr, sizeof(phdr), 1, pcap_file_);
        fwrite(rt_buf, 1, static_cast<size_t>(rt_len), pcap_file_);
        fflush(pcap_file_);
    }

    void MonitorSession::build_tx_descriptor(Ssv6200TxDesc *desc,
                                             const uint8_t *frame, int frame_len,
                                             int rate_idx)
    {
        /* Use raw dwords here because the legacy Ssv6200TxDesc bitfield names do
         * not match the 19Q3 ssv6006_tx_desc ABI after word 4. */
        std::memset(desc, 0, SSV6XXX_TX_DESC_LEN);
        auto *dw = reinterpret_cast<uint32_t *>(desc);

        const uint16_t fc     = frame_len >= 2 ? read_le_u16(frame) : 0;
        const uint8_t type    = static_cast<uint8_t>((fc >> 2) & 0x3u);
        const uint8_t subtype = static_cast<uint8_t>((fc >> 4) & 0xFu);
        const bool qos        = type == 2 && (subtype & 0x8u);
        const bool use_4addr  = type == 2 && (fc & 0x0300u) == 0x0300u;
        const bool more_frag  = (fc & 0x0400u) != 0;
        const bool multicast  = frame_len >= 10 && (frame[4] & 0x01u);
        const bool is_mgmt    = type == 0;
        const uint8_t txq     = is_mgmt ? 4 : 0;

        uint8_t hdr_len = 24;
        if (type == 1) {
            /* BAR/BA/PS-Poll/RTS are 16 bytes; CTS/ACK are 10 bytes. */
            hdr_len = (subtype == 12 || subtype == 13) ? 10 : 16;
        } else if (type == 2) {
            hdr_len = static_cast<uint8_t>(24 + (use_4addr ? 6 : 0) + (qos ? 2 : 0));
        }

        const uint8_t rate      = encode_tx_rate(rate_idx, channel_ > 14);
        const bool is_ht        = ((rate >> 6) & 0x03u) == 3;
        const uint8_t ctrl_rate = is_ht ? static_cast<uint8_t>(channel_ > 14 ? 0x80 : 0x00)
                                        : rate;

        /* DW0: total skb length, M2_TXREQ, and frame-derived flags.  The
         * descriptor's `ht` bit means an 802.11 HT-Control field is present in
         * the MAC header; it is not the PHY modulation selector.  Ordinary
         * QoS Data frames used for MCS testing do not carry that field. */
        dw[0] = static_cast<uint32_t>(SSV6XXX_TX_DESC_LEN + frame_len) | (2u << 16) |
                (1u << 19) | (static_cast<uint32_t>(qos) << 20) |
                (static_cast<uint32_t>(use_4addr) << 22) |
                (static_cast<uint32_t>(more_frag) << 28) |
                (static_cast<uint32_t>((subtype >> 2) & 0x3u) << 29);

        /* Kernel flow: HWHCI -> CPU -> (crypto/MIC for data) -> TX EDCA queue. */
        uint32_t fcmd = static_cast<uint32_t>(0x06u + txq);
        fcmd          = (fcmd << 4) | M_ENG_CPU;
        if (!is_mgmt) {
            fcmd = (fcmd << 4) | M_ENG_ENCRYPT;
            fcmd = (fcmd << 4) | M_ENG_MIC;
        }
        dw[1] = (fcmd << 4) | M_ENG_HWHCI;

        /* DW2/DW3: header placement, destination type, unassociated WSID, queue. */
        dw[2] = static_cast<uint32_t>(SSV6XXX_TX_DESC_LEN) |
                (static_cast<uint32_t>(more_frag) << 8) |
                (static_cast<uint32_t>(!multicast) << 9) |
                (static_cast<uint32_t>(hdr_len) << 10);
        dw[3] = (0x0Fu << 19) | (static_cast<uint32_t>(txq) << 23);

        /* Kernel ABI WORD6/WORD7: report mode, then series-0 data/control rate. */
        dw[6] = (1u << 18); /* rate_rpt_mode = 1 */
        dw[7] = static_cast<uint32_t>(rate) |
                (static_cast<uint32_t>(ctrl_rate) << 8);
        dw[8] = (static_cast<uint32_t>(frame_len) & 0x0FFFu) | (0x0Fu << 12) |
                (1u << 20); /* 15 tries, last rate */

        logger_->debug(
            "[MONITOR] TX desc: public_rate_idx={} drate=0x{:02x} crate=0x{:02x} "
            "word7=0x{:08x} ht_control={} qos={}",
            rate_idx, static_cast<unsigned>(rate), static_cast<unsigned>(ctrl_rate),
            static_cast<unsigned>(dw[7]), static_cast<unsigned>((dw[0] >> 21) & 0x1u),
            static_cast<unsigned>(qos));
    }

    uint64_t MonitorSession::current_time_ms()
    {
        using namespace std::chrono;
        auto now = steady_clock::now().time_since_epoch();
        return static_cast<uint64_t>(duration_cast<milliseconds>(now).count());
    }

} // namespace ssv6xxx
