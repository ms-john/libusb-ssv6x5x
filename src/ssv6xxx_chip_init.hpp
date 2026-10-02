/*
 * ssv6xxx_chip_init.hpp - SSV6X5X 芯片初始化器 (C++14)
 *
 * 封装硬件初始化流程（匹配内核驱动 ssv6xxx_init_hw 顺序）：
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
 * 设计特点：
 *   - 依赖 UsbTransport 进行底层寄存器访问
 *   - 从构造函数注入 logger 和 USB 传输层引用
 *   - 所有错误抛出 InitException 异常
 *   - 支持分阶段初始化或一次性完整初始化
 *
 * 使用示例：
 * @code
 * auto logger = spdlog::stdout_color_mt("chip");
 * ssv6xxx::UsbTransport usb(logger);
 * usb.open();
 *
 * ssv6xxx::ChipInitializer chip(usb, logger);
 * ssv6xxx::DriverConfig config;
 * config.default_channel = 6;
 *
 * try {
 *     chip.full_init(config);  // 执行完整初始化
 *     logger->info("芯片初始化完成！");
 * } catch (const ssv6xxx::InitException& e) {
 *     logger->error("初始化失败: {}", e.what());
 * }
 * @endcode
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <spdlog/spdlog.h>

#include "ssv6xxx_exceptions.hpp"
#include "ssv6xxx_types.hpp"

/* 前向声明 */
namespace ssv6xxx
{
    class UsbTransport;
}

namespace ssv6xxx
{

    /* ==================== 初始化阶段枚举 ==================== */

    /**
     * @brief 硬件初始化阶段标识
     *
     * 用于跟踪当前初始化进度和日志输出。
     */
    enum class InitPhase : uint8_t {
        NONE       = 0,  ///< 未开始
        CHIP_ID    = 1,  ///< 阶段 1: 芯片 ID 验证
        CPU_RESET  = 2,  ///< 阶段 2: CPU 复位 + ON3 + USB ROM 就绪
        MAC_RESET  = 3,  ///< 阶段 3: MAC 复位（USB ACC 控制）
        HW_CONFIG  = 4,  ///< 阶段 4: 硬件配置
        FIRMWARE   = 5,  ///< 阶段 5: 固件下载 + EP0 切换
        FW_VERIFY  = 6,  ///< 阶段 6: 固件验证
        PHY_RF     = 7,  ///< 阶段 7: PHY/RF 校准表 + PLL
        CHANNEL    = 8,  ///< 阶段 8: 信道设置
        PHY_ENABLE = 9,  ///< 阶段 9: PHY 启用（最后一步）
        COMPLETED  = 10, ///< 全部完成
    };

    /**
     * @brief 将 InitPhase 枚举转换为可读字符串
     * @param phase 初始化阶段
     * @return 阶段名称字符串
     */
    const char *init_phase_name(InitPhase phase);

    /* ==================== 芯片初始化器类 ==================== */

    /**
     * @brief 芯片初始化器 - 封装硬件初始化流程（匹配内核驱动顺序）
     *
     * 职责：
     *   1. **芯片 ID 验证**：读取 ADR_CHIP_ID_[0-3]，确认设备身份
     *   2. **CPU 复位**：复位 CPU，禁用/启用 ON3 电源域，等待 USB ROM 就绪
     *   3. **MAC 复位**：禁用 USB ACC → BRG_SW_RST → 启用 USB ACC
     *   4. **硬件配置**：HCI 控制、MMU、TX/RX 阈值、TSF 定时器等
     *   5. **固件下载**：通过 USB 控制端点写入 SRAM + EP0 切换
     *   6. **固件验证**：读取 FW_VERSION_REG，确认固件运行
     *   7. **PHY/RF 校准表**：加载 tu_phy_tbl[] 和 tu_rf_tbl[] + PLL 初始化
     *   8. **信道设置**：2.4GHz vs 5GHz 不同路径，PLL 模式序列
     *   9. **PHY 启用**：写入 TU_PHY_ALL_EN（最后一步）
     *
     * 设计模式：
     *   - **门面模式**：full_init() 提供一站式初始化接口
     *   - **模板方法模式**：每个阶段可以单独调用或组合调用
     *   - **状态跟踪**：通过 current_phase_ 记录当前阶段
     *
     * 重要约束：
     *   - **必须按顺序执行**：阶段顺序匹配内核驱动 ssv6xxx_init_hw()
     *   - **不可跳过关键阶段**：缺少任何阶段都会导致接收失败
     *   - **异常安全**：失败时抛出 InitException，不会留下部分初始化状态
     *
     * 线程安全：否（单线程使用）
     * 异常安全：强异常安全保证
     */
    class ChipInitializer
    {
    public:
        /* ==================== 类型别名 ==================== */

