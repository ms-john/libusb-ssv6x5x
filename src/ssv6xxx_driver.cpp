/*
 * ssv6xxx_driver.cpp - SSV6X5X 驱动公共 API 门面类实现 (C++14)
 *
 * 实现门面模式，组合三个核心子模块：
 *   - UsbTransport: USB 设备通信层
 *   - ChipInitializer: 7 阶段硬件初始化器
 *   - MonitorSession: 帧接收/注入会话
 *
 * 主要职责：
 *   1. 管理子模块的生命周期（构造/析构）
 *   2. 协调初始化流程（init → start_monitor → poll/rx_loop → shutdown）
 *   3. 状态验证（确保操作在正确的状态下执行）
 *   4. 接口转发（将高层 API 转发到对应子模块）
 *   5. 异常处理（统一包装和记录异常）
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

#include <spdlog/spdlog.h>

#include "ssv6xxx_chip_init.hpp"
#include "ssv6xxx_driver.hpp"
#include "ssv6xxx_monitor_session.hpp"
#include "ssv6xxx_regs.hpp"
#include "ssv6xxx_usb_transport.hpp"

namespace ssv6xxx
{

    namespace
    {
        constexpr uint8_t kEdccaAverageTimeCode25Us = 2;
        /** @brief 将 EDCCA 平均周期编码换算为参考驱动使用的近似微秒数。 */
        uint32_t edcca_sample_duration_us(uint8_t code)
        {
            static constexpr uint32_t kDurationsUs[] = {
                6,
                12,
                25,
                51,
                102,
                204,
                409,
                819,
            };
            return kDurationsUs[code & 0x07U];
        }

        /** @brief 返回单调时钟微秒值，供跨线程诊断快照关联采样时刻。 */
        uint64_t monotonic_time_us()
        {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::steady_clock::now().time_since_epoch())
                                             .count());
        }
    } // namespace

    /* ==================== 构造/析构 ==================== */

    SSV6xxxDriver::SSV6xxxDriver()
        : SSV6xxxDriver(spdlog::default_logger())
    {
    }

    SSV6xxxDriver::SSV6xxxDriver(std::shared_ptr<spdlog::logger> logger)
        : logger_(std::move(logger))
    {
        if (!logger_) {
            throw std::invalid_argument("SSV6xxxDriver: logger 不能为空");
        }

        logger_->info("[DRIVER] 构造驱动实例");

        /* 创建子模块对象（但不执行初始化） */
        try {
            usb_ = std::make_unique<UsbTransport>(logger_);
            logger_->info("[DRIVER] USB 传输层已创建");
        } catch (const std::exception &e) {
            logger_->error("[DRIVER] 创建 UsbTransport 失败: {}", e.what());
            throw DriverException("无法创建 USB 传输层: " + std::string(e.what()));
        }

        /* ChipInitializer 和 MonitorSession 将在 init() 时延迟创建 */
        /* 这是因为它们依赖 UsbTransport 的引用，而 UsbTransport 需要先 open() */
    }

    SSV6xxxDriver::~SSV6xxxDriver()
    {
        logger_->info("[DRIVER] 析构驱动实例");

        /* RAII: 自动清理所有资源 */
        try {
            /* 步骤 1: 停止监控（如果正在运行） */
            if (monitor_) {
                logger_->info("[DRIVER] 自动停止监控...");
                monitor_->stop();
            }

            /* 步骤 2: 关闭芯片（如果已初始化） */
            if (initialized_) {
                logger_->info("[DRIVER] 自动关闭芯片...");
                if (chip_) {
                    chip_->shutdown();
                }
                initialized_ = false;
            }

            /* 步骤 3: 关闭 USB 设备 */
            if (usb_ && usb_->is_open()) {
                logger_->info("[DRIVER] 自动关闭 USB 设备...");
                usb_->close();
            }

            /* 步骤 4: 销毁子模块对象（unique_ptr 自动释放） */
            monitor_.reset();
            chip_.reset();
            usb_.reset();

            logger_->info("[DRIVER] 所有资源已释放");
        } catch (const std::exception &e) {
            /* 析构函数不应抛出异常，仅记录错误 */
            logger_->error("[DRIVER] 析构时发生异常（已忽略）: {}", e.what());
        }
    }

    /* ==================== 核心操作 ==================== */

    void SSV6xxxDriver::init(const DriverConfig &config)
    {
        logger_->info("========================================");
        logger_->info("  SSV6X5X 驱动初始化");
        logger_->info("========================================");

        /* 如果已经初始化过，先关闭 */
        if (initialized_) {
            logger_->warn("[DRIVER] 驱动已初始化，先执行 shutdown()...");
            shutdown();
        }

        try {
            /* 步骤 1: 打开 USB 设备 */
            logger_->info("[DRIVER] 步骤 1/3: 打开 USB 设备...");
            usb_->open(config.usb_device, 0x8065, 0x6000, config.android_fd);

            /* 步骤 2: 创建芯片初始化器并执行完整初始化 */
            logger_->info("[DRIVER] 步骤 2/3: 执行硬件初始化...");

            /* 延迟创建 ChipInitializer（此时 UsbTransport 已打开） */
            chip_ = std::make_unique<ChipInitializer>(*usb_, logger_);

            /* 执行 7 阶段硬件初始化 */
            chip_->full_init(config);

            /* 功率设置放在完整初始化之后，避免覆盖 PHY/RF 校准表和信道配置。 */
            if (config.tx_power_index >= 0) {
                chip_->set_tx_power_index(config.default_channel,
                                          static_cast<uint32_t>(config.tx_power_index));
            }

            /* 步骤 3: 创建监控会话（可选，延迟到 start_monitor()） */
            logger_->info("[DRIVER] 步骤 3/3: 准备监控会话...");
            monitor_ = std::make_unique<MonitorSession>(*usb_, logger_);
            monitor_->set_channel(config.default_channel);
            if (rx_callback_configured_) {
                monitor_->set_callback(rx_callback_, rx_callback_user_data_);
            }

            initialized_ = true;

            logger_->info("========================================");
            logger_->info("  ✅ 驱动初始化完成！");
            logger_->info("  芯片 ID: {}", chip_->chip_id());
            logger_->info("  当前信道: {}", static_cast<int>(config.default_channel));
            logger_->info("========================================");

        } catch (const UsbException &e) {
            logger_->error("[DRIVER] 初始化失败 (USB 错误): {}", e.what());
            /* 清理部分初始化的资源 */
            chip_.reset();
            if (usb_)
                usb_->close();
            throw; // 重新抛出，让调用者处理
        } catch (const InitException &e) {
            logger_->error("[DRIVER] 初始化失败 (初始化错误): {}", e.what());
            /* 清理部分初始化的资源 */
            chip_.reset();
            if (usb_)
                usb_->close();
            throw;
        } catch (const std::exception &e) {
            logger_->error("[DRIVER] 初始化失败 (未知错误): {}", e.what());
            chip_.reset();
            if (usb_)
                usb_->close();
            throw DriverException("驱动初始化失败: " + std::string(e.what()));
        }
    }

    void SSV6xxxDriver::shutdown()
    {
        logger_->info("[DRIVER] 关闭驱动...");

        /* 如果监控正在运行，先停止 */
        if (monitor_) {
            logger_->info("[DRIVER] 停止监控会话...");
            monitor_->stop();
        }

        /* 如果芯片已初始化，执行关闭序列 */
        if (initialized_ && chip_) {
            logger_->info("[DRIVER] 关闭芯片...");
            try {
                chip_->shutdown();
            } catch (const std::exception &e) {
                logger_->warn("[DRIVER] 芯片关闭时出错（继续清理）: {}", e.what());
            }
            initialized_ = false;
        }

        /* 关闭 USB 设备 */
        if (usb_ && usb_->is_open()) {
            logger_->info("[DRIVER] 关闭 USB 设备...");
            usb_->close();
        }

        logger_->info("[DRIVER] 驱动已关闭");
    }

    /* ==================== 信道控制 ==================== */

    void SSV6xxxDriver::set_channel(uint8_t channel, ChannelBandwidth bw)
    {
        ensure_initialized();

        logger_->info("[DRIVER] 切换信道: {} (带宽: {})", channel,
                      bw == ChannelBandwidth::HT20         ? "HT20"
                      : bw == ChannelBandwidth::HT40_MINUS ? "HT40-"
                                                           : "HT40+");

        try {
            chip_->set_channel(channel, bw);

            /* 同步更新 MonitorSession 的信道信息 */
            if (monitor_) {
                monitor_->set_channel(channel);
            }
        } catch (const InitException &e) {
            logger_->error("[DRIVER] 信道切换失败: {}", e.what());
            throw;
        }
    }

    void SSV6xxxDriver::set_tx_power_index(uint32_t power_index)
    {
        ensure_initialized();
        const uint8_t ch = get_current_channel();
        logger_->info("[DRIVER] 设置 TX 功率索引 {} (ch={})", power_index,
                      static_cast<int>(ch));
        try {
            chip_->set_tx_power_index(ch, power_index);
        } catch (const InitException &e) {
            logger_->error("[DRIVER] TX 功率设置失败: {}", e.what());
            throw;
        }
    }

    bool SSV6xxxDriver::reset_channel_survey()
    {
        ensure_initialized();
        uint32_t control = 0;
        if (usb_->read_reg(ADR_WIFI_PHY_COMMON_EDCCA_0, &control) < 0)
            return false;

        control &= ~(RG_EDCCA_AVG_T_MSK | RG_EDCCA_STAT_EN_MSK);
        control |= kEdccaAverageTimeCode25Us;
        if (usb_->write_reg(ADR_WIFI_PHY_COMMON_EDCCA_0, control) < 0)
            return false;
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        return usb_->write_reg(ADR_WIFI_PHY_COMMON_EDCCA_0,
                               control | RG_EDCCA_STAT_EN_MSK) >= 0;
    }

    ChannelSurvey SSV6xxxDriver::read_channel_survey()
    {
        ensure_initialized();
        ChannelSurvey survey;
        const NoiseFloorReading noise = read_noise_floor();
        survey.captured_at_us         = monotonic_time_us();
        survey.noise_floor_reading    = noise;
        survey.noise_floor_valid      = noise.valid;
        survey.raw_noise_power        = noise.raw_power;
        survey.noise_floor_dbm        = noise.dbm;

        uint32_t control = 0;
        uint32_t primary = 0;
        const bool control_read =
            usb_->read_reg(ADR_WIFI_PHY_COMMON_EDCCA_0, &control) >= 0;
        const bool primary_read =
            usb_->read_reg(ADR_WIFI_PHY_COMMON_EDCCA_1, &primary) >= 0;
        if (control_read && primary_read) {
            survey.average_time_code = static_cast<uint8_t>(
                control & RG_EDCCA_AVG_T_MSK);
            survey.sample_duration_us =
                edcca_sample_duration_us(survey.average_time_code);
            survey.period_samples = static_cast<uint16_t>(
                primary & RO_EDCCA_PRIMARY_PRD_MSK);
            survey.busy_samples = static_cast<uint16_t>(
                (primary & RO_PRIMARY_EDCCA_MSK) >> RO_PRIMARY_EDCCA_SFT);
            survey.period_us = static_cast<uint64_t>(survey.period_samples) *
                               survey.sample_duration_us;
            survey.busy_us = static_cast<uint64_t>(survey.busy_samples) *
                             survey.sample_duration_us;
            if ((control & RG_EDCCA_STAT_EN_MSK) == 0) {
                survey.invalid_reason_mask |= kChannelSurveyInvalidEdccaDisabled;
            }
            if (survey.period_samples == 0) {
                survey.invalid_reason_mask |= kChannelSurveyInvalidEdccaNoSamples;
            }
            if (survey.busy_samples > survey.period_samples) {
                survey.invalid_reason_mask |= kChannelSurveyInvalidEdccaInconsistent;
            }
            const auto edcca_invalid_reasons = survey.invalid_reason_mask & (kChannelSurveyInvalidEdccaDisabled | kChannelSurveyInvalidEdccaNoSamples | kChannelSurveyInvalidEdccaInconsistent);
            survey.edcca_valid               = edcca_invalid_reasons == 0;
        } else {
            if (!control_read) {
                survey.read_error_mask |= kChannelSurveyErrorEdccaControl;
            }
            if (!primary_read) {
                survey.read_error_mask |= kChannelSurveyErrorEdccaCounters;
            }
        }

        uint32_t secondary = 0;
        if (usb_->read_reg(ADR_WIFI_PHY_COMMON_EDCCA_2, &secondary) >= 0) {
            survey.secondary_period_samples = static_cast<uint16_t>(
                secondary & RO_EDCCA_SECONDARY_PRD_MSK);
            survey.secondary_busy_samples = static_cast<uint16_t>(
                (secondary & RO_SECONDARY_EDCCA_MSK) >>
                RO_SECONDARY_EDCCA_SFT);
            survey.secondary_busy_us =
                static_cast<uint64_t>(survey.secondary_busy_samples) *
                survey.sample_duration_us;
        }

        uint32_t fcs_success = 0;
        uint32_t fcs_errors  = 0;
        const bool fcs_success_read =
            usb_->read_reg(ADR_MRX_FCS_SUCC, &fcs_success) >= 0;
        const bool fcs_errors_read =
            usb_->read_reg(ADR_MRX_FCS_ERR, &fcs_errors) >= 0;
        if (fcs_success_read && fcs_errors_read) {
            survey.fcs_success = fcs_success & 0xFFFFU;
            survey.fcs_errors  = fcs_errors & 0xFFFFU;
            survey.fcs_valid   = true;
        } else {
            if (!fcs_success_read) {
                survey.read_error_mask |= kChannelSurveyErrorFcsSuccess;
            }
            if (!fcs_errors_read) {
                survey.read_error_mask |= kChannelSurveyErrorFcsErrors;
            }
            survey.invalid_reason_mask |= kChannelSurveyInvalidFcsUnavailable;
        }
        return survey;
    }

    NoiseFloorReading SSV6xxxDriver::read_noise_floor()
    {
        ensure_initialized();
        NoiseFloorReading reading;
        uint32_t phy_measurement = 0;
        if (usb_->read_reg(ADR_WIFI_11GN_RX_REG_246, &phy_measurement) < 0) {
            return reading;
        }

        const auto raw_power = static_cast<std::uint8_t>(
            (phy_measurement & RO_11GN_NOISE_PWR_MSK) >> RO_11GN_NOISE_PWR_SFT);
        if (raw_power == 0) {
            return reading;
        }

        // 芯片寄存器保存的是正数幅度，参考驱动将其解释为负 dBm；零值表示无效。
        reading.valid     = true;
        reading.raw_power = raw_power;
        reading.dbm       = static_cast<std::int8_t>(-static_cast<int>(raw_power));
        return reading;
    }

    HardwareHealthCounters SSV6xxxDriver::read_hardware_health()
    {
        ensure_initialized();
        HardwareHealthCounters health;
        health.captured_at_us = monotonic_time_us();

        const auto read_counter = [this, &health](uint32_t address,
                                                  uint32_t &target,
                                                  uint32_t error_bit) {
            if (usb_->read_reg(address, &target) < 0) {
                health.read_error_mask |= error_bit;
            }
        };
        read_counter(ADR_MRX_FCS_SUCC,
                     health.fcs_success,
                     kHardwareHealthErrorFcsSuccess);
        read_counter(ADR_MRX_FCS_ERR,
                     health.fcs_errors,
                     kHardwareHealthErrorFcsErrors);
        read_counter(ADR_MRX_ALC_FAIL,
                     health.alc_fail,
                     kHardwareHealthErrorAlcFail);
        read_counter(ADR_MRX_MISS, health.miss, kHardwareHealthErrorMiss);
        read_counter(ADR_RX_PACKET_LENGTH_STATUS,
                     health.rx_packet_length_status,
                     kHardwareHealthErrorRxPacketLength);
        read_counter(ADR_RX_HOST_EVENT_COUNT,
                     health.host_event_count,
                     kHardwareHealthErrorHostEvent);
        read_counter(ADR_RX_FLOW_DATA,
                     health.rx_flow_data,
                     kHardwareHealthErrorRxFlow);
        read_counter(ADR_WIFI_PHY_COMMON_RX_EN_CNT_REG,
                     health.phy_rx_enable_count,
                     kHardwareHealthErrorPhyRxEnable);
        read_counter(ADR_WIFI_PHY_COMMON_TOP_STATUS_RO,
                     health.phy_top_status,
                     kHardwareHealthErrorPhyStatus);
        read_counter(ADR_WIFI_PHY_COMMON_MAC_IF_CNT_RO,
                     health.phy_mac_if_count,
                     kHardwareHealthErrorPhyMacIf);
        health.valid = health.read_error_mask == 0;
        return health;
    }

    int8_t SSV6xxxDriver::get_noise_floor()
    {
        const NoiseFloorReading reading = read_noise_floor();
        if (!reading.valid) {
            throw UsbException(
                "PHY 噪声功率寄存器没有有效数据，请先启动监控并等待 11g/n 接收测量");
        }
        return reading.dbm;
    }

    /* ==================== 监控模式 ==================== */

    void SSV6xxxDriver::start_monitor()
    {
        ensure_initialized();

        if (!monitor_) {
            throw InvalidStateException("监控会话未创建");
        }

        if (monitor_->is_running()) {
            logger_->warn("[DRIVER] 监控已在运行中，忽略重复调用");
            return;
        }

        logger_->info("[DRIVER] 启动监控模式...");

        try {
            monitor_->start();
            logger_->info("[DRIVER] 监控模式已启动");
        } catch (const UsbException &e) {
            logger_->error("[DRIVER] 启动监控失败: {}", e.what());
            throw;
        }
    }

    void SSV6xxxDriver::stop_monitor()
    {
        if (!monitor_) {
            return; // 幂等操作：未启动则直接返回
        }

        logger_->info("[DRIVER] 停止监控模式...");
        monitor_->stop();
        logger_->info("[DRIVER] 监控模式已停止");
    }

    void SSV6xxxDriver::request_stop() noexcept
    {
        if (monitor_) {
            monitor_->request_stop();
        }
    }

    bool SSV6xxxDriver::is_monitoring() const noexcept
    {
        return (monitor_ && monitor_->is_running());
    }

    /* ==================== 帧接收轮询 ==================== */

    int SSV6xxxDriver::poll(int timeout_ms)
    {
        ensure_monitoring();

        return monitor_->poll(timeout_ms);
    }

    void SSV6xxxDriver::rx_loop(int timeout_ms)
    {
        ensure_monitoring();

        logger_->info("[DRIVER] 进入阻塞式接收循环 (timeout={}ms)...", timeout_ms);
        monitor_->rx_loop(timeout_ms);
        logger_->info("[DRIVER] 接收循环已退出");
    }

    /* ==================== 帧注入 ==================== */

    void SSV6xxxDriver::inject_frame(const uint8_t *frame, int frame_len,
                                     int rate_idx)
    {
        ensure_initialized();

        if (!monitor_) {
            throw InvalidStateException("监控会话未创建");
        }

        logger_->debug("[DRIVER] 注入帧: {} bytes, rate_idx={}", frame_len, rate_idx);

        try {
            monitor_->inject_frame(frame, frame_len, rate_idx);
        } catch (const UsbException &e) {
            logger_->error("[DRIVER] 帧注入失败: {}", e.what());
            throw;
        } catch (const std::invalid_argument &e) {
            logger_->error("[DRIVER] 帧参数无效: {}", e.what());
            throw;
        }
    }

    /* ==================== 回调与事件 ==================== */

    void SSV6xxxDriver::set_rx_callback(RxFrameCallback cb, void *user_data)
    {
        // 先保存结构化回调，使 init() 前注册和重新初始化后的行为一致。
        rx_callback_            = std::move(cb);
        rx_callback_user_data_  = user_data;
        rx_callback_configured_ = true;
        if (monitor_) {
            monitor_->set_callback(rx_callback_, rx_callback_user_data_);
            logger_->info("[DRIVER] RX 回调已设置{}",
                          rx_callback_ ? "" : "（已清除）");
        } else {
            logger_->debug("[DRIVER] RX 回调已保存，将在 init() 创建监控会话后生效");
        }
    }

    /* ==================== PCap 输出 ==================== */

    void SSV6xxxDriver::open_pcap(const std::string &filepath)
    {
        if (!monitor_) {
            throw InvalidStateException("监控会话未创建");
        }

        logger_->info("[DRIVER] 打开 PCap 文件: {}", filepath);

        try {
            monitor_->open_pcap(filepath);
            logger_->info("[DRIVER] PCap 文件已打开");
        } catch (const std::runtime_error &e) {
            logger_->error("[DRIVER] 无法打开 PCap 文件: {}", e.what());
            throw;
        }
    }

    void SSV6xxxDriver::close_pcap()
    {
        if (!monitor_ || !monitor_->is_pcap_open()) {
            return; // 幂等操作
        }

        logger_->info("[DRIVER] 关闭 PCap 文件...");
        monitor_->close_pcap();
        logger_->info("[DRIVER] PCap 文件已关闭");
    }

    bool SSV6xxxDriver::is_pcap_open() const noexcept
    {
        return (monitor_ && monitor_->is_pcap_open());
    }

    /* ==================== 统计信息 ==================== */

    MonitorStats SSV6xxxDriver::get_stats() const noexcept
    {
        if (!monitor_) {
            return MonitorStats{}; // 返回空统计
        }
        return monitor_->get_stats();
    }

    void SSV6xxxDriver::reset_stats()
    {
        if (monitor_) {
            monitor_->reset_stats();
            logger_->info("[DRIVER] 统计计数器已重置");
        }
    }

    void SSV6xxxDriver::print_stats() const
    {
        if (!monitor_) {
            logger_->warn("[DRIVER] 监控会话不存在，无法打印统计");
            return;
        }

        monitor_->print_stats();
    }

    /* ==================== 状态查询 ==================== */

    std::string SSV6xxxDriver::get_chip_id() const
    {
        if (!chip_) {
            return ""; // 未初始化
        }
        return chip_->chip_id();
    }

    uint8_t SSV6xxxDriver::get_current_channel() const noexcept
    {
        if (!monitor_) {
            return 0; // 未初始化
        }
        return monitor_->get_channel();
    }

    /* ==================== 私有辅助方法 ==================== */

    void SSV6xxxDriver::ensure_initialized() const
    {
        if (!initialized_) {
            throw InvalidStateException(
                "驱动未初始化。请先调用 init(config) 完成初始化。");
        }
    }

    void SSV6xxxDriver::ensure_monitoring() const
    {
        if (!monitor_ || !monitor_->is_running()) {
            throw InvalidStateException(
                "监控模式未启动。请先调用 start_monitor() 启动监控。");
        }
    }

} // namespace ssv6xxx
