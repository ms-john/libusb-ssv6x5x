/*
 * ssv6xxx_chip_init.cpp - SSV6X5X 芯片初始化器实现 (C++14)
 *
 * 匹配内核驱动 ssv6xxx_init_hw() 的初始化流程：
 *   1. 芯片 ID 验证
 *   2. CPU 复位 + ON3 电源域 + USB ROM 就绪
 *   3. MAC 复位（禁用/启用 USB ACC）
 *   4. 硬件配置（HCI、MMU、TX/RX 阈值等）
 *   5. 固件下载到 SRAM + EP0 切换
 *   6. 固件验证
 *   7. PHY/RF 校准表加载 + PLL 初始化
 *   8. 信道设置（2.4GHz/5GHz）
 *   9. PHY 启用（最后一步）
 *
 * 源代码来源：
 *   - tu_ssv6xxx_init_hw() / tu_ssv6xxx_init_mac() (smac/init.c)
 *   - ssv6xxx_hci_load_firmware_request() (hci/ssv_hci.c)
 *   - ssv6006_turismoC_wifi_phy_reg.c (PHY 校准表)
 *   - ssv6006_turismoC_rf_reg.c (RF 校准表)
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <spdlog/spdlog.h>
#include <unistd.h>
#include <vector>

#include "ssv6xxx_chip_init.hpp"
#include "ssv6xxx_descriptors.hpp"
#include "ssv6xxx_embedded_firmware.hpp"
#include "ssv6xxx_regs.hpp"
#include "ssv6xxx_usb_transport.hpp"

namespace ssv6xxx
{

    /* ==================== 常量定义 ==================== */

    constexpr int CHECKSUM_BLOCK_SIZE    = 1024;
    constexpr uint32_t ADR_SRAM_MODE_CFG = 0xC0000128;
    constexpr int PLATFORM_CLK_CPU_BIT   = 24;

    /* TurismoC (SSV6006C) 寄存器地址 */
    constexpr uint32_t ADR_TU_PHY_ENABLE           = 0xCCB0E004;
    constexpr uint32_t ADR_WIFI_PHY_COMMON_SYS_REG = 0xCCB0E000;
    constexpr uint32_t ADR_MTX_TIME_TOUT           = MT_REG_CSR_BASE + 0xC0;

    /* EFUSE 控制器寄存器（对应内核 ssv6006c_read_efuse()）。 */
    constexpr uint32_t ADR_EFUSE_STATUS      = 0xC2000108;
    constexpr uint32_t ADR_EFUSE_STATUS2     = 0xC200010C;
    constexpr uint32_t ADR_EFUSE_RD_KICK     = 0xC2000114;
    constexpr uint32_t ADR_EFUSE_WDATA_0_0   = 0xC2000120;
    constexpr uint32_t ADR_EFUSE_WDATA_0_1   = 0xC2000124;
    constexpr uint32_t EFUSE_ITEM_XTAL       = 4;
    constexpr uint32_t EFUSE_ITEM_TX_POWER_1 = 5;
    constexpr uint32_t EFUSE_ITEM_TX_POWER_2 = 6;
    constexpr uint32_t EFUSE_ITEM_RATE_1     = 12;
    constexpr uint32_t EFUSE_ITEM_RATE_2     = 13;
    constexpr size_t EFUSE_MAP_BYTES         = 28; // (256 - chip identity 32) bits

    /* PHY 启用寄存器位 */
    constexpr uint32_t TU_PHY_MD_EN     = (1u << 0);
    constexpr uint32_t TU_PHY_RX_EN     = (1u << 1);
    constexpr uint32_t TU_PHY_TX_EN     = (1u << 2);
    constexpr uint32_t TU_PHY_11GN_EN   = (1u << 3);
    constexpr uint32_t TU_PHY_11B_EN    = (1u << 4);
    constexpr uint32_t TU_PHY_RXFIFO_EN = (1u << 5);
    constexpr uint32_t TU_PHY_TXFIFO_EN = (1u << 6);
    constexpr uint32_t TU_PHY_11BGN_EN  = (1u << 8);
    constexpr uint32_t TU_PHY_ALL_EN =
        (TU_PHY_MD_EN | TU_PHY_RX_EN | TU_PHY_TX_EN | TU_PHY_11GN_EN |
         TU_PHY_11B_EN | TU_PHY_RXFIFO_EN | TU_PHY_TXFIFO_EN | TU_PHY_11BGN_EN);

    /* 模式寄存器位 */
    constexpr int TU_MODE_MANUAL_SFT      = 2;
    constexpr uint32_t TU_MODE_MANUAL_MSK = (1u << 2);
    constexpr int TU_MODE_SFT             = 8;
    constexpr uint32_t TU_MODE_MSK        = (7u << 8);

    namespace
    {

        constexpr uint32_t RESET_N_CPUN10_BIT = (1u << 24);
        constexpr uint32_t CLK_EN_CPUN10_BIT  = (1u << 1);
        constexpr uint32_t N10_STANDBY_BIT    = (1u << 30);
        constexpr uint32_t ROM_IVB_VALUE      = 0x0004;
        constexpr uint32_t SKIP_START_ADDR    = 0x00010000;
        constexpr uint32_t SKIP_END_ADDR      = 0x00010B00;

        bool should_use_kernel_embedded_firmware(const std::string &firmware_path)
        {
            if (firmware_path.empty()) {
                return true;
            }

            static const char *kDefaultFwName = "ssv6x5x-sw.bin";
            if (firmware_path == kDefaultFwName) {
                return true;
            }

            const std::string default_rel = std::string("firmware/") + kDefaultFwName;
            if (firmware_path == default_rel) {
                return true;
            }

            if (firmware_path.size() >= std::strlen(kDefaultFwName) &&
                firmware_path.compare(firmware_path.size() - std::strlen(kDefaultFwName),
                                      std::strlen(kDefaultFwName), kDefaultFwName) == 0) {
                return true;
            }

            return false;
        }

/**
 * @brief 寄存器写入宏（带错误检查）
 */
#define REG_WRITE(addr, val)                                                    \
    do {                                                                        \
        int _r = usb_.write_reg((addr), (val));                                 \
        if (_r < 0) {                                                           \
            char _buf[128];                                                     \
            snprintf(_buf, sizeof(_buf), "[L%d]: write 0x%08X <- 0x%08X => %d", \
                     __LINE__, (unsigned)(addr), (unsigned)(val), _r);          \
            throw InitException(std::string(_buf));                             \
        }                                                                       \
    } while (0)

#define REG_SET_BITS(addr, set, clr)                                               \
    do {                                                                           \
        int _r = usb_.set_bits((addr), (set), (clr));                              \
        if (_r < 0) {                                                              \
            char _buf[128];                                                        \
            snprintf(_buf, sizeof(_buf), "[L%d]: set_bits 0x%08X => %d", __LINE__, \
                     (unsigned)(addr), _r);                                        \
            throw InitException(std::string(_buf));                                \
        }                                                                          \
    } while (0)

#define REG_READ(addr)                                                         \
    ({                                                                         \
        uint32_t _v;                                                           \
        int _r = usb_.read_reg((addr), &_v);                                   \
        if (_r < 0) {                                                          \
            char _buf[128];                                                    \
            snprintf(_buf, sizeof(_buf), "[L%d]: read 0x%08X => %d", __LINE__, \
                     (unsigned)(addr), _r);                                    \
            throw InitException(std::string(_buf));                            \
        }                                                                      \
        _v;                                                                    \
    })

        const char *bw_name(ChannelBandwidth bw)
        {
            switch (bw) {
                case ChannelBandwidth::HT40_MINUS:
                    return "HT40-";
                case ChannelBandwidth::HT40_PLUS:
                    return "HT40+";
                case ChannelBandwidth::HT20:
                default:
                    return "HT20";
            }
        }

        uint32_t finalize_fw_checksum(uint32_t checksum)
        {
            checksum =
                ((checksum >> 24) + (checksum >> 16) + (checksum >> 8) + checksum) & 0xFF;
            return checksum << 16;
        }

        void accumulate_fw_checksum(uint32_t &checksum, uint32_t sram_addr,
                                    const uint8_t *block_data)
        {
            const auto *fw_words             = reinterpret_cast<const uint32_t *>(block_data);
            constexpr size_t words_per_block = CHECKSUM_BLOCK_SIZE / sizeof(uint32_t);

            if (sram_addr >= SKIP_START_ADDR && sram_addr < SKIP_END_ADDR) {
                if (sram_addr + CHECKSUM_BLOCK_SIZE <= SKIP_END_ADDR) {
                    return;
                }

                const size_t start_word =
                    static_cast<size_t>((SKIP_END_ADDR - sram_addr) / sizeof(uint32_t));
                for (size_t i = start_word; i < words_per_block; ++i) {
                    checksum += fw_words[i];
                }
                return;
            }

            for (size_t i = 0; i < words_per_block; ++i) {
                checksum += fw_words[i];
            }
        }

        bool kernel_like_reset_cpu(UsbTransport &usb, bool *used_standby = nullptr)
        {
            uint32_t org_int_mask = 0;
            if (usb.read_reg(ADR_MASK_TYPMCU_INT_MAP, &org_int_mask) < 0) {
                return false;
            }

            uint32_t plat_clk = 0;
            if (usb.read_reg(ADR_PLATFORM_CLOCK_ENABLE, &plat_clk) < 0) {
                return false;
            }

            bool standby_ok = false;
            if (plat_clk & RESET_N_CPUN10_BIT) {
                if (usb.write_reg(ADR_MASK_TYPMCU_INT_MAP, 0xFFFFDFFF) < 0 ||
                    usb.write_reg(ADR_SYSCTRL_COMMAND, 0x0000000E) < 0) {
                    return false;
                }

                for (unsigned i = 0; i < 10; ++i) {
                    uint32_t dbg2 = 0;
                    if (usb.read_reg(ADR_N10_DBG2, &dbg2) < 0) {
                        return false;
                    }
                    if (dbg2 & N10_STANDBY_BIT) {
                        standby_ok = true;
                        break;
                    }
                    usleep(1000);
                }
            }

            const int reset_ret =
                usb.write_reg(ADR_PLATFORM_CLOCK_ENABLE, plat_clk & ~RESET_N_CPUN10_BIT);
            const int mask_ret = usb.write_reg(ADR_MASK_TYPMCU_INT_MAP, org_int_mask);
            if (used_standby) {
                *used_standby = standby_ok;
            }
            return (reset_ret >= 0 && mask_ret >= 0);
        }

        void kernel_like_jump_to_rom(UsbTransport &usb)
        {
            bool standby_ok = false;
            if (!kernel_like_reset_cpu(usb, &standby_ok)) {
                throw InitException("CPU 复位失败，无法跳转到 ROM");
            }

            uint32_t ivb = 0;
            if (usb.read_reg(ADR_N10CFG_DEF_IVB, &ivb) < 0) {
                throw InitException("读取 N10CFG_DEF_IVB 失败");
            }
            ivb = (ivb & 0xFFFF0000u) | ROM_IVB_VALUE;
            if (usb.write_reg(ADR_N10CFG_DEF_IVB, ivb) < 0) {
                throw InitException("设置 ROM IVB 失败");
            }

            uint32_t plat_clk = 0;
            if (usb.read_reg(ADR_PLATFORM_CLOCK_ENABLE, &plat_clk) < 0 ||
                usb.write_reg(ADR_PLATFORM_CLOCK_ENABLE, plat_clk | RESET_N_CPUN10_BIT) <
                    0) {
                throw InitException("释放 CPU 复位失败");
            }

            if (!standby_ok) {
                spdlog::warn("  CPU 未进入 standby，继续执行 ROM 跳转");
            }
            usleep(500000);
        }

    } // anonymous namespace

    /* ==================== InitPhase 辅助函数 ==================== */

    const char *init_phase_name(InitPhase phase)
    {
        switch (phase) {
            case InitPhase::NONE:
                return "未开始";
            case InitPhase::CHIP_ID:
                return "芯片ID验证";
            case InitPhase::CPU_RESET:
                return "CPU复位+ROM就绪";
            case InitPhase::MAC_RESET:
                return "MAC复位";
            case InitPhase::HW_CONFIG:
                return "硬件配置";
            case InitPhase::FIRMWARE:
                return "固件下载";
            case InitPhase::FW_VERIFY:
                return "固件验证";
            case InitPhase::PHY_RF:
                return "PHY/RF校准表+PLL";
            case InitPhase::CHANNEL:
                return "信道设置";
            case InitPhase::PHY_ENABLE:
                return "PHY启用";
            case InitPhase::COMPLETED:
                return "完成";
            default:
                return "未知";
        }
    }

    /* ==================== 构造/析构 ==================== */

    ChipInitializer::ChipInitializer(UsbTransport &usb,
                                     std::shared_ptr<spdlog::logger> logger)
        : usb_(usb), logger_(std::move(logger)), current_phase_(InitPhase::NONE),
          mcu_running_(false)
    {
        if (!logger_) {
            throw std::invalid_argument("ChipInitializer: logger 不能为空");
        }
    }

    ChipInitializer::~ChipInitializer()
    {
        if (current_phase_ != InitPhase::NONE && !is_initialized()) {
            try {
                shutdown();
            } catch (...) {
                logger_->error("[INIT] 析构时关闭失败，忽略异常");
            }
        }
    }

    void ChipInitializer::set_firmware_loader(FirmwareLoader loader)
    {
        fw_loader_ = loader;
    }

    int ChipInitializer::default_firmware_loader(const std::string &firmware_path,
                                                 const uint8_t **data,
                                                 size_t *size)
    {
        if (should_use_kernel_embedded_firmware(firmware_path)) {
            const auto *kernel_fw       = embedded_firmware::kernel_data();
            const size_t kernel_fw_size = embedded_firmware::kernel_size();

            auto *fw_data = new (std::nothrow) uint8_t[kernel_fw_size];
            if (!fw_data) {
                return -ENOMEM;
            }

            std::memcpy(fw_data, kernel_fw, kernel_fw_size);
            *data = fw_data;
            *size = kernel_fw_size;
            spdlog::info("  固件来源: 内核内嵌数组 ssv6x5x_sw_bin ({} bytes)",
                         kernel_fw_size);
            return 0;
        }

        FILE *fp = fopen(firmware_path.c_str(), "rb");
        if (!fp) {
            return -ENOENT;
        }

        fseek(fp, 0, SEEK_END);
        long file_size = ftell(fp);
        fseek(fp, 0, SEEK_SET);

        if (file_size <= 0) {
            fclose(fp);
            return -EINVAL;
        }

        auto *fw_data = new (std::nothrow) uint8_t[file_size];
        if (!fw_data) {
            fclose(fp);
            return -ENOMEM;
        }

        if (fread(fw_data, 1, static_cast<size_t>(file_size), fp) !=
            static_cast<size_t>(file_size)) {
            delete[] fw_data;
            fclose(fp);
            return -EIO;
        }
        fclose(fp);

        *data = fw_data;
        *size = static_cast<size_t>(file_size);
        spdlog::info("  固件来源: 外部文件 {} ({} bytes)", firmware_path, *size);
        return 0;
    }

    /* ==================== 阶段 1: 芯片 ID 验证 ==================== */

    std::string ChipInitializer::read_chip_id()
    {
        logger_->info("=== 阶段 1: 芯片 ID 验证 ===");

        chip_id_ = usb_.read_chip_id();

        if (chip_id_.empty()) {
            throw InitException("读取芯片 ID 失败");
        }

        logger_->info("  芯片型号: {}", chip_id_);
        current_phase_ = InitPhase::CHIP_ID;

        return chip_id_;
    }

    void ChipInitializer::read_efuse_calibration()
    {
        /*
         * 内核在 ssv6xxx_read_configuration() 中、CPU 复位之前读取 EFUSE。
         * 这里必须保持同一时机；固件启动后再访问 EFUSE 控制器会改变 HCI
         * 状态，部分板卡会因此在后续 PLL 初始化阶段失去响应。
         */
        logger_->info("  正在读取 EFUSE 校准映射...");

        uint32_t status = 0;
        if (usb_.read_reg(ADR_EFUSE_STATUS, &status) < 0) {
            logger_->warn("  EFUSE 状态读取失败，使用默认校准值");
            return;
        }

        if ((status & 0x1u) == 0) {
            if (usb_.write_reg(ADR_EFUSE_RD_KICK, 1) < 0) {
                logger_->warn("  EFUSE 读取触发失败，使用默认校准值");
                return;
            }

            bool done = false;
            for (int i = 0; i < 10000; ++i) {
                uint32_t progress = 0;
                if (usb_.read_reg(ADR_EFUSE_STATUS2, &progress) < 0)
                    break;
                if ((progress & 0x1u) != 0) {
                    done = true;
                    break;
                }
                usleep(100);
            }
            if (!done) {
                logger_->warn("  EFUSE 读取超时，使用默认校准值");
                return;
            }
        }

        uint32_t chip_identity = 0;
        if (usb_.read_reg(ADR_EFUSE_WDATA_0_0, &chip_identity) < 0) {
            logger_->warn("  EFUSE 芯片身份读取失败，使用默认校准值");
            return;
        }

        std::array<uint8_t, EFUSE_MAP_BYTES> map{};
        std::array<uint32_t, EFUSE_MAP_BYTES / sizeof(uint32_t)> words{};
        for (size_t i = 0; i < words.size(); ++i) {
            if (usb_.read_reg(ADR_EFUSE_WDATA_0_1 +
                                  static_cast<uint32_t>(i * sizeof(uint32_t)),
                              &words[i]) < 0) {
                logger_->warn("  EFUSE 数据读取失败，使用默认校准值");
                return;
            }
            map[i * 4 + 0] = static_cast<uint8_t>(words[i] & 0xFFu);
            map[i * 4 + 1] = static_cast<uint8_t>((words[i] >> 8) & 0xFFu);
            map[i * 4 + 2] = static_cast<uint8_t>((words[i] >> 16) & 0xFFu);
            map[i * 4 + 3] = static_cast<uint8_t>((words[i] >> 24) & 0xFFu);
        }

        logger_->info(
            "  EFUSE chip=0x{:08X}, map={:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
            chip_identity, words[0], words[1], words[2], words[3], words[4], words[5],
            words[6]);

        auto read_bits = [&map](size_t bit, size_t count) -> uint32_t {
            uint32_t value = 0;
            for (size_t i = 0; i < count; ++i) {
                const size_t current = bit + i;
                const uint32_t bit_value =
                    (map[current / 8] >> (current % 8)) & 0x1u;
                value |= bit_value << i;
            }
            return value;
        };

        /* 解析格式与内核 parser_efuse() 一致：4 bit 类型 + 变长数据。 */
        size_t bit = 0;
        while (bit + 4 <= map.size() * 8) {
            const uint32_t type = read_bits(bit, 4);
            size_t payload_bits = 0;
            switch (type) {
                case 0: // 结束标记
                    bit = map.size() * 8;
                    continue;
                case 1: // EFUSE_R_CALIBRATION_RESULT
                case 2: // EFUSE_SAR_RESULT
                case EFUSE_ITEM_XTAL:
                case EFUSE_ITEM_TX_POWER_1:
                case EFUSE_ITEM_TX_POWER_2:
                case EFUSE_ITEM_RATE_1:
                case EFUSE_ITEM_RATE_2:
                    payload_bits = 8;
                    break;
                case 3:  // EFUSE_MAC
                case 11: // EFUSE_MAC_NEW
                    payload_bits = 48;
                    break;
                case 7: // EFUSE_CHIP_ID
                    payload_bits = 4;
                    break;
                case 8: // NO_USE
                    payload_bits = 0;
                    break;
                case 9:  // EFUSE_VID
                case 10: // EFUSE_PID
                    payload_bits = 16;
                    break;
                default:
                    /* 其它类型不应出现在当前 ABI 中。 */
                    bit = map.size() * 8;
                    continue;
            }

            if (bit + 4 + payload_bits > map.size() * 8)
                break;

            const uint32_t value = read_bits(bit + 4, payload_bits);
            if (type < 32)
                efuse_.item_mask |= (1u << type);
            switch (type) {
                case EFUSE_ITEM_XTAL:
                    efuse_.xtal_offset = static_cast<uint8_t>(value);
                    break;
                case EFUSE_ITEM_TX_POWER_1:
                    efuse_.tx_power_index_1 = static_cast<uint8_t>(value);
                    break;
                case EFUSE_ITEM_TX_POWER_2:
                    efuse_.tx_power_index_2 = static_cast<uint8_t>(value);
                    break;
                case EFUSE_ITEM_RATE_1:
                    efuse_.rate_table_1 = static_cast<uint8_t>(value);
                    break;
                case EFUSE_ITEM_RATE_2:
                    efuse_.rate_table_2 = static_cast<uint8_t>(value);
                    break;
                default:
                    break;
            }
            bit += 4 + payload_bits;
        }

        logger_->info(
            "  EFUSE 校准: xtal=0x{:02X} tx1=0x{:02X} tx2=0x{:02X} rate1=0x{:02X} rate2=0x{:02X} mask=0x{:08X}",
            efuse_.xtal_offset, efuse_.tx_power_index_1, efuse_.tx_power_index_2,
            efuse_.rate_table_1, efuse_.rate_table_2, efuse_.item_mask);
    }

    /* ==================== 阶段 2: CPU 复位 + ON3 + USB ROM 就绪
     * ==================== */

    void ChipInitializer::reset_cpu_and_rom()
    {
        logger_->info("=== 阶段 2: CPU 复位 + ON3 电源域 + USB ROM 就绪 ===");

        /* ★ 匹配内核 ssv6xxx_init_hw() 的前置步骤：
         *   HAL_RESET_CPU(sh) → SSV_SET_ON3_ENABLE(false) → SSV_SET_ON3_ENABLE(true)
         *   → HAL_WAIT_USB_ROM_READY(sh)
         *
         * 内核的 HAL_RESET_CPU 实际调用 jump_to_rom：
         *   1. ssv6006c_reset_cpu() — 让 CPU 进入 standby，然后 RESET_N_CPUN10=0
         *   2. SET_N10CFG_DEFAULT_IVB(0x4) — 设置 IVB 为 ROM 地址 0x40000
         *   3. SET_RESET_N_CPUN10(1) — 释放复位，让 CPU 从 ROM 启动
         *   4. msleep(500) — 等待 ROM 代码就绪
         *
         * 但在用户空间 USB 场景下，设备刚插入时 ROM 代码可能已经在运行，
         * 只需确保 CPU 处于复位状态即可。 */

        logger_->info("  正在复位 CPU...");
        bool standby_ok = false;
        if (!kernel_like_reset_cpu(usb_, &standby_ok)) {
            throw InitException("CPU 复位失败");
        }
        if (!standby_ok) {
            logger_->warn("  CPU 未进入 standby，已退化为直接复位");
        }

        /* ON3 电源域控制（匹配内核 SSV_SET_ON3_ENABLE） */
        logger_->info("  正在控制 ON3 电源域...");
        {
            /* 等待 SYSCTRL_STATUS == 0x303（振荡器和 DPLL 就绪） */
            for (int i = 0; i < 100; i++) {
                uint32_t status = REG_READ(ADR_SYSCTRL_STATUS);
                if (status == 0x303)
                    break;
                usleep(1000);
            }

            /* 禁用 ON3：写入 POWER_ON_OFF_CTRL + SYSCTRL_COMMAND(0x40c) */
            REG_WRITE(ADR_POWER_ON_OFF_CTRL, 0x7334);
            REG_WRITE(ADR_SYSCTRL_COMMAND, 0x40c);

            /* 等待状态稳定 */
            for (int i = 0; i < 100; i++) {
                uint32_t status = REG_READ(ADR_SYSCTRL_STATUS);
                if (status == 0x303)
                    break;
                usleep(1000);
            }

            /* 启用 ON3：写入 SYSCTRL_COMMAND(0x80c) */
            REG_WRITE(ADR_SYSCTRL_COMMAND, 0x80c);

            /* 等待状态稳定 */
            for (int i = 0; i < 100; i++) {
                uint32_t status = REG_READ(ADR_SYSCTRL_STATUS);
                if (status == 0x303)
                    break;
                usleep(1000);
            }
        }
        logger_->info("  ON3 电源域控制完成");

        /* 等待 USB ROM 代码就绪（USB20_HOST_SELRW=1）
         * ROM 代码就绪后会连接 HCI 到 USB，此时才能进行后续操作 */
        logger_->info("  等待 USB ROM 代码就绪...");
        {
            bool rom_ready = false;
            for (int i = 0; i < 100; i++) {
                uint32_t sel = REG_READ(ADR_USB20_HOST_SEL);
                if (sel & 0x1) {
                    logger_->info("  USB ROM 就绪（等待 {} ms）", i);
                    rom_ready = true;
                    break;
                }
                usleep(1000);
            }
            if (!rom_ready) {
                logger_->warn("  ⚠️  USB ROM 就绪超时（100ms），继续尝试...");
            }
        }

        current_phase_ = InitPhase::CPU_RESET;
        logger_->info("  CPU 复位 + ROM 就绪完成");
    }

    /* ==================== 阶段 3: MAC 复位（含 USB ACC 控制） ====================
     */

    void ChipInitializer::mac_reset()
    {
        logger_->info("=== 阶段 3: MAC 复位 ===");

        /* ★ 匹配内核 ssv6xxx_init_hw() 的 MAC 复位流程：
         *   SSV_DISABLE_USB_ACC(sc, 0x4) → HAL_RESET_HW_MAC(sh) →
         * SSV_ENABLE_USB_ACC(sc, 0x4)
         *
         * USB ACC 控制是关键：MAC 复位期间需要禁用 EP4(RX) 的 USB 访问，
         * 否则复位过程中 USB 端点状态可能不一致
         *
         * MAC 复位后需要重新启用所有 USB ACC 端点（CMD/RSP/TX/RX），
         * 因为 MAC 复位可能影响 USB 端点的访问控制状态 */

        /* 步骤 1: 禁用 USB ACC EP4（RX 端点） */
        logger_->info("  禁用 USB ACC EP4...");
        {
            uint32_t acc = REG_READ(ADR_USB_ACC_CTRL_REG_0);
            REG_WRITE(ADR_USB_ACC_CTRL_REG_0, acc & ~(1u << 3));
        }

        /* 步骤 2: 禁用 PHY */
        REG_SET_BITS(ADR_TU_PHY_ENABLE, 0, TU_PHY_ALL_EN);

        /* 步骤 3: MAC 复位（使用 sysplf_reset 绕过序列号匹配）
         * sysplf_reset 内部会自动清理 USB 端点状态和重置序列号 */
        logger_->info("  正在复位 MAC...");
        usb_.sysplf_reset(ADR_BRG_SW_RST, (1u << MAC_SW_RST_SFT));

        /* 步骤 4: 轮询等待复位释放 */
        for (unsigned i = 0; i < 10000; i++) {
            uint32_t rv = REG_READ(ADR_BRG_SW_RST);
            if (rv == 0)
                break;
        }

        {
            uint32_t rv;
            usb_.read_reg(ADR_BRG_SW_RST, &rv);
            if (rv != 0) {
                logger_->warn("  MAC 复位超时！");
                throw InitException("MAC 复位超时");
            }
        }

        /* 步骤 5: 设置 MAC 时钟频率（匹配内核 ssv6006c_reset_hw_mac）
         * ★ 内核只设置三个参数，不写 ADR_PLATFORM_CLOCK_ENABLE：
         *   SET_MAC_CLK_80M(n)       — ADR_MTX_TIME_FINETUNE bit[28]
         *   SET_PHYTXSTART_NCYCLE(n) — ADR_MTX_TIME_FINETUNE bit[16:22]
         *   SET_PRESCALER_US(n)      — ADR_PRESCALER_USTIMER bit[0:8]
         *
         * ⚠️ 绝对不能写 ADR_PLATFORM_CLOCK_ENABLE！
         *   bit[0] = SYS_CLK_EN（系统时钟使能），清零会导致整个芯片停摆
         *   bit[1] = MAC_CLK_EN（MAC 时钟使能），清零会导致 MAC 停止工作 */
        {
            uint32_t clk_digi = REG_READ(ADR_CLOCK_SELECTION);
            uint32_t clk_sel  = (clk_digi >> 0) & 0xF;
            if (clk_sel == 8) {
                detected_bus_clock_mhz_ = 80;
                logger_->info("  MAC 时钟: 80MHz");
                /* SET_MAC_CLK_80M(1) + SET_PHYTXSTART_NCYCLE(26) */
                uint32_t mtx_fine = REG_READ(ADR_MTX_TIME_FINETUNE);
                mtx_fine          = (mtx_fine & 0xEFF0FFFFu) | (1u << 28) | (26u << 16);
                REG_WRITE(ADR_MTX_TIME_FINETUNE, mtx_fine);
                /* SET_PRESCALER_US(80) */
                REG_WRITE(ADR_PRESCALER_USTIMER, 80);
            } else if (clk_sel == 4) {
                detected_bus_clock_mhz_ = 40;
                logger_->info("  MAC 时钟: 40MHz");
                /* SET_MAC_CLK_80M(0) + SET_PHYTXSTART_NCYCLE(13) */
                uint32_t mtx_fine = REG_READ(ADR_MTX_TIME_FINETUNE);
                mtx_fine          = (mtx_fine & 0xEFF0FFFFu) | (0u << 28) | (13u << 16);
                REG_WRITE(ADR_MTX_TIME_FINETUNE, mtx_fine);
                /* SET_PRESCALER_US(40) */
                REG_WRITE(ADR_PRESCALER_USTIMER, 40);
            } else {
                logger_->warn("  MAC 时钟选择异常: 0x{:X}", clk_sel);
            }
        }

        /* 步骤 6: 启用所有 USB ACC 端点（匹配内核 ssv6006c_enable_usb_acc）
         * 内核只启用 EP4(RX)，但 MAC 复位可能影响所有端点的 ACC 状态，
         * 这里启用 CMD(bit0)/RSP(bit1)/TX(bit2)/RX(bit3) 以确保 USB 通信正常 */
        logger_->info("  启用所有 USB ACC 端点...");
        {
            uint32_t acc = REG_READ(ADR_USB_ACC_CTRL_REG_0);
            acc |= (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3);
            REG_WRITE(ADR_USB_ACC_CTRL_REG_0, acc);
            logger_->info("    USB_ACC_CTRL_REG_0: 0x{:08X}", acc);
        }

        mcu_running_ = false;
        logger_->info("  MAC 复位完成");
        current_phase_ = InitPhase::MAC_RESET;
    }

    /* ==================== 阶段 4: 硬件配置 ==================== */

    void ChipInitializer::hw_config(const DriverConfig &config)
    {
        logger_->info("=== 阶段 4: 硬件配置 ===");
        logger_->info("  正在配置 HCI 和 MAC 寄存器...");

        /* TXQ 配置 */
        REG_WRITE(ADR_TXQ4_MTX_Q_AIFSN, 0xFFFF2101);
        REG_SET_BITS(ADR_MTX_BCN_EN_MISC, 0, MTX_HALT_MNG_UNTIL_DTIM_MSK);

        /* 控制寄存器 */
        REG_WRITE(ADR_CONTROL, 0x12000006);

        /* RX 时间戳配置 */
        REG_WRITE(ADR_RX_TIME_STAMP_CFG, ((28u << MRX_STP_OFST_SFT) | 0x01));

        /* HCI TX/RX 信息大小 */
        REG_WRITE(
            ADR_HCI_TX_RX_INFO_SIZE,
            (static_cast<uint32_t>(TXPB_OFFSET) << TX_PBOFFSET_SFT) |
                (static_cast<uint32_t>(SSV6XXX_TX_DESC_LEN) << TX_INFO_SIZE_SFT) |
                (static_cast<uint32_t>(SSV6XXX_RX_HW_DESC_LEN) << RX_INFO_SIZE_SFT) |
                (static_cast<uint32_t>(SSV6XXX_RX_PINFO_PAD) << RX_LAST_PHY_SIZE_SFT));

        /* MMU 配置 */
        {
            uint32_t mmu = REG_READ(ADR_MMU_CTRL);
            mmu |= (0xFFu << MMU_SHARE_MCU_SFT);
            REG_WRITE(ADR_MMU_CTRL, mmu);
        }

        /* Watchdog 配置 */
        {
            uint32_t wd = REG_READ(ADR_MRX_WATCH_DOG);
            wd &= 0xFFFFFFF0;
            REG_WRITE(ADR_MRX_WATCH_DOG, wd);
        }

        /* TX/RX ID 阈值 */
        {
            uint32_t id_thr = REG_READ(ADR_TRX_ID_THRESHOLD);
            id_thr          = (id_thr & 0xFFFF0000) |
                     (SSV6200_ID_TX_THRESHOLD << TX_ID_THOLD_SFT) |
                     (SSV6200_ID_RX_THRESHOLD << RX_ID_THOLD_SFT);
            REG_WRITE(ADR_TRX_ID_THRESHOLD, id_thr);
        }

        /* TX/RX 长度阈值 */
        {
            uint32_t len_thr = REG_READ(ADR_ID_LEN_THREADSHOLD1);
            len_thr          = (len_thr & 0x0F) |
                      (SSV6200_PAGE_TX_THRESHOLD << ID_TX_LEN_THOLD_SFT) |
                      (SSV6200_PAGE_RX_THRESHOLD << ID_RX_LEN_THOLD_SFT);
            REG_WRITE(ADR_ID_LEN_THREADSHOLD1, len_thr);
        }

        /* TSF 定时器启用 */
        REG_SET_BITS(ADR_MTX_BCN_EN_MISC, (1u << MTX_TSF_TIMER_EN_SFT), 0);

        /* 全局设置 */
        REG_WRITE(ADR_GLBLE_SET,
                  (0u << OP_MODE_SFT) | (0u << SNIFFER_MODE_SFT) |
                      (1u << DUP_FLT_SFT) |
                      (static_cast<uint32_t>(SSV6200_TX_PKT_RSVD_SETTING)
                       << TX_PKT_RSVD_SFT) |
                      (static_cast<uint32_t>(RXPB_OFFSET) << PB_OFFSET_SFT));

        /* MAC 地址 */
        REG_WRITE(ADR_STA_MAC_0, static_cast<uint32_t>(config.mac_addr[0]) |
                                     static_cast<uint32_t>(config.mac_addr[1]) << 8 |
                                     static_cast<uint32_t>(config.mac_addr[2]) << 16 |
                                     static_cast<uint32_t>(config.mac_addr[3]) << 24);
        REG_WRITE(ADR_STA_MAC_1, static_cast<uint32_t>(config.mac_addr[4]) |
                                     static_cast<uint32_t>(config.mac_addr[5]) << 8);

        /* BSSID（使用相同 MAC 地址） */
        REG_WRITE(ADR_BSSID_0, static_cast<uint32_t>(config.mac_addr[0]) |
                                   static_cast<uint32_t>(config.mac_addr[1]) << 8 |
                                   static_cast<uint32_t>(config.mac_addr[2]) << 16 |
                                   static_cast<uint32_t>(config.mac_addr[3]) << 24);
        REG_WRITE(ADR_BSSID_1, static_cast<uint32_t>(config.mac_addr[4]) |
                                   static_cast<uint32_t>(config.mac_addr[5]) << 8);

        /* 以太网类型过滤器（清零） */
        REG_WRITE(ADR_TX_ETHER_TYPE_0, 0);
        REG_WRITE(ADR_TX_ETHER_TYPE_1, 0);
        REG_WRITE(ADR_RX_ETHER_TYPE_0, 0);
        REG_WRITE(ADR_RX_ETHER_TYPE_1, 0);

        /* 原因陷阱 */
        REG_WRITE(ADR_REASON_TRAP0, 0x7FBC7F87);
        REG_WRITE(ADR_REASON_TRAP1, 0x0000033F);
        REG_WRITE(ADR_TRAP_HW_ID, M_ENG_CPU);
        REG_WRITE(ADR_WSID0, 0);
        REG_WRITE(ADR_WSID1, 0);

        /* RX 流路由 */
        REG_WRITE(ADR_RX_FLOW_DATA, RX_HCI);
        REG_WRITE(ADR_RX_FLOW_MNG, RX_HCI);
        REG_WRITE(ADR_RX_FLOW_CTRL, RX_HCI);

        /* 决策表 */
        static const uint16_t deci_tbl[] = {
            0x3DF1,
            0x38F0,
            0x3AF1,
            0x21C1,
            0x4BF1,
            0x4CB1,
            0x10F0,
            0x0AF0,
            0x16F0,
            0x02F0,
            0x40F0,
            0x0000,
            0x0001,
            0x0003,
            0x0001,
            0x0001,
            0x2008,
            0x1001,
            0x0808,
            0x1040,
            0x2008,
            0x800E,
            0x0BB8,
            0x2B88,
            0x0800,
        };
        for (int i = 0; i < 16; i++)
            REG_WRITE(ADR_MRX_FLT_TB0 + static_cast<uint32_t>(i * 4), deci_tbl[i]);
        for (int i = 0; i < 9; i++)
            REG_WRITE(ADR_MRX_FLT_EN0 + static_cast<uint32_t>(i * 4), deci_tbl[16 + i]);

        /* STA 操作模式 */
        REG_SET_BITS(ADR_GLBLE_SET, SSV6XXX_OPMODE_STA, OP_MODE_MSK);

        /* 邮箱阈值 */
        REG_WRITE(ADR_MB_THRESHOLD6, 0x80000000);
        REG_WRITE(ADR_MB_THRESHOLD8, 0x04020000);
        REG_WRITE(ADR_MB_THRESHOLD9, 0x00000404);

        logger_->info("  硬件配置完成");
        current_phase_ = InitPhase::HW_CONFIG;
    }

    /* ==================== 阶段 5: 固件下载 + EP0 切换 ==================== */

    void ChipInitializer::download_firmware(const DriverConfig &config)
    {
        logger_->info("=== 阶段 5: 固件下载 ===");

        FirmwareLoader loader = fw_loader_ ? fw_loader_ : default_firmware_loader;

        const uint8_t *firmware_data = nullptr;
        size_t firmware_size         = 0;

        logger_->info("  正在打开固件文件: {}", config.firmware_path);
        int ret = loader(config.firmware_path, &firmware_data, &firmware_size);
        if (ret < 0 || !firmware_data || firmware_size == 0) {
            throw InitException("无法读取固件文件: " + config.firmware_path);
        }

        std::vector<uint8_t> fw_block(CHECKSUM_BLOCK_SIZE, 0xA5);
        uint32_t checksum           = FW_CHECKSUM_INIT;
        uint32_t sram_addr          = FW_START_ADDR;
        size_t remaining            = firmware_size;
        size_t offset               = 0;
        uint32_t block_count        = 0;
        const uint32_t total_blocks = static_cast<uint32_t>(
            (firmware_size + CHECKSUM_BLOCK_SIZE - 1) / CHECKSUM_BLOCK_SIZE);

        logger_->info("  正在跳转到 ROM 代码（固件下载前置）...");
        kernel_like_jump_to_rom(usb_);

        logger_->info("  正在写入固件块（1024B 对齐 + 0xA5 填充）...");
        while (remaining > 0) {
            const size_t chunk =
                std::min(remaining, static_cast<size_t>(CHECKSUM_BLOCK_SIZE));
            std::fill(fw_block.begin(), fw_block.end(), 0xA5);
            std::memcpy(fw_block.data(), firmware_data + offset, chunk);

            try {
                usb_.download_firmware(fw_block.data(), fw_block.size(), sram_addr);
            } catch (...) {
                delete[] firmware_data;
                throw;
            }

            accumulate_fw_checksum(checksum, sram_addr, fw_block.data());

            offset += chunk;
            remaining -= chunk;
            sram_addr += CHECKSUM_BLOCK_SIZE;
            ++block_count;

            if (block_count == total_blocks || (block_count % 32) == 0) {
                logger_->info("  固件写入进度: {}/{} 块 ({:.0f}%)", block_count,
                              total_blocks, 100.0 * block_count / total_blocks);
            }
        }
        delete[] firmware_data;
        checksum = finalize_fw_checksum(checksum);

        logger_->info("  正在复位 CPU (USB ROM → 固件转换)...");
        if (!kernel_like_reset_cpu(usb_)) {
            throw InitException("固件写入后 CPU 复位失败");
        }

        /* 设置 SRAM 模式：ILM 160KB, DLM 32KB */
        REG_SET_BITS(ADR_SRAM_MODE_CFG, (1u << 1), (1u << 1));

        REG_WRITE(FW_VERSION_REG, static_cast<uint32_t>(block_count << 16));

        uint32_t ivb = REG_READ(ADR_N10CFG_DEF_IVB);
        REG_WRITE(ADR_N10CFG_DEF_IVB, ivb & 0xFFFF0000u);
        REG_SET_BITS(ADR_MANUAL_RESET_N, CLK_EN_CPUN10_BIT, CLK_EN_CPUN10_BIT);
        REG_SET_BITS(ADR_PLATFORM_CLOCK_ENABLE, RESET_N_CPUN10_BIT,
                     RESET_N_CPUN10_BIT);

        logger_->info("    MCU 已启用，等待 50ms 让固件自检...");
        usleep(50000);

        uint32_t fw_status;
        usb_.read_reg(FW_VERSION_REG, &fw_status);
        fw_status &= FW_STATUS_MASK;
        logger_->info("  固件自检结果: fw=0x{:08X}, host=0x{:08X}", fw_status,
                      checksum);
        if (fw_status != checksum) {
            throw InitException("固件自检校验和不匹配");
        }
        usb_.write_reg(FW_VERSION_REG, (~checksum) & FW_STATUS_MASK);

        /* 对齐内核默认路径：固件加载后继续使用 CMD/RSP bulk 访问寄存器。
         * EP0 读写仅在特定内核配置(CONFIG_USB_EP0_RW_REGISTER)下启用，
         * 对当前固件默认禁用，避免探测过程干扰固件启动后的命令通道。 */
        logger_->info("  固件后寄存器访问: 保持 CMD/RSP bulk 模式");

        mcu_running_ = true;
        logger_->info("  固件下载成功");
        current_phase_ = InitPhase::FIRMWARE;
    }

    /* ==================== 阶段 6: 固件验证 ==================== */

    void ChipInitializer::verify_firmware()
    {
        logger_->info("=== 阶段 6: 固件验证 ===");

        /* 验证固件版本 */
        uint32_t fw_ver = REG_READ(FW_VERSION_REG);
        logger_->info("  固件版本寄存器: 0x{:08X}", fw_ver);

        if (fw_ver == FIRWARE_NOT_MATCH_CODE) {
            throw InitException("固件芯片 ID 不匹配");
        }

        /* WDOG 在这版固件中可能保持不变，只作为辅助诊断；后续阻塞式
         * HCI 命令的匹配响应才是固件存活的可靠判据。 */
        {
            uint32_t wdog1, wdog2;
            usb_.read_reg(ADR_MCU_WDOG_REG, &wdog1);
            usleep(100000);
            usb_.read_reg(ADR_MCU_WDOG_REG, &wdog2);
            logger_->info("  WDOG: 0x{:08X} -> 0x{:08X} ({:s})", wdog1, wdog2,
                          (wdog1 != wdog2) ? "计数器变化" : "计数器未变化，仅供参考");
        }

        logger_->info("  固件验证通过");
        current_phase_ = InitPhase::FW_VERIFY;
    }

    /* ==================== 阶段 7: PHY/RF 校准表 + PLL 初始化 ====================
     */

    void ChipInitializer::load_phy_rf_tables(const DriverConfig &config)
    {
        logger_->info("=== 阶段 7: PHY/RF 校准表 + PLL 初始化 ===");

        uint32_t bus_clock_mhz = detected_bus_clock_mhz_;
        if (config.bus_clock == BusClock::MHZ_40)
            bus_clock_mhz = 40;
        else if (config.bus_clock == BusClock::MHZ_80)
            bus_clock_mhz = 80;

        /* ADR_PMU_REG_3 bit[0] 是内核 GET_RG_DCDC_MODE 使用的硬件状态。
         * AUTO 只读取并保留该状态，绝不主动切换稳压器拓扑。 */
        const uint32_t pmu_reg3 = REG_READ(0xCCB0B008);
        bool use_dcdc           = (pmu_reg3 & 0x1u) != 0;
        if (config.voltage_regulator == VoltageRegulator::LDO)
            use_dcdc = false;
        else if (config.voltage_regulator == VoltageRegulator::DCDC)
            use_dcdc = true;

        logger_->info("  板级配置: bus={}MHz regulator={} (PMU_REG_3=0x{:08X})",
                      bus_clock_mhz, use_dcdc ? "DCDC" : "LDO", pmu_reg3);

        /* Match ssv6006_turismoC_set_pll_phy_rf(): firmware requires the complete
         * calibration payload, not the parameterless INIT_PLL helper. */
        SsvFwRfCali cali{};
        cali.xtal                 = 40;
        cali.options              = 1u | (1u << 4) | (bus_clock_mhz << 16);
        cali.thermal_thresholds   = 30u | (50u << 16);
        const SsvFwTempTable temp = {
            {6, 6, 6, 6, 6, 6, 6}, 0x42, 0x42, 7, 9, 10, 7, 7, 7, 7, 7, 0};
        cali.rf_table.rt_config         = temp;
        cali.rf_table.ht_config         = temp;
        cali.rf_table.lt_config         = temp;
        cali.rf_table.rf_gain           = 4;
        cali.rf_table.rate_gain_b       = 12;
        cali.rf_table.rate_config_g     = {1, 3, 5, 7};
        cali.rf_table.rate_config_20n   = {1, 3, 5, 7};
        cali.rf_table.rate_config_40n   = {1, 3, 5, 7};
        cali.rf_table.low_boundary      = 30;
        cali.rf_table.high_boundary     = 50;
        const SsvFwTemp5gTable temp5g   = {11, 8, 8, 7, 0x9264924a, 0x96dbb6cc};
        cali.rf_table.rt_5g_config      = temp5g;
        cali.rf_table.ht_5g_config      = temp5g;
        cali.rf_table.lt_5g_config      = temp5g;
        cali.rf_table.band_f0_threshold = 0x141E;
        cali.rf_table.band_f1_threshold = 0x157C;
        cali.rf_table.band_f2_threshold = 0x1644;

        /*
         * ssv6006_turismoC_chg_xtal_freq_offset()/chg_dpd_bbscale() 会在内核
         * 中把每张卡的 EFUSE 参数覆盖到固件校准表。不能把所有 6006C 都当成
         * 同一块板：003 的晶振偏移是 0x73，而 005 是 0x7D；5 GHz 增益也不
         * 相同。缺少这些参数时，PLL 锁定表面成功，但实际信道会偏移或 RX
         * 灵敏度严重异常，表现为持续花屏/无法解密。
         */
        if ((efuse_.item_mask & (1u << EFUSE_ITEM_XTAL)) != 0) {
            cali.rf_table.rt_config.freq_xi = efuse_.xtal_offset;
            cali.rf_table.rt_config.freq_xo = efuse_.xtal_offset;
            cali.rf_table.ht_config.freq_xi = efuse_.xtal_offset;
            cali.rf_table.ht_config.freq_xo = efuse_.xtal_offset;
            cali.rf_table.lt_config.freq_xi = efuse_.xtal_offset;
            cali.rf_table.lt_config.freq_xo = efuse_.xtal_offset;
        }

        auto valid_gain_index = [](uint8_t value) -> uint8_t {
            return (value <= 15) ? value : 0;
        };
        if ((efuse_.item_mask & (1u << EFUSE_ITEM_TX_POWER_1)) != 0) {
            const uint8_t gain_2g = valid_gain_index(efuse_.tx_power_index_1 & 0x0F);
            const uint8_t gain_5500 =
                valid_gain_index((efuse_.tx_power_index_1 >> 4) & 0x0F);
            for (auto *table : {&cali.rf_table.rt_config, &cali.rf_table.ht_config,
                                &cali.rf_table.lt_config}) {
                for (auto &gain : table->band_gain)
                    gain = gain_2g;
            }
            for (auto *table : {&cali.rf_table.rt_5g_config,
                                &cali.rf_table.ht_5g_config,
                                &cali.rf_table.lt_5g_config}) {
                table->bbscale_band1 = gain_5500;
            }
        }
        if ((efuse_.item_mask & (1u << EFUSE_ITEM_TX_POWER_2)) != 0) {
            const uint8_t gain_5700 = valid_gain_index(efuse_.tx_power_index_2 & 0x0F);
            const uint8_t gain_5900 =
                valid_gain_index((efuse_.tx_power_index_2 >> 4) & 0x0F);
            for (auto *table : {&cali.rf_table.rt_5g_config,
                                &cali.rf_table.ht_5g_config,
                                &cali.rf_table.lt_5g_config}) {
                table->bbscale_band2 = gain_5700;
                table->bbscale_band3 = gain_5900;
            }
        }
        logger_->info(
            "  应用 EFUSE RF 校准: freq=0x{:02X}, 2G/5500/5700/5900 gain={}/{}/{}/{}, rate1=0x{:02X} rate2=0x{:02X}",
            ((efuse_.item_mask & (1u << EFUSE_ITEM_XTAL)) != 0) ? efuse_.xtal_offset
                                                                : 0x42,
            ((efuse_.item_mask & (1u << EFUSE_ITEM_TX_POWER_1)) != 0)
                ? (efuse_.tx_power_index_1 & 0x0F)
                : 0xFF,
            ((efuse_.item_mask & (1u << EFUSE_ITEM_TX_POWER_1)) != 0)
                ? ((efuse_.tx_power_index_1 >> 4) & 0x0F)
                : 0xFF,
            ((efuse_.item_mask & (1u << EFUSE_ITEM_TX_POWER_2)) != 0)
                ? (efuse_.tx_power_index_2 & 0x0F)
                : 0xFF,
            ((efuse_.item_mask & (1u << EFUSE_ITEM_TX_POWER_2)) != 0)
                ? ((efuse_.tx_power_index_2 >> 4) & 0x0F)
                : 0xFF,
            efuse_.rate_table_1, efuse_.rate_table_2);
        cali.rf_table.signature[0] = 'S';
        cali.rf_table.signature[1] = 'S';
        cali.rf_table.signature[2] = 'V';
        cali.rf_table.version      = 2;
        cali.rf_table.dcdc_flag    = use_dcdc ? 1u : 0u;
        cali.rf_table.extpa_tbl    = {0, 0, 1, 0x0d, 8, 8, 0x0c, 0};

        /* ★ 匹配内核 ssv6xxx_init_hw() 流程：
         *   HAL_SET_RFPHY(sh, SSV6XXX_RFPHY_CMD_INIT_PLL_PHY_RF)
         *   → ssv6006c_init_pll_phy_rf()
         *     → ssv6006c_init_rf()      — RF 校准表
         *     → ssv6006c_init_phy()     — PHY 校准表
         *     → ssv6006c_init_pll()     — PLL 初始化
         *     → ssv6006c_init_rf_phy_extend() — 扩展设置
         *
         * 这些操作在固件下载后执行，因为固件需要先运行才能处理
         * 某些 RF/PHY 相关的 HCI 命令。但校准表本身是直接寄存器写入，
         * 不依赖固件。 */

        /* RF 校准表结构体 */
        struct TuRegval {
            uint32_t addr;
            uint32_t val;
        };

        /* RF 校准表（来自内核 turismoC_rf_reg.c） */
        static const TuRegval tu_rf_tbl[] = {
            {0xCCB0A420, 0x0033E73F},
            {0xCCB0A554, 0x03024444},
            {0xCCB0A594, 0x111E0950},
            {0xCCB0A598, 0x0F1E00FF},
            {0xCCB0A530, 0x001F1F01},
            {0xCCB0A604, 0x001F1F01},
            {0xCCB0A62C, 0x9264924A},
            {0xCCB0A630, 0x96DBB6CC},
            {0xCCB0A634, 0x00000000},
            {0xCCB0A8CC, 0x141E157C},
            {0xCCB0A8D0, 0x00001644},
            {0xCCB0ADA8, 0x806c6c66},
            {0xCCB0ADAC, 0x00000060},
            {0xccb0a88c, 0x00000010},
            {0xccb0a808, 0x88000000},
            {0xCCB0B000, 0x24844214},
        };

        /* RF 高功率补丁 */
        static const TuRegval tu_rf_hp_tbl[] = {
            {0xccb0A424, 0x57444432},
            {0xCCB0A62C, 0x923D923F},
            {0xCCB0A630, 0x92439242},
            {0xCCB0A634, 0x1C309000},
            {0xCCB0A400, 0x22000044},
        };

        /* 加载 RF 校准表 */
        int rf_count = static_cast<int>(sizeof(tu_rf_tbl) / sizeof(tu_rf_tbl[0]));
        logger_->info("  正在加载 RF 校准表 ({} 条目)...", rf_count);
        for (int i = 0; i < rf_count; i++) {
            REG_WRITE(tu_rf_tbl[i].addr, tu_rf_tbl[i].val);
        }

        /* 加载 RF 高功率补丁 */
        int rf_hp_count =
            static_cast<int>(sizeof(tu_rf_hp_tbl) / sizeof(tu_rf_hp_tbl[0]));
        logger_->info("  正在加载 RF 高功率补丁 ({} 条目)...", rf_hp_count);
        for (int i = 0; i < rf_hp_count; i++) {
            REG_WRITE(tu_rf_hp_tbl[i].addr, tu_rf_hp_tbl[i].val);
        }

        /* PLL 初始化 */
        logger_->info("  正在初始化 PLL...");
        {
            uint32_t regval = REG_READ(0xCCB0B004);
            regval          = (regval & 0x7FFFFFFF) | (1u << 31);
            REG_WRITE(0xCCB0B004, regval);
        }

        for (int i = 0; i < 100; i++) {
            usleep(1000);
            uint32_t regval = REG_READ(0xCCB0B044);
            if ((regval & 0x07) == 0x03)
                break;
        }

        usleep(1000);

        /* 设置 PHY_COMMON_SYS + 时钟选择 */
        logger_->info("  正在设置 PHY_COMMON_SYS + 时钟选择...");
        REG_WRITE(ADR_WIFI_PHY_COMMON_SYS_REG, 0x80010000);
        REG_WRITE(ADR_CLOCK_SELECTION, bus_clock_mhz == 80 ? 0x00000008
                                                           : 0x00000004);
        usleep(1000);

        {
            uint32_t phy_sys = REG_READ(ADR_WIFI_PHY_COMMON_SYS_REG);
            uint32_t clk_sel = REG_READ(ADR_CLOCK_SELECTION);
            logger_->info("    PHY_COMMON_SYS=0x{:08X} CLOCK_SEL=0x{:08X}", phy_sys,
                          clk_sel);
        }

        /* PHY 校准表（来自内核 turismoC_wifi_phy_reg.c） */
        static const TuRegval tu_phy_tbl[] = {
            {0xccb0e010, 0x00000FFF},
            {0xccb0e014, 0x00807f03},
            {0xccb0e018, 0x0055003C},
            {0xccb0e01C, 0x00000064},
            {0xccb0e020, 0x00000000},
            {0xccb0e02C, 0x7004606C},
            {0xccb0e030, 0x7004606C},
            {0xccb0e034, 0x1A040400},
            {0xccb0e038, 0x630F36D0},
            {0xccb0e03C, 0x100c0003},
            {0xccb0e040, 0x11600800},
            {0xccb0e044, 0x00080868},
            {0xccb0e048, 0xFF001160},
            {0xccb0e04C, 0x00100040},
            {0xccb0e060, 0x11501150},
            {0xccb0e12C, 0x00001160},
            {0xccb0e130, 0x00100040},
            {0xccb0e134, 0x00080010},
            {0xccb0e180, 0x00010060},
            {0xccb0e184, 0xB5A19080},
            {0xccb0e188, 0xB5A19080},
            {0xccb0e18c, 0xB5A19080},
            {0xccb0e190, 0x00010006},
            {0xccb0e194, 0x06060606},
            {0xccb0e198, 0x06060606},
            {0xccb0e19c, 0x06060606},
            {0xccb0e080, 0x0110000F},
            {0xccb0e098, 0x00102000},
            {0xccb0e09C, 0x00100018},
            {0xccb0e4b4, 0x00002001},
            {0xccb0ecA4, 0x00009001},
            {0xccb0ecB8, 0x000C50CC},
            {0xccb0fc44, 0x00028080},
            {0xccb0f008, 0x00004775},
            {0xccb0f00c, 0x10000075},
            {0xccb0f010, 0x3F304905},
            {0xccb0f014, 0x40182000},
            {0xccb0f018, 0x20600000},
            {0xccb0f01C, 0x0c010080},
            {0xccb0f03C, 0x0000005a},
            {0xccb0f020, 0x20202020},
            {0xccb0f024, 0x20000000},
            {0xccb0f028, 0x50505050},
            {0xccb0f02c, 0x20202020},
            {0xccb0f030, 0x20000000},
            {0xccb0f034, 0x00002424},
            {0xccb0f09c, 0x000030A0},
            {0xccb0f0C0, 0x0f0003c0},
            {0xccb0f0C4, 0x30023003},
            {0xccb0f0CC, 0x00000120},
            {0xccb0f0D0, 0x00000020},
            {0xccb0f130, 0x40000000},
            {0xccb0f164, 0x000e0090},
            {0xccb0f188, 0x82000000},
            {0xccb0f190, 0x00000020},
            {0xccb0f194, 0x09360001},
            {0xccb0f3F8, 0x00100001},
            {0xccb0f3FC, 0x00010425},
            {0xccb0e804, 0x00020000},
            {0xccb0e808, 0x20280060},
            {0xccb0e80c, 0x00003467},
            {0xccb0e810, 0x00430000},
            {0xccb0e814, 0x30000015},
            {0xccb0e818, 0x00390005},
            {0xccb0e81C, 0x05050005},
            {0xccb0e820, 0x00570057},
            {0xccb0e824, 0x00570057},
            {0xccb0e828, 0x00236700},
            {0xccb0e82c, 0x000d1746},
            {0xccb0e830, 0x05051787},
            {0xccb0e834, 0x07800000},
            {0xccb0e89c, 0x009000B0},
            {0xccb0e8A0, 0x00000000},
            {0xccb0ebF8, 0x00100000},
            {0xccb0ebFC, 0x00000001},
        };

        /* PHY 高功率补丁 */
        static const TuRegval tu_phy_hp_tbl[] = {
            {0xccb0e180, 0x00010072},
            {0xccb0e184, 0x90807966},
            {0xccb0e188, 0x90807966},
            {0xccb0e18c, 0x90807966},
        };

        /* 加载 PHY 校准表 */
        int phy_count = static_cast<int>(sizeof(tu_phy_tbl) / sizeof(tu_phy_tbl[0]));
        logger_->info("  正在加载 PHY 校准表 ({} 条目)...", phy_count);
        for (int i = 0; i < phy_count; i++) {
            REG_WRITE(tu_phy_tbl[i].addr, tu_phy_tbl[i].val);
        }

        /* 加载 PHY 高功率补丁 */
        int phy_hp_count =
            static_cast<int>(sizeof(tu_phy_hp_tbl) / sizeof(tu_phy_hp_tbl[0]));
        logger_->info("  正在加载 PHY 高功率补丁 ({} 条目)...", phy_hp_count);
        for (int i = 0; i < phy_hp_count; i++) {
            REG_WRITE(tu_phy_hp_tbl[i].addr, tu_phy_hp_tbl[i].val);
        }

        /* 应用初始化补丁 */
        logger_->info("  应用初始化补丁 [0xccb0e134 => 0x00100010]...");
        REG_WRITE(0xccb0e134, 0x00100010);

        /* 5G BB gain 设置 */
        REG_WRITE(ADR_TU_WIFI_PADPD_5G_BB_GAIN, 0x80808080);

        /* 设置混杂模式过滤器覆盖 */
        for (int i = 0; i < 9; i++)
            REG_WRITE(ADR_MRX_FLT_EN0 + static_cast<uint32_t>(i * 4), 0x0000);

        for (int i = 0; i < 12; i++)
            REG_WRITE(ADR_MRX_FLT_TB0 + static_cast<uint32_t>(i * 4), 0x0000FFF0);

        REG_WRITE(ADR_MRX_FLT_TB13, MRX_MODE_PROMISCUOUS);

        for (int i = 14; i < 16; i++)
            REG_WRITE(ADR_MRX_FLT_TB0 + static_cast<uint32_t>(i * 4), 0x0000FFF0);

        /* This must be the final RF/PHY initialization action.  In r3408 the
         * load_phy_table/load_rf_table HAL hooks are empty and firmware owns the
         * actual calibration.  Sending this before the compatibility tables above
         * caused those writes to overwrite the freshly calibrated state. */
        logger_->info("  请求固件执行最终 PLL/RF/PHY 校准 ({} bytes)...",
                      sizeof(cali));
        int fw_init_ret = usb_.send_host_cmd(SSV6XXX_HOST_CMD_RFPHY_OPS,
                                             SSV6XXX_RFPHY_CMD_INIT_PLL_PHY_RF, &cali,
                                             sizeof(cali), true);
        if (fw_init_ret < 0) {
            throw InitException("固件 INIT_PLL_PHY_RF 命令失败");
        }

        logger_->info("  PHY/RF 校准表 + PLL 初始化完成");
        current_phase_ = InitPhase::PHY_RF;
    }

    /* ==================== 阶段 8: 信道设置 ==================== */

    void ChipInitializer::set_channel(uint8_t channel, ChannelBandwidth bandwidth)
    {
        logger_->info("=== 阶段 8: 信道设置 ===");
        logger_->info("  由固件设置信道 {} ({})...", channel, bw_name(bandwidth));

        /* r3408 TurismoC HAL_SET_CHANNEL is firmware-only.  Do not also drive the
         * synthesizer, PLL mode and RG_MODE registers from the host: that races the
         * firmware RF state machine and prevents MAC_PHY_TRX_EN from synchronizing.
         */
        set_channel_via_firmware(channel, static_cast<uint8_t>(bandwidth));
        apply_rx_promiscuous(true);

        current_phase_ = InitPhase::CHANNEL;
    }

    void ChipInitializer::set_tx_power_index(uint8_t channel, uint32_t power_index)
    {
        if (channel == 0 || channel > 165) {
            throw InitException("发射功率设置失败：信道超出范围");
        }
        if (power_index > 127) {
            throw InitException("发射功率设置失败：功率索引必须在 0-127 之间");
        }

        /* 固件 RFPHY_CMD_TX_PWR 在当前 ssv6x5x-sw.bin 上无响应。
         * 与内核 turismoC 一致：直接写 ADR_MODE_REGISTER 的
         * RG_TX_GAIN[30:24] + RG_TX_GAIN_MANUAL(bit5)。 */
        constexpr uint32_t ADR_MODE_REGISTER = 0xCCB0A400;
        constexpr uint32_t RG_TX_GAIN_MSK    = 0x7F000000u;
        constexpr uint32_t RG_TX_GAIN_SFT    = 24u;
        constexpr uint32_t RG_TX_GAIN_MANUAL = 1u << 5;

        uint32_t mode = 0;
        if (usb_.read_reg(ADR_MODE_REGISTER, &mode) < 0) {
            throw InitException("读取 MODE_REGISTER 失败");
        }
        const uint32_t before = mode;
        mode &= ~RG_TX_GAIN_MSK;
        mode |= ((power_index & 0x7Fu) << RG_TX_GAIN_SFT);
        mode |= RG_TX_GAIN_MANUAL;

        logger_->info(
            "  设置 {} TX 增益索引 {} (MODE_REGISTER 0x{:08X} -> 0x{:08X})...",
            channel <= 14 ? "2.4GHz" : "5GHz", power_index, before, mode);
        if (usb_.write_reg(ADR_MODE_REGISTER, mode) < 0) {
            throw InitException("写入 TX 增益寄存器失败");
        }
        uint32_t check = 0;
        if (usb_.read_reg(ADR_MODE_REGISTER, &check) == 0) {
            const uint32_t now_gain = (check & RG_TX_GAIN_MSK) >> RG_TX_GAIN_SFT;
            logger_->info("  TX 增益回读: index={} manual={}", now_gain,
                          (check & RG_TX_GAIN_MANUAL) != 0);
        }
    }

    void ChipInitializer::set_channel_24g(uint8_t channel, ChannelBandwidth bw)
    {
        logger_->info("  [2.4GHz] 设置信道 {}", channel);

        uint32_t regval, cur_ch;

        REG_WRITE(0xCCB0F3FC, 0x00000000);
        REG_WRITE(0xCCB0EBFC, 0x00000000);
        usleep(1000);

        /* SIFS/SIGEXT 设置 */
        REG_SET_BITS(ADR_MTX_TIME_IFS, 10u << SIFS_SFT, SIFS_MSK);
        REG_SET_BITS(ADR_MTX_TIME_FINETUNE, 6u << SIGEXT_SFT, SIGEXT_MSK);

        /* 设置带宽 */
        set_phy_bandwidth(bw);

        /* 清除 5G 标志 */
        regval = REG_READ(0xCCB0E000);
        regval = (regval & 0xFFFFF7FF);
        REG_WRITE(0xCCB0E000, regval);

        /* 使能合成器 */
        regval = REG_READ(0xCCB0A400);
        regval = (regval & 0xFFFFFFFB) | (1u << 2);
        REG_WRITE(0xCCB0A400, regval);

        /* 设置 2.4G 信道表 */
        regval = REG_READ(ADR_TU_2G_CH_TABLE);
        regval = (regval & 0xFFFFFFF7) | (1u << 3);
        REG_WRITE(ADR_TU_2G_CH_TABLE, regval);

        /* 处理当前信道等于目标信道的情况 */
        regval = REG_READ(ADR_TU_2G_CH_TABLE);
        cur_ch = (regval & 0x7F800) >> 11;
        if (cur_ch == channel) {
            uint8_t alt = (channel == 1) ? 11 : 1;
            regval      = (regval & 0xFFF807FF) | (static_cast<uint32_t>(alt) << 11);
            REG_WRITE(ADR_TU_2G_CH_TABLE, regval);
        }

        usleep(100);

        /* 写入目标信道 */
        regval = REG_READ(ADR_TU_2G_CH_TABLE);
        regval = (regval & 0xFFF807FF) | (static_cast<uint32_t>(channel) << 11);
        REG_WRITE(ADR_TU_2G_CH_TABLE, regval);

        /* PLL 模式序列: mode 0 → mode 3 */
        regval = REG_READ(0xCCB0A400);
        regval = (regval & 0xFFFFF8FF) | (0u << 8);
        REG_WRITE(0xCCB0A400, regval);

        regval = REG_READ(0xCCB0A400);
        regval = (regval & 0xFFFFF8FF) | (3u << 8);
        REG_WRITE(0xCCB0A400, regval);

        /* 禁用合成器 */
        regval = REG_READ(0xCCB0A400);
        regval = (regval & 0xFFFFFFFB) | (0u << 2);
        REG_WRITE(0xCCB0A400, regval);

        /* 锁定 PLL */
        regval = REG_READ(0xCCB0F3FC);
        regval = (regval & 0xFFFFFFFE) | (1u << 0);
        REG_WRITE(0xCCB0F3FC, regval);

        regval = REG_READ(0xCCB0EBFC);
        regval = (regval & 0xFFFFFFFE) | (1u << 0);
        REG_WRITE(0xCCB0EBFC, regval);

        /* 通知固件新信道 */
        REG_WRITE(ADR_CH_STA_PRI, 0x00000213);

        /* 诊断输出 */
        {
            uint32_t wdog1 = REG_READ(ADR_MCU_WDOG_REG);
            usleep(100000);
            uint32_t wdog2 = REG_READ(ADR_MCU_WDOG_REG);

            uint32_t mode_val = REG_READ(ADR_TU_MODE_REG);
            uint32_t phy_val  = REG_READ(ADR_TU_PHY_ENABLE);
            uint32_t ctrl_val = REG_READ(ADR_CONTROL);
            uint32_t flt_en0  = REG_READ(ADR_MRX_FLT_EN0);
            uint32_t flt_tb13 = REG_READ(ADR_MRX_FLT_TB13);
            uint32_t sys_val  = REG_READ(ADR_WIFI_PHY_COMMON_SYS_REG);
            uint32_t add_on0  = REG_READ(ADR_DIGITAL_ADD_ON_0);

            logger_->info("    WDOG1=0x{:08X} WDOG2=0x{:08X} ({:s})", wdog1, wdog2,
                          (wdog1 != wdog2) ? "计数器变化" : "计数器未变化，仅供参考");
            uint32_t ch_table = REG_READ(ADR_TU_2G_CH_TABLE);
            logger_->info("    MODE=0x{:08X} PHY=0x{:08X} CTRL=0x{:08X}", mode_val,
                          phy_val, ctrl_val);
            logger_->info("    CH_TABLE=0x{:08X}", ch_table);
            logger_->info("    PHY_SYS=0x{:08X} ADD_ON0=0x{:08X}", sys_val, add_on0);
            logger_->info(
                "    MRX_FLT_EN0=0x{:08X} TB13=0x{:08X} (期望 TB13=0x{:02X} 混杂)",
                flt_en0, flt_tb13, MRX_MODE_PROMISCUOUS);
        }

        logger_->info("  2.4GHz 信道已设置到 {} ({})", channel, bw_name(bw));

        set_channel_via_firmware(channel, static_cast<uint8_t>(bw));

        /* 重新应用混杂模式 */
        apply_rx_promiscuous(true);

        /* 启用 RF 收发器 */
        REG_SET_BITS(ADR_CBR_HARD_WIRE_PIN,
                     static_cast<uint32_t>(RF_MODE_TRX_EN) << CBR_RG_MODE_SFT,
                     CBR_RG_MODE_MSK);
        {
            uint32_t cbr_val = REG_READ(ADR_CBR_HARD_WIRE_PIN);
            logger_->info("    CBR_HARD_WIRE_PIN = 0x{:08X} (RF_MODE={:s})", cbr_val,
                          ((cbr_val & CBR_RG_MODE_MSK) >> CBR_RG_MODE_SFT) ==
                                  RF_MODE_TRX_EN
                              ? "TRX_EN"
                              : "??");
        }
    }

    void ChipInitializer::set_channel_5g(uint8_t channel, ChannelBandwidth bw)
    {
        logger_->info("  [5GHz] 设置信道 {}", channel);

        uint32_t regval, cur_ch;

        REG_WRITE(0xCCB0F3FC, 0x00000000);
        REG_WRITE(0xCCB0EBFC, 0x00000000);
        usleep(1000);

        /* SIFS/SIGEXT 设置 */
        REG_SET_BITS(ADR_MTX_TIME_IFS, 16u << SIFS_SFT, SIFS_MSK);
        REG_SET_BITS(ADR_MTX_TIME_FINETUNE, 0, SIGEXT_MSK);

        /* 设置带宽 */
        set_phy_bandwidth(bw);

        /* 设置 5G 标志 */
        regval = REG_READ(0xCCB0E000);
        regval = (regval & 0xFFFFF7FF) | (1u << 11);
        REG_WRITE(0xCCB0E000, regval);

        /* 使能合成器 */
        regval = REG_READ(0xCCB0A400);
        regval = (regval & 0xFFFFFFFB) | (1u << 2);
        REG_WRITE(0xCCB0A400, regval);

        /* 高频段 LDO 偏置调整 (UNII-3, ch149+) */
        if (channel >= 149) {
            constexpr uint32_t ADR_5G_LDO   = 0xCCB0A554;
            constexpr uint32_t LDO_CLR_MASK = (0x7u << 4) | (0x7u << 8) | (0x7u << 12);
            uint32_t ldo_val                = (REG_READ(ADR_5G_LDO) & ~LDO_CLR_MASK) | (6u << 4) |
                               (6u << 8) | (6u << 12);
            logger_->info("    5G LDO 高频段调整 0xCCB0A554 0x{:08X} -> 0x{:08X}",
                          REG_READ(ADR_5G_LDO), ldo_val);
            REG_WRITE(ADR_5G_LDO, ldo_val);
        }

        /* 设置 5G 信道表 */
        regval = REG_READ(ADR_TU_5G_CH_TABLE);
        regval = (regval & 0xFFFFFFEF) | (1u << 4);
        REG_WRITE(ADR_TU_5G_CH_TABLE, regval);

        /* 处理当前信道等于目标信道的情况 */
        regval = REG_READ(ADR_TU_5G_CH_TABLE);
        cur_ch = (regval & 0x0000FF00) >> 8;
        if (cur_ch == channel) {
            uint8_t alt = (channel == 36) ? 40 : 36;
            regval      = (regval & 0xFFFF00FF) | (static_cast<uint32_t>(alt) << 8);
            REG_WRITE(ADR_TU_5G_CH_TABLE, regval);
        }

        usleep(100);

        /* 写入目标信道 */
        regval = REG_READ(ADR_TU_5G_CH_TABLE);
        regval = (regval & 0xFFFF00FF) | (static_cast<uint32_t>(channel) << 8);
        REG_WRITE(ADR_TU_5G_CH_TABLE, regval);

        /* PLL 模式序列: mode 0 → mode 7 */
        regval = REG_READ(0xCCB0A400);
        regval = (regval & 0xFFFFF8FF) | (0u << 8);
        REG_WRITE(0xCCB0A400, regval);

        regval = REG_READ(0xCCB0A400);
        regval = (regval & 0xFFFFF8FF) | (7u << 8);
        REG_WRITE(0xCCB0A400, regval);

        /* 禁用合成器 */
        regval = REG_READ(0xCCB0A400);
        regval = (regval & 0xFFFFFFFB) | (0u << 2);
        REG_WRITE(0xCCB0A400, regval);

        /* 锁定 PLL */
        regval = REG_READ(0xCCB0F3FC);
        regval = (regval & 0xFFFFFFFE) | (1u << 0);
        REG_WRITE(0xCCB0F3FC, regval);

        regval = REG_READ(0xCCB0EBFC);
        regval = (regval & 0xFFFFFFFE) | (1u << 0);
        REG_WRITE(0xCCB0EBFC, regval);

        /* 确保 CPU 时钟已启用 */
        {
            uint32_t clk_en;
            if (usb_.read_reg(ADR_PLATFORM_CLOCK_ENABLE, &clk_en) == 0) {
                logger_->info("    5G CLOCK_ENABLE=0x{:08X}", clk_en);
                if (!(clk_en & (1u << 24))) {
                    logger_->info("    5G CPU 时钟已关闭，正在启用...");
                    usb_.write_reg(ADR_PLATFORM_CLOCK_ENABLE, clk_en | (1u << 24));
                }
            }
        }

        /* 通知固件新信道 */
        REG_WRITE(ADR_CH_STA_PRI, 0x00000213);

        /* 诊断输出 */
        {
            uint32_t wdog1 = REG_READ(ADR_MCU_WDOG_REG);
            usleep(100000);
            uint32_t wdog2 = REG_READ(ADR_MCU_WDOG_REG);

            logger_->info("    WDOG1=0x{:08X} WDOG2=0x{:08X} ({:s})", wdog1, wdog2,
                          (wdog1 != wdog2) ? "计数器变化" : "计数器未变化，仅供参考");

            uint32_t ch_table = REG_READ(ADR_TU_5G_CH_TABLE);
            logger_->info("    CH_TABLE=0x{:08X}", ch_table);
        }

        logger_->info("  5GHz 信道已设置到 {} ({})", channel, bw_name(bw));

        set_channel_via_firmware(channel, static_cast<uint8_t>(bw));

        /* 重新应用混杂模式 */
        apply_rx_promiscuous(true);

        /* 启用 RF 收发器 */
        REG_SET_BITS(ADR_CBR_HARD_WIRE_PIN,
                     static_cast<uint32_t>(RF_MODE_TRX_EN) << CBR_RG_MODE_SFT,
                     CBR_RG_MODE_MSK);
    }

    /* ==================== 阶段 9: PHY 启用（最后一步） ==================== */

    void ChipInitializer::enable_phy()
    {
        logger_->info("=== 阶段 9: PHY 启用 ===");

        /* ★ 匹配内核 ssv6xxx_init_hw() 中 HAL_SET_PHY_MODE(sh, true) 的位置
         * 在内核中，PHY 启用是在信道设置之后才执行的：
         *   HAL_SET_CHANNEL_CHECK() → ... → HAL_SET_PHY_MODE(sh, true)
         *
         * 之前用户空间驱动在固件下载后立即启用 PHY（阶段 6），
         * 这导致 PHY 在未校准的状态下运行，可能引起写入失败。
         * 现在移到信道设置之后，确保 PHY 在所有校准完成后才启用。 */

        /* Match r3408 exactly:
         * 1. HAL_SET_PHY_MODE writes sub-mode bits only (0x17e, bit0 stays clear).
         * 2. SSV_PHY_ENABLE asks firmware to assert the master PHY enable.
         * 3. mac80211 start asks firmware to enable RF.
         * A direct 0x17f register write is not equivalent: firmware owns the
         * RF/PHY state transition and the MAC-PHY synchronization handshake. */
        constexpr uint32_t TU_PHY_SUBMODES_EN = TU_PHY_ALL_EN & ~TU_PHY_MD_EN;
        logger_->info("  配置 PHY 子模式: 0x{:08X}...", TU_PHY_SUBMODES_EN);
        REG_WRITE(ADR_TU_PHY_ENABLE, TU_PHY_SUBMODES_EN);

        logger_->info("  请求固件启用 PHY 主开关...");
        int ret = usb_.send_host_cmd(SSV6XXX_HOST_CMD_RFPHY_OPS,
                                     SSV6XXX_RFPHY_CMD_PHY_ENABLE, nullptr, 0, true);
        if (ret < 0)
            throw InitException("固件 PHY_ENABLE 命令失败");

        logger_->info("  请求固件启用 RF...");
        ret = usb_.send_host_cmd(SSV6XXX_HOST_CMD_RFPHY_OPS,
                                 SSV6XXX_RFPHY_CMD_RF_ENABLE, nullptr, 0, true);
        if (ret < 0)
            throw InitException("固件 RF_ENABLE 命令失败");

        {
            uint32_t phy_check = REG_READ(ADR_TU_PHY_ENABLE);
            logger_->info("    PHY_ENABLE 寄存器: 0x{:08X} (期望 0x{:08X})", phy_check,
                          TU_PHY_ALL_EN);
        }

        usleep(50000);

        /* 验证 PHY 启用后的状态 */
        {
            uint32_t mode_val = REG_READ(ADR_TU_MODE_REG);
            uint32_t phy_val  = REG_READ(ADR_TU_PHY_ENABLE);
            uint32_t ctrl_val = REG_READ(ADR_CONTROL);
            logger_->info("    MODE=0x{:08X} PHY=0x{:08X} CTRL=0x{:08X}", mode_val,
                          phy_val, ctrl_val);
        }

        /* 记录 WDOG 辅助诊断；固件已通过前面的阻塞式 HCI 响应验证。 */
        {
            uint32_t wdog1, wdog2;
            usb_.read_reg(ADR_MCU_WDOG_REG, &wdog1);
            usleep(100000);
            usb_.read_reg(ADR_MCU_WDOG_REG, &wdog2);
            logger_->info("    WDOG: 0x{:08X} -> 0x{:08X} ({:s})", wdog1, wdog2,
                          (wdog1 != wdog2) ? "计数器变化" : "计数器未变化，仅供参考");
        }

        logger_->info("  PHY 启用完成");
        current_phase_ = InitPhase::PHY_ENABLE;
    }

    void ChipInitializer::set_phy_bandwidth(ChannelBandwidth bw)
    {
        uint32_t sys_set    = 0;
        uint32_t add_on_set = 0;
        uint32_t mtx_set    = 0;
        uint32_t mtx_clr    = MTX_BLOCKTX_IGNORE_TOMAC_CCA_ED_PRIMARY_MSK;

        switch (bw) {
            case ChannelBandwidth::HT40_MINUS:
                mtx_clr |= MTX_BLOCKTX_IGNORE_TOMAC_CCA_ED_SECONDARY_MSK;
                sys_set    = RG_PRIMARY_CH_SIDE_MSK | RG_SYSTEM_BW_MSK;
                add_on_set = RG_40M_MODE_MSK;
                break;
            case ChannelBandwidth::HT40_PLUS:
                mtx_clr |= MTX_BLOCKTX_IGNORE_TOMAC_CCA_ED_SECONDARY_MSK;
                sys_set    = RG_SYSTEM_BW_MSK;
                add_on_set = RG_40M_MODE_MSK | RG_LO_UP_CH_MSK;
                break;
            case ChannelBandwidth::HT20:
            default:
                mtx_set = MTX_BLOCKTX_IGNORE_TOMAC_CCA_ED_SECONDARY_MSK;
                break;
        }

        logger_->info("    PHY 带宽: {}", bw_name(bw));
        REG_SET_BITS(ADR_MTX_MISC_EN, mtx_set, mtx_clr);
        REG_SET_BITS(ADR_WIFI_PHY_COMMON_SYS_REG, sys_set,
                     RG_PRIMARY_CH_SIDE_MSK | RG_SYSTEM_BW_MSK);
        REG_SET_BITS(ADR_DIGITAL_ADD_ON_0, add_on_set,
                     RG_40M_MODE_MSK | RG_LO_UP_CH_MSK);
    }

    /* ==================== 关闭/清理 ==================== */

    void ChipInitializer::shutdown()
    {
        logger_->info("\n=== 关闭芯片 ===");

        /* 切换回 bulk 端点寄存器访问模式 */
        usb_.switch_to_bulk_mode();

        /* 禁用 PHY */
        {
            uint32_t regval;
            if (usb_.read_reg(ADR_TU_PHY_ENABLE, &regval) == 0) {
                usb_.write_reg(ADR_TU_PHY_ENABLE, regval & ~TU_PHY_ALL_EN);
            }
        }

        /* 停止 MCU */
        {
            uint32_t regval;
            if (usb_.read_reg(0xC000001C, &regval) == 0) {
                usb_.write_reg(0xC000001C, regval & 0xFEFFFFFF);
                usb_.read_reg(0xC00000EC, &regval);
                usb_.write_reg(0xC00000EC, regval & 0xFFFFEFFF);
                usb_.read_reg(0xC00000E8, &regval);
                usb_.write_reg(0xC00000E8, regval | 0x00000004);
                usb_.read_reg(0xC000001C, &regval);
                usb_.write_reg(0xC000001C, regval & 0xFEFFFFFF);
            }
        }

        mcu_running_   = false;
        current_phase_ = InitPhase::NONE;
        logger_->info("  芯片关闭序列完成");
    }

    /* ==================== 完整初始化流程 ==================== */

    void ChipInitializer::full_init(const DriverConfig &config)
    {
        logger_->info("========================================");
        logger_->info("  开始完整初始化流程（匹配内核驱动顺序）");
        logger_->info("========================================");

        /* 阶段 1: 芯片 ID 验证 */
        read_chip_id();
        read_efuse_calibration();

        /* 阶段 2: CPU 复位 + ON3 电源域 + USB ROM 就绪 */
        reset_cpu_and_rom();

        /* 阶段 3: MAC 复位（含 USB ACC 控制） */
        mac_reset();

        /* 阶段 4: 硬件配置 */
        hw_config(config);

        /* 阶段 5: 固件下载 + EP0 切换 */
        download_firmware(config);

        /* 阶段 6: 固件验证 */
        verify_firmware();

        /* 阶段 7: PHY/RF 校准表 + PLL 初始化 */
        load_phy_rf_tables(config);

        /* HAL_INI_HW_SEC_PHY_TABLE + ssv6xxx_init_mac: initialize the firmware
         * security table and perform the master PHY reset transition before the
         * firmware calibrates the selected channel. */
        logger_->info("=== 阶段 7.5: 安全表与 PHY 主使能 ===");
        int cmd_ret = usb_.send_host_cmd(SSV6XXX_HOST_CMD_SECURITY,
                                         SSV6XXX_SECURITY_CMD_INIT, nullptr, 0, true);
        if (cmd_ret < 0)
            throw InitException("固件 SECURITY_INIT 命令失败");

        /* ssv6200_start() does not reuse the MAC setup performed before firmware
         * download. It explicitly runs ssv6xxx_init_mac() after the firmware and
         * RF/PHY tables are ready, bracketed by PHY_DISABLE/PHY_ENABLE. Firmware
         * startup changes parts of the MAC table (ADR_CONTROL was observed as
         * 0x10000006 instead of the kernel table's 0x12000006), so skipping this
         * second initialization leaves the MAC/PHY receive handshake inactive. */
        logger_->info("=== 阶段 7.6: 固件后重新初始化 MAC ===");
        cmd_ret = usb_.send_host_cmd(SSV6XXX_HOST_CMD_RFPHY_OPS,
                                     SSV6XXX_RFPHY_CMD_PHY_DISABLE, nullptr, 0, true);
        if (cmd_ret < 0)
            throw InitException("固件 PHY_DISABLE 命令失败");
        hw_config(config);

        /* mac80211 add_interface() notifies firmware before start()/RF_ENABLE.
         * Register the userspace monitor as a STA VIF so firmware keeps the MAC-PHY
         * TRX handshake active even though we receive promiscuously. */
        logger_->info("=== 阶段 7.7: 注册固件 STA VIF ===");
        SsvVifParam vif{};
        std::memcpy(vif.mac, config.mac_addr, sizeof(vif.mac));
        vif.vif_idx = 0;
        vif.type    = SSV6XXX_VIF_TYPE_STA;
        cmd_ret     = usb_.send_host_cmd(SSV6XXX_HOST_CMD_VIF_OPS, SSV6XXX_VIF_CMD_ADD,
                                         &vif, sizeof(vif), true);
        if (cmd_ret < 0)
            throw InitException("固件 VIF_ADD 命令失败");

        /* 阶段 8: 信道设置 */
        set_channel(config.default_channel, config.bandwidth);

        /* 阶段 9: PHY 启用（最后一步，匹配内核 HAL_SET_PHY_MODE） */
        enable_phy();

        current_phase_ = InitPhase::COMPLETED;

        logger_->info("========================================");
        logger_->info("  ✅ 初始化完成！当前阶段: {}",
                      init_phase_name(current_phase_));
        logger_->info("========================================");
    }

    /* ==================== 私有辅助方法 ==================== */

    void ChipInitializer::apply_rx_promiscuous(bool notify_fw)
    {
        logger_->info("  应用 RX 混杂模式...");

        /* 清除过滤器使能寄存器 */
        for (int i = 0; i < 9; i++) {
            usb_.write_reg(ADR_MRX_FLT_EN0 + static_cast<uint32_t>(i * 4), 0);
        }

        /* 设置过滤器表为混杂模式 */
        for (int i = 0; i < 16; i++) {
            usb_.write_reg(ADR_MRX_FLT_TB0 + static_cast<uint32_t>(i * 4), 0x0000FFF0);
        }

        /* 设置 TB13 为混杂模式 */
        usb_.write_reg(ADR_MRX_FLT_TB13, MRX_MODE_PROMISCUOUS);

        /* 验证 */
        {
            uint32_t tb13 = 0;
            if (usb_.read_reg(ADR_MRX_FLT_TB13, &tb13) == 0) {
                logger_->info("  MRX_FLT_TB13=0x{:08X} ({:s})", tb13,
                              (tb13 == MRX_MODE_PROMISCUOUS) ? "混杂模式" : "意外值");
            }
        }

        if (notify_fw) {
            logger_->info("  HOST_CMD_MRX_MODE: 混杂模式...");
            int ret = usb_.send_host_cmd(SSV6XXX_HOST_CMD_MRX_MODE,
                                         SSV6XXX_MRX_PROMISCUOUS, nullptr, 0, false);
            if (ret < 0) {
                logger_->warn("  HOST_CMD_MRX_MODE 发送失败: {}", ret);
            }
        }
    }

    void ChipInitializer::wait_for_reg(uint32_t addr, uint32_t mask,
                                       uint32_t target, unsigned max_attempts,
                                       const char *reg_name)
    {
        for (unsigned i = 0; i < max_attempts; i++) {
            uint32_t val = REG_READ(addr);
            if ((val & mask) == target) {
                if (reg_name) {
                    logger_->info("  {}: 成功 ({} 次)", reg_name, i);
                }
                return;
            }
            usleep(50); // 50ms 间隔
        }

        if (reg_name) {
            uint32_t final_val;
            usb_.read_reg(addr, &final_val);
            char buf[256];
            snprintf(buf, sizeof(buf),
                     "%s: 超时 (%u 次), 最终值=0x%08X, 期望掩码=0x%08X, 目标=0x%08X",
                     reg_name, max_attempts, final_val, mask, target);
            throw InitException(std::string(buf));
        }
    }

    /* 占位符方法（未来扩展）*/

    void ChipInitializer::control_rf_via_firmware()
    {
        logger_->info("  通过固件发送 INIT_PLL_PHY_RF 命令...");
        logger_->info("  (当前路径暂不主动发送 INIT_PLL_PHY_RF)");
    }

    void ChipInitializer::set_channel_via_firmware(uint8_t channel,
                                                   uint8_t chan_type)
    {
        uint8_t nl_chan_type = NL80211_CHAN_HT20;
        switch (static_cast<ChannelBandwidth>(chan_type)) {
            case ChannelBandwidth::HT40_MINUS:
                nl_chan_type = NL80211_CHAN_HT40MINUS;
                break;
            case ChannelBandwidth::HT40_PLUS:
                nl_chan_type = NL80211_CHAN_HT40PLUS;
                break;
            case ChannelBandwidth::HT20:
            default:
                break;
        }
        logger_->info("  通过固件设置信道 (ch={}, type={})...", channel,
                      nl_chan_type);
        SsvRfChan payload{};
        payload.chan      = channel;
        payload.chan_type = nl_chan_type;

        /* The kernel marks RFPHY_CMD_CHAN as blocking.  Wait until firmware has
         * completed calibration before enabling PHY/RX. */
        int ret =
            usb_.send_host_cmd(SSV6XXX_HOST_CMD_RFPHY_OPS, SSV6XXX_RFPHY_CMD_CHAN,
                               &payload, sizeof(payload), true);
        if (ret < 0) {
            logger_->warn("  RFPHY_CMD_CHAN 发送失败: {}", ret);
        }
    }

    void ChipInitializer::calibrate_5g_rx(uint8_t target_channel)
    {
        logger_->info("  运行 5G RX 校准 (ch{})...", target_channel);
        /* TODO: 实现 5G RX 校准逻辑 */
    }

} // namespace ssv6xxx

/* 取消宏定义 */
#undef REG_WRITE
#undef REG_SET_BITS
#undef REG_READ
