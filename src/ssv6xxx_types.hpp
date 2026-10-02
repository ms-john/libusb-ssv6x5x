/*
 * ssv6xxx_types.hpp - SSV6X5X 类型定义 (C++14)
 *
 * 定义公共 API 的基础类型：
 *   - ChannelBandwidth 枚举（信道带宽）
 *   - DriverConfig 配置结构体（驱动初始化参数）
 *   - RxFrameCallback 回调类型别名（结构化帧接收回调）
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace ssv6xxx
{

    /* ==================== 枚举类型 ==================== */

    /**
     * @brief 信道带宽枚举
     *
     * 用于配置 RF 前端的信道带宽。
     * HT20 是基本模式，HT40 需要额外的辅助信道配置。
     */
    enum class ChannelBandwidth : uint8_t {
        HT20       = 0, ///< 20 MHz 带宽（标准模式）
        HT40_MINUS = 1, ///< 40 MHz 带宽（辅助信道在下方）
        HT40_PLUS  = 2, ///< 40 MHz 带宽（辅助信道在上方）
    };

    /** USB 总线时钟来源。AUTO 会读取芯片当前的时钟选择寄存器。 */
    enum class BusClock : uint8_t {
        AUTO   = 0,
        MHZ_40 = 40,
        MHZ_80 = 80,
    };

    /** 板级稳压器类型。AUTO 会保留并读取 PMU 当前模式。 */
    enum class VoltageRegulator : uint8_t {
        AUTO = 0,
        LDO  = 1,
        DCDC = 2,
    };

    /**
     * @brief USB 设备选择条件
     *
     * port_path（例如 "1.4"）对应物理 USB 端口链路，重新插拔后通常保持
     * 不变；bus/address 会随重新枚举变化，更适合临时调试。未设置任何条件时，
     * 只有系统中恰好存在一张匹配网卡才会自动选择。
     */
    struct UsbDeviceSelector {
        int bus{-1};
        int address{-1};
        std::string port_path;

        bool empty() const noexcept
        {
            return bus < 0 && address < 0 && port_path.empty();
        }
    };

    /* ==================== 配置结构体 ==================== */

    /**
     * @brief 驱动初始化配置
     *
     * 包含 MAC 地址、固件路径、初始信道等参数。
     * 所有字段都有合理的默认值，可以直接使用默认构造。
     *
     * 使用示例：
     * @code
     * // 使用默认配置
     * ssv6xxx::DriverConfig config;
     *
     * // 自定义配置
     * ssv6xxx::DriverConfig config;
     * config.firmware_path = "custom/firmware.bin";
     * config.default_channel = 149;  // 5GHz 信道
     * config.bandwidth = ssv6xxx::ChannelBandwidth::HT40_PLUS;
     * @endcode
     */
    struct DriverConfig {
        /* MAC 地址（6 字节） */
        uint8_t mac_addr[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

        /* 固件文件路径（相对于工作目录） */
        std::string firmware_path = "firmware/ssv6x5x-sw.bin";

        /* 初始信道号（2.4GHz: 1-14, 5GHz: 36-165） */
        uint8_t default_channel = 6;

        /* RF 合成器信道（用于 HT40，0 表示自动计算） */
        uint8_t rf_channel = 0;

        /* 初始带宽 */
        ChannelBandwidth bandwidth = ChannelBandwidth::HT20;

        /* 发射功率索引；-1 表示保留固件/EFUSE 工厂校准默认值。 */
        int tx_power_index = -1;

        /* 目标 USB 设备；多设备环境中必须指定 */
        UsbDeviceSelector usb_device;

        /* Android UsbDeviceConnection 的文件描述符；-1 保留原有设备枚举路径 */
        int android_fd = -1;

        /* 板级总线时钟（AUTO 从硬件当前状态检测） */
        BusClock bus_clock = BusClock::AUTO;

        /* 板级稳压器（AUTO 保留并读取硬件当前状态） */
        VoltageRegulator voltage_regulator = VoltageRegulator::AUTO;

        /* 是否输出详细调试信息 */
        bool verbose = false;
    };

    /** @brief 信道统计寄存器读取失败的位掩码。 */
    enum ChannelSurveyError : std::uint32_t {
        kChannelSurveyErrorNone          = 0,
        kChannelSurveyErrorEdccaControl  = 1u << 0,
        kChannelSurveyErrorEdccaCounters = 1u << 1,
        kChannelSurveyErrorFcsSuccess    = 1u << 2,
        kChannelSurveyErrorFcsErrors     = 1u << 3,
        kChannelSurveyErrorReset         = 1u << 4,
    };

    /** @brief 硬件健康计数器读取失败的位掩码。 */
    enum HardwareHealthError : std::uint32_t {
        kHardwareHealthErrorFcsSuccess     = 1u << 0,
        kHardwareHealthErrorFcsErrors      = 1u << 1,
        kHardwareHealthErrorAlcFail        = 1u << 2,
        kHardwareHealthErrorMiss           = 1u << 3,
        kHardwareHealthErrorRxPacketLength = 1u << 4,
        kHardwareHealthErrorHostEvent      = 1u << 5,
        kHardwareHealthErrorRxFlow         = 1u << 6,
        kHardwareHealthErrorPhyRxEnable    = 1u << 7,
        kHardwareHealthErrorPhyStatus      = 1u << 8,
        kHardwareHealthErrorPhyMacIf       = 1u << 9,
    };

    /** @brief RX PHY 元数据字段有效性位图。 */
    enum RxFrameMetadataField : std::uint32_t {
        kRxMetadataChannel   = 1u << 0,
        kRxMetadataFrequency = 1u << 1,
        kRxMetadataPhy       = 1u << 2,
        kRxMetadataRate      = 1u << 3,
        kRxMetadataRssi      = 1u << 4,
        kRxMetadataSnr       = 1u << 5,
        kRxMetadataMcs       = 1u << 6,
        kRxMetadataBandwidth = 1u << 7,
        kRxMetadataTiming    = 1u << 8,
    };

    /** @brief EDCCA/FCS 统计不可用于评分的原因位图。 */
    enum ChannelSurveyInvalidReason : std::uint32_t {
        kChannelSurveyInvalidNone              = 0,
        kChannelSurveyInvalidEdccaDisabled     = 1u << 0,
        kChannelSurveyInvalidEdccaNoSamples    = 1u << 1,
        kChannelSurveyInvalidEdccaInconsistent = 1u << 2,
        kChannelSurveyInvalidFcsUnavailable    = 1u << 3,
        kChannelSurveyInvalidResetFailed       = 1u << 4,
    };

    /** @brief 噪声底统计的物理单位。UNKNOWN 表示尚未完成硬件标定。 */
    enum class NoiseFloorUnit : std::uint8_t {
        Unknown = 0,
        Dbm     = 1,
    };

    /** @brief 噪声底统计不可用的原因位图。 */
    enum NoiseFloorInvalidReason : std::uint32_t {
        kNoiseFloorInvalidNone         = 0,
        kNoiseFloorInvalidUnavailable  = 1u << 0,
        kNoiseFloorInvalidUncalibrated = 1u << 1,
        kNoiseFloorInvalidSampleCount  = 1u << 2,
    };

    /** @brief PHY 噪声功率寄存器的一次原始读数。 */
    struct NoiseFloorReading {
        bool valid             = false;
        std::uint8_t raw_power = 0;
        std::int8_t dbm        = 0;
    };

    /** @brief 一个扫描窗口的噪声底统计；无真实硬件样本时所有数值保持无效。 */
    struct NoiseFloorStatistics {
        bool valid                        = false;
        NoiseFloorUnit unit               = NoiseFloorUnit::Unknown;
        double average                    = 0.0;
        double percentile50               = 0.0;
        double percentile90               = 0.0;
        double percentile95               = 0.0;
        std::uint64_t sample_count        = 0;
        bool raw_latest_valid             = false;
        std::uint8_t raw_latest           = 0;
        std::uint32_t invalid_reason_mask = kNoiseFloorInvalidUnavailable | kNoiseFloorInvalidUncalibrated;
    };

    /** @brief 监控模式下从硬件 RX 描述符提取的 PHY 元数据。 */
    struct RxFrameMetadata {
        std::uint32_t valid_fields      = 0;
        std::uint8_t channel            = 0;
        std::uint32_t frequency_mhz     = 0;
        std::uint8_t phy_mode           = 0;
        std::uint8_t phy_rate           = 0;
        std::uint8_t rate_mbps          = 0;
        std::int8_t rssi_dbm            = 0;
        std::uint8_t snr                = 0;
        std::int8_t mcs                 = -1;
        std::uint8_t n_ess              = 0;
        std::uint16_t bandwidth_mhz     = 20;
        bool phy_valid                  = false;
        bool short_gi                   = false;
        bool stbc                       = false;
        bool fec                        = false;
        std::uint16_t phy_packet_length = 0;
        std::uint8_t long_rate          = 0;
        std::int16_t frequency_offset   = 0;
        std::uint32_t rx_timestamp_1    = 0;
        std::uint32_t rx_timestamp_2    = 0;
        std::uint16_t sequence          = 0;

        /** @brief 判断指定的 PHY 元数据字段是否可信。 */
        bool has(RxFrameMetadataField field) const noexcept
        {
            return (valid_fields & static_cast<std::uint32_t>(field)) != 0;
        }
    };

    /** @brief RX 帧视图；数据指针仅在回调执行期间有效。 */
    struct RxFrameView {
        const std::uint8_t *data = nullptr;
        int length               = 0;
        RxFrameMetadata metadata;
    };

    /** @brief 一次硬件接收健康状态寄存器快照。 */
    struct HardwareHealthCounters {
        bool valid                            = false;
        std::uint32_t read_error_mask         = 0;
        std::uint32_t fcs_success             = 0;
        std::uint32_t fcs_errors              = 0;
        std::uint32_t alc_fail                = 0;
        std::uint32_t miss                    = 0;
        std::uint32_t rx_packet_length_status = 0;
        std::uint32_t host_event_count        = 0;
        std::uint32_t rx_flow_data            = 0;
        std::uint32_t phy_rx_enable_count     = 0;
        std::uint32_t phy_top_status          = 0;
        std::uint32_t phy_mac_if_count        = 0;
        std::uint64_t captured_at_us          = 0;
    };

    /** @brief 当前信道 EDCCA 能量占用和 PHY 接收计数快照。 */
    struct ChannelSurvey {
        bool edcca_valid                  = false;
        bool fcs_valid                    = false;
        bool noise_floor_valid            = false;
        std::uint32_t read_error_mask     = kChannelSurveyErrorNone;
        std::uint32_t invalid_reason_mask = kChannelSurveyInvalidNone;
        uint8_t average_time_code         = 0;
        uint16_t period_samples           = 0;
        uint16_t busy_samples             = 0;
        uint32_t sample_duration_us       = 0;
        uint64_t period_us                = 0;
        uint64_t busy_us                  = 0;
        uint16_t secondary_period_samples = 0;
        uint16_t secondary_busy_samples   = 0;
        uint64_t secondary_busy_us        = 0;
        uint32_t fcs_success              = 0;
        uint32_t fcs_errors               = 0;
        uint8_t raw_noise_power           = 0;
        int8_t noise_floor_dbm            = 0;
        NoiseFloorReading noise_floor_reading;
        std::uint64_t captured_at_us = 0;
    };

    /* ==================== 回调类型 ==================== */

    /**
     * @brief 帧接收回调函数类型
     *
     * 当接收到完整的 802.11 帧时调用此回调。
     * 回调参数包含帧数据、长度、速率、信号强度和频率信息。
     *
     * @param frame     原始帧视图；数据指针仅在回调执行期间有效
     * @param user_data 用户自定义数据（由 set_rx_callback 时传入）
     *
     * 使用示例：
     * @code
     * ssv6xxx::RxFrameCallback my_cb = [](const ssv6xxx::RxFrameView& frame,
     *                                 void* user_data) {
     *     std::printf("收到 %d 字节, RSSI=%d dBm\n",
     *                 frame.length, frame.metadata.rssi_dbm);
     * };
     * @endcode
     */
    using RxFrameCallback =
        std::function<void(const RxFrameView &frame, void *user_data)>;

} // namespace ssv6xxx