        /**
         * @brief 固件数据读取回调类型
         *
         * 用于从外部获取固件二进制数据。
         * 回调返回的数据指针必须在 ChipInitializer 使用期间保持有效。
         *
         * @param firmware_path 固件文件路径
         * @param [output] data 数据指针（由调用者分配和释放）
         * @param [output] size 数据大小（字节）
         * @return 成功返回 0，失败返回负错误码
         */
        using FirmwareLoader = int (*)(const std::string &firmware_path,
                                       const uint8_t **data, size_t *size);

        /* ==================== 构造/析构 ==================== */

        /**
         * @brief 构造函数
         *
         * 初始化内部状态，但不执行任何硬件操作。
         * 必须显式调用 full_init() 或各阶段方法才开始初始化。
         *
         * @param usb USB 传输层引用（必须已打开且有效）
         * @param logger spdlog 异步 logger（必须非空）
         * @throws std::invalid_argument 如果参数无效
         *
         * @note UsbTransport 的生命周期必须长于 ChipInitializer
         */
        ChipInitializer(UsbTransport &usb, std::shared_ptr<spdlog::logger> logger);

        /**
         * @brief 析构函数
         *
         * 如果初始化未完成，自动执行清理操作（停止 MCU）。
         */
        ~ChipInitializer();

        /* 禁止拷贝和移动 */
        ChipInitializer(const ChipInitializer &)            = delete;
        ChipInitializer &operator=(const ChipInitializer &) = delete;
        ChipInitializer(ChipInitializer &&)                 = delete;
        ChipInitializer &operator=(ChipInitializer &&)      = delete;

        /* ==================== 完整初始化 ==================== */

        /**
         * @brief 执行完整的初始化流程
         *
         * 按顺序执行所有阶段（匹配内核驱动 ssv6xxx_init_hw）：
         *   1. read_chip_id()         - 验证芯片身份
         *   2. reset_cpu_and_rom()    - 复位 CPU + ON3 电源域 + USB ROM 就绪
         *   3. mac_reset()            - 禁用 USB ACC → MAC 复位 → 启用 USB ACC
         *   4. hw_config()            - 配置硬件参数
         *   5. download_firmware()    - 下载固件到 SRAM + EP0 切换
         *   6. verify_firmware()      - 验证固件运行
         *   7. load_phy_rf_tables()   - 加载 PHY/RF 校准表 + PLL 初始化
         *   8. set_channel()          - 设置初始信道
         *   9. enable_phy()           - 启用 PHY（最后一步）
         *
         * @param config 驱动配置（包含 MAC 地址、固件路径、初始信道等）
         *
         * @throws InitException 任何阶段失败时抛出
         * @throws UsbException 底层 USB 操作失败时抛出
         *
         * @warning 此方法会修改芯片的硬件状态，不可逆操作
         * @warning 必须在 UsbTransport::open() 之后调用
         */
        void full_init(const DriverConfig &config);

        /* ==================== 分阶段初始化（高级用法） ==================== */

        /**
         * @brief 阶段 1: 读取并验证芯片 ID
         *
         * 读取 ADR_CHIP_ID_[0-3] 寄存器，组装成人类可读字符串。
         * 验证是否为支持的芯片型号（如 SSV6256P）。
         *
         * @return 芯片 ID 字符串（如 "SSV6256P"）
         *
         * @throws InitException 读取失败或芯片不支持时抛出
         *
         * @note 应在 open() 后立即调用以验证设备身份
         */
        std::string read_chip_id();

        /**
         * @brief 阶段 2: CPU 复位 + ON3 电源域 + USB ROM 就绪
         *
         * 匹配内核驱动 ssv6xxx_init_hw() 的前置步骤：
         *   1. 复位 CPU（RESET_N_CPUN10=0）
         *   2. 禁用 ON3 电源域
         *   3. 启用 ON3 电源域
         *   4. 等待 USB ROM 代码就绪（USB20_HOST_SELRW=1）
         *
         * @throws InitException CPU 复位失败或 ROM 就绪超时时抛出
         */
        void reset_cpu_and_rom();

        /**
         * @brief 阶段 3: MAC 复位序列（含 USB ACC 控制）
         *
         * 匹配内核驱动流程：
         *   1. 禁用 USB ACC EP4（RX 端点访问控制）
         *   2. 禁用 PHY（清除 TU_PHY_ALL_EN 位）
         *   3. 断言 BRG_SW_RST（MAC 软复位）
         *   4. 轮询等待复位释放
         *   5. 设置 MAC 时钟频率
         *   6. 启用 USB ACC EP4
         *
         * @throws InitException 复位超时或失败时抛出
         */
        void mac_reset();

        /**
         * @brief 阶段 4: 硬件配置
         *
         * 配置以下硬件模块：
         *   - HCI 控制寄存器
         *   - MMU（内存管理单元）配置
         *   - TX/RX 阈值和流控
         *   - TSF 定时器
         *   - MAC/BSSID 地址
         *   - 以太网类型过滤器
         *   - 原因陷阱（cause trap）
         *   - RX 流路由
         *   - 决策表
         *   - 邮箱阈值
         *
         * @param config 驱动配置（包含 MAC 地址等）
         *
         * @throws InitException 寄存器写入失败时抛出
         */
        void hw_config(const DriverConfig &config);

        /**
         * @brief 阶段 5: 下载固件到 SRAM + EP0 切换
         *
         * 从文件系统读取固件并通过 USB 控制端点写入 SRAM。
         * 下载完成后切换到 EP0 寄存器访问模式。
         *
         * 协议细节：
         *   - 使用 bRequest=0xF0, VENDOR OUT
         *   - 以 512 字节块写入 SRAM 地址 0x00
         *   - 绕过 CMD/RSP 协议（固件尚未运行）
         *   - 跳过 0x10000-0x10AFF 区域（Turismo C0/D0 bug）
         *   - 下载后: 设置 SRAM 模式、块计数、启用 MCU
         *   - 等待固件启动并切换到 EP0 模式
         *
         * @param config 驱动配置（包含 firmware_path）
         *
         * @throws InitException 固件文件不存在或下载失败时抛出
         */
        void download_firmware(const DriverConfig &config);

        /**
         * @brief 阶段 6: 验证固件运行
         *
         * 读取 FW_VERSION_REG 确认固件正在运行。
         * 通过 WDOG 寄存器变化验证固件活跃。
         *
         * @throws InitException 固件验证失败时抛出
         */
        void verify_firmware();

        /**
         * @brief 阶段 7: 加载 PHY/RF 校准表 + PLL 初始化
         *
         * 加载静态嵌入的校准数据并初始化 PLL：
         *   - tu_rf_tbl[]: RF 校准表
         *   - tu_phy_tbl[]: PHY 校准表
         *   - PLL 初始化和锁定
         *   - 时钟源切换
         *   - 混杂模式过滤器设置
         *
         * 这些是芯片特定的模拟校准值，不在固件中。
         * 错误的值会导致静默 RX 失败（无数据包接收）。
         *
         * @throws InitException 校准表加载失败时抛出
         *
         * @warning 不要修改这些表，除非有来自内核驱动的匹配更改
         */
        void load_phy_rf_tables(const DriverConfig &config);

        /**
         * @brief 阶段 8: 设置初始信道
         *
         * 根据频率范围选择不同的寄存器路径：
         *   - **2.4GHz (1-14)**: 使用 0xCCB0A464 路径
         *   - **5GHz (36-165)**: 使用 0xCCB0A580 路径
         *
         * PLL 模式序列：
         *   - 2.4G: mode 0 → mode 3
         *   - 5G: mode 0 → mode 7
         *
         * 最后通过 ADR_CH_STA_PRI 通知固件新信道。
         *
         * @param channel 信道号（2.4G: 1-14, 5G: 36-165）
         * @param bandwidth 信道带宽（HT20/HT40_MINUS/HT40_PLUS）
         *
         * @throws InitException 信道设置失败时抛出
         *
         * @note RF 合成器信道（rf_channel）如果为 0 则自动计算
         */
        void set_channel(uint8_t channel, ChannelBandwidth bandwidth);

        /**
         * @brief 设置当前频段的发射功率索引
         *
         * 直接写 ADR_MODE_REGISTER 的 RG_TX_GAIN[30:24] + RG_TX_GAIN_MANUAL
         * （与内核 turismoC SET_RG_TX_GAIN 一致）。固件 RFPHY_CMD_TX_PWR
         * 在当前 ssv6x5x-sw.bin 上无响应。索引不是校准 dBm 值。
         *
         * @param channel 当前信道（用于日志频段标注）
         * @param power_index 功率索引（0-127）
         */
        void set_tx_power_index(uint8_t channel, uint32_t power_index);

        /**
         * @brief 阶段 9: 启用 PHY（最后一步）
         *
         * 写入 TU_PHY_ALL_EN 启用所有 PHY 子模块。
         * 匹配内核驱动 ssv6xxx_init_hw() 中 HAL_SET_PHY_MODE(sh, true)
         * 的位置 — 在信道设置之后才启用 PHY。
         *
         * @throws InitException PHY 启用失败时抛出
         */
        void enable_phy();

        /* ==================== 关闭/清理 ==================== */

        /**
         * @brief 安全关闭芯片
         *
         * 执行清理操作：
         *   1. 停止 MCU（清除 CPU 时钟使能位）
         *   2. 保持 PHY 禁用状态
         *   3. 不重新启用 CPU 时钟（确保下次启动干净状态）
         *
         * 幂等操作：可重复调用而不出错。
         *
         * @note 应在程序退出前调用，或在异常处理中调用
         */
        void shutdown();

        /* ==================== 状态查询 ==================== */

        /**
         * @brief 获取当前初始化阶段
         * @return 当前阶段（InitPhase 枚举）
         */
        InitPhase current_phase() const noexcept
        {
            return current_phase_;
        }

        /**
         * @brief 检查初始化是否已完成
         * @return true 如果所有阶段都已完成
         */
        bool is_initialized() const noexcept
        {
            return current_phase_ == InitPhase::COMPLETED;
        }

        /**
         * @brief 获取芯片 ID 字符串
         * @return 芯片 ID（如 "SSV6256P"），如果未读取则返回空字符串
         */
        const std::string &chip_id() const noexcept
        {
            return chip_id_;
        }

        /* ==================== 高级配置 ==================== */

        /**
         * @brief 设置自定义固件加载器
         *
         * 允许从非标准来源加载固件（如内存缓冲区、网络等）。
         *
         * @param loader 固件加载函数指针
         *
         * @warning 必须在 download_firmware() 之前调用
         *
         * 使用示例：
         * @code
         * // 从内存缓冲区加载
         * static int my_loader(const std::string& path,
         *                       const uint8_t** data, size_t* size) {
         *     *data = my_firmware_buffer;
         *     *size = my_firmware_size;
         *     return 0;
         * }
         * chip.set_firmware_loader(my_loader);
         * @endcode
         */
        void set_firmware_loader(FirmwareLoader loader);

    private:
        /* ==================== 引用 ==================== */
        UsbTransport &usb_; ///< USB 传输层引用（不拥有所有权）

        /* ==================== 日志 ==================== */
        std::shared_ptr<spdlog::logger> logger_; ///< 异步日志记录器

        /* ==================== 状态 ==================== */
        InitPhase current_phase_{InitPhase::NONE}; ///< 当前初始化阶段
        std::string chip_id_;                      ///< 芯片 ID 字符串
        bool mcu_running_{false};                  ///< MCU 运行状态标志
        uint32_t detected_bus_clock_mhz_{40};      ///< MAC 复位阶段检测到的总线时钟

        struct EfuseCalibration {
            uint32_t item_mask{0};
            uint8_t xtal_offset{0};
            uint8_t tx_power_index_1{0};
            uint8_t tx_power_index_2{0};
            uint8_t rate_table_1{0};
            uint8_t rate_table_2{0};
        } efuse_;

        /* ==================== 配置 ==================== */
        FirmwareLoader fw_loader_{nullptr}; ///< 自定义固件加载器

        /* ==================== 私有辅助方法 ==================== */

        /** 内置默认固件加载器（从文件系统读取） */
        static int default_firmware_loader(const std::string &firmware_path,
                                           const uint8_t **data, size_t *size);

        /** 应用混杂模式过滤器覆盖（固件前设置） */
        void apply_rx_promiscuous(bool notify_fw);

        /** 读取内核在初始化前读取的 EFUSE 校准映射 */
        void read_efuse_calibration();

        /** 设置 PHY 带宽（HT20/HT40） */
        void set_phy_bandwidth(ChannelBandwidth bw);

        /** 设置 2.4GHz 信道 */
        void set_channel_24g(uint8_t channel, ChannelBandwidth bw);

        /** 设置 5GHz 信道 */
        void set_channel_5g(uint8_t channel, ChannelBandwidth bw);

        /** 5GHz RX 校准（可选） */
        void calibrate_5g_rx(uint8_t target_channel);

        /** 通过固件 HCI 命令控制 RF/PHY */
        void control_rf_via_firmware();

        /** 通过固件设置信道 */
        void set_channel_via_firmware(uint8_t channel, uint8_t chan_type);

        /** 等待寄存器值变为目标值（轮询） */
        void wait_for_reg(uint32_t addr, uint32_t mask, uint32_t target,
                          unsigned max_attempts = 10000,
                          const char *reg_name  = nullptr);
    };

} // namespace ssv6xxx
