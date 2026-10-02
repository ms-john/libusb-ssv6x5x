/*
 * ssv6xxx_driver.hpp - SSV6X5X 驱动公共 API 门面类 (C++14)
 *
 * 提供统一的高层接口，组合三个核心子模块：
 *   - UsbTransport: USB 设备通信层
 *   - ChipInitializer: 7 阶段硬件初始化器
 *   - MonitorSession: 帧接收/注入会话
 *
 * 设计模式：
 *   - **门面模式 (Facade)**: 将复杂的子系统操作封装为简单接口
 *   - **组合模式 (Composition)**: 通过组合而非继承实现功能复用
 *   - **RAII 资源管理**: 构造/析构自动管理资源生命周期
 *
 * 使用示例：
 * @code
 * // 基础用法：初始化 + 监控 + 注入
 * auto logger = spdlog::stdout_color_mt("ssv6xxx");
 *
 * try {
 *     ssv6xxx::SSV6xxxDriver driver(logger);
 *
 *     ssv6xxx::DriverConfig config;
 *     config.mac_addr = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
 *     config.default_channel = 149;  // 5GHz
 *     config.bandwidth = ssv6xxx::ChannelBandwidth::HT40_PLUS;
 *
 *     driver.init(config);                    // 完整初始化（7 个阶段）
 *
 *     driver.set_rx_callback([](const ssv6xxx::RxFrameView& frame, void*) {
 *         std::printf("[RX] %d bytes, RSSI=%d dBm\n",
 *                     frame.length, frame.metadata.rssi_dbm);
 *     });
 *
 *     driver.open_pcap("capture.pcap");       // 可选：输出到 pcap
 *     driver.start_monitor();                  // 启动监控模式
 *
 *     while (driver.is_monitoring()) {
 *         driver.poll(100);                   // 轮询帧接收
 *
 *         // 可以在这里注入帧...
 *         // driver.inject_frame(beacon, beacon_len, 11);  // 54Mbps
 *     }
 *
 * } catch (const ssv6xxx::DriverException& e) {
 *     logger->error("驱动错误: {}", e.what());
 * }
 * // 析构函数自动调用 shutdown() 清理资源
 * @endcode
 *
 * 高级用法：
 * @code
 * // 在单独线程中运行监控循环
 * std::thread monitor_thread([&driver]() {
 *     driver.rx_loop();  // 阻塞直到 request_stop()/stop_monitor()
 * });
 *
 * // 主线程可以做其他事情...
 * std::this_thread::sleep_for(std::chrono::seconds(60));
 *
 * // 请求接收循环退出，等待线程结束后释放资源
 * driver.request_stop();
 * monitor_thread.join();
 * driver.stop_monitor();
 * @endcode
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <spdlog/spdlog.h>

#include "ssv6xxx_exceptions.hpp"
#include "ssv6xxx_monitor_session.hpp" // MonitorStats 类型定义
#include "ssv6xxx_types.hpp"

/* 前向声明 */
namespace ssv6xxx
{
    class UsbTransport;
    class ChipInitializer;
    class MonitorSession;
} // namespace ssv6xxx

namespace ssv6xxx
{

    /* ==================== 驱动门面类 ==================== */

    /**
     * @brief SSV6X5X 驱动公共 API 门面类
     *
     * 这是用户空间驱动的最高层接口，封装了所有底层复杂性。
     * 用户只需与此类交互即可完成设备初始化、监控、注入等操作。
     *
     * 职责：
     *   1. **资源管理**：管理 UsbTransport、ChipInitializer、MonitorSession
     * 的生命周期
     *   2. **初始化编排**：协调 7 阶段硬件初始化流程
     *   3. **状态验证**：确保操作在正确的状态下执行（已初始化、监控中等）
     *   4. **接口转发**：将高层调用转发到对应的子模块
     *   5. **异常转换**：将子模块异常统一包装为 DriverException 层次结构
     *
     * 线程安全：
     *   - 控制类公共方法都不是线程安全的（除了 is_*() 查询和 request_stop()）
     *   - 如果需要多线程访问，外部必须加锁
     *   - 典型用法：主线程控制 + 单独的 rx_loop() 线程
     *
     * 生命周期：
     *   1. 构造 → 2. init() → 3. start_monitor() → [4. poll()/rx_loop()] → 5. 析构
     *   或：构造 → 2. init() → 3. inject_frame() → 4. 析构（仅注入模式）
     *
     * @note 此类拥有所有子模块的唯一所有权（unique_ptr）
     * @note 析构时会自动调用 shutdown() 和 stop_monitor()
     */
    class SSV6xxxDriver
    {
    public:
        /* ==================== 构造/析构 ==================== */

        /**
         * @brief 使用默认日志器创建驱动对象
         *
         * 一个驱动对象在 init() 成功后独占一张 USB 网卡，直到
         * shutdown() 或对象析构。
         */
        SSV6xxxDriver();

        /**
         * @brief 构造函数
         *
         * 初始化 logger 并分配子模块对象（但不打开设备或执行初始化）。
         * 必须显式调用 init() 才能使用驱动功能。
         *
         * @param logger spdlog 异步 logger（必须非空）
         *
         * @throws std::invalid_argument 如果 logger 为空
         *
         * @note 构造函数不会抛出 USB 或硬件相关异常
         * @note 子模块在构造时创建，但处于未初始化状态
         */
        explicit SSV6xxxDriver(std::shared_ptr<spdlog::logger> logger);

        /**
         * @brief 析构函数（自动清理所有资源）
         *
         * 执行以下清理操作（按顺序）：
         *   1. 如果监控正在运行 → 调用 stop_monitor()
         *   2. 如果芯片已初始化 → 调用 shutdown()
         *   3. 销毁所有子模块对象（MonitorSession、ChipInitializer、UsbTransport）
         *
         * @note 即使在异常路径中也能保证资源释放（RAII）
         * @note 如果子模块析构抛出异常，会被捕获并记录日志
         */
        ~SSV6xxxDriver();

        /* 禁止拷贝（包含唯一资源所有权） */
        SSV6xxxDriver(const SSV6xxxDriver &)            = delete;
        SSV6xxxDriver &operator=(const SSV6xxxDriver &) = delete;

        /* 禁止移动（简化实现，避免悬空引用） */
        SSV6xxxDriver(SSV6xxxDriver &&)            = delete;
        SSV6xxxDriver &operator=(SSV6xxxDriver &&) = delete;

        /* ==================== 核心操作 ==================== */

        /**
         * @brief 初始化驱动（完整 7 阶段流程）
         *
         * 执行步骤：
         *   1. 打开 USB 设备（libusb_open + detach_kernel_driver）
         *   2. 执行 ChipInitializer::full_init(config)
         *      - 阶段 1: 芯片 ID 验证
         *      - 阶段 2: MAC 复位
         *      - 阶段 3: 硬件配置
         *      - 阶段 4: PHY/RF 校准表加载
         *      - 阶段 5: 固件下载
         *      - 阶段 6: PHY 启用 + 固件验证
         *      - 阶段 7: 信道设置
         *   3. 标记 initialized_ = true
         *
         * @param config 驱动配置参数（MAC 地址、固件路径、初始信道等）
         *
         * @throws UsbException USB 设备打开失败时抛出
         * @throws InitException 任何初始化阶段失败时抛出
         * @throws DriverException 其他驱动错误时抛出
         *
         * @warning 此方法会修改芯片硬件状态，不可逆操作
         * @warning 需要 root/sudo 权限（USB 设备访问）
         * @warning 一次只能有一个实例运行（USB 接口被独占声明）
         *
         * @note 此方法通常耗时 200-500ms（取决于固件大小和校准时间）
         * @note 如果已经初始化过，会先 shutdown() 再重新初始化
         *
         * @see shutdown(), is_initialized()
         */
        void init(const DriverConfig &config);

        /**
         * @brief 安全关闭驱动
         *
         * 执行以下清理操作：
         *   1. 如果监控正在运行 → 自动停止
         *   2. 调用 ChipInitializer::shutdown()（停止 MCU，禁用 PHY）
         *   3. 调用 UsbTransport::close()（释放 USB 接口）
         *   4. 重置 initialized_ = false
         *
         * @note 可以安全地重复调用（幂等操作）
         * @note 关闭后可以再次调用 init() 重新初始化
         * @note 通常不需要手动调用，析构函数会自动处理
         *
         * @see init(), is_initialized()
         */
        void shutdown();

        /* ==================== 信道控制 ==================== */

        /**
         * @brief 切换信道
         *
         * 动态切换 RF 接收/发送信道。
         * 支持在运行时切换（无需重启监控）。
         *
         * @param channel 目标信道号
         *                - 2.4GHz: 1-14
         *                - 5GHz: 36-165（具体范围取决于地区法规）
         * @param bw 信道带宽（默认 HT20）
         *
         * @throws InvalidStateException 如果驱动未初始化
         * @throws InitException 信道设置失败时抛出
         *
         * @warning 切换信道会导致短暂的中断（约 10-50ms）
         * @warning 5GHz 信道可能需要额外的校准时间
         *
         * 使用示例：
         * @code
         * driver.init(config);
         * driver.start_monitor();
         *
         * // 从信道 6 (2.4G HT20) 切换到信道 149 (5G HT40+)
         * driver.set_channel(149, ssv6xxx::ChannelBandwidth::HT40_PLUS);
         * @endcode
         *
         * @see get_current_channel(), DriverConfig::default_channel
         */
        void set_channel(uint8_t channel,
                         ChannelBandwidth bw = ChannelBandwidth::HT20);

        /**
         * @brief 运行时设置发射功率索引（0-127，非 dBm）
         *
         * 直接写 ADR_MODE_REGISTER 的 RG_TX_GAIN/RG_TX_GAIN_MANUAL
         * （与内核 turismoC SET_RG_TX_GAIN 一致）。应先 set_channel()
         * 到目标信道再调用。
         *
         * @param power_index 0-127
         * @throws InvalidStateException 驱动未初始化
         * @throws InitException 寄存器访问失败
         */
        void set_tx_power_index(uint32_t power_index);

        /**
         * @brief 读取 11g/n PHY 噪声功率寄存器的一次快照。
         * @return 原始 7-bit 值、dBm 换算值和有效状态；读取失败或值为零时无效。
         */
        NoiseFloorReading read_noise_floor();

        /**
         * @brief 清零并启用当前信道的 EDCCA 统计窗口。
         * @return 寄存器读写全部成功时返回 true。
         * @note 使用参考内核驱动的 25 us EDCCA 平均周期。
         */
        bool reset_channel_survey();

        /**
         * @brief 读取当前信道的 EDCCA、PHY 噪声功率和 FCS 计数。
         * @return 带有效标志、读取错误位图、噪声功率和单调时间戳的硬件统计快照。
         */
        ChannelSurvey read_channel_survey();

        /**
         * @brief 读取用于诊断接收链路的硬件计数器。
         * @return FCS、PHY、HCI 和缓冲区状态计数；读取失败通过位图返回。
         */
        HardwareHealthCounters read_hardware_health();

        /**
         * @brief 获取当前有效的 PHY 噪声底 dBm 值。
         * @throws UsbException 寄存器读取失败或当前没有有效 PHY 测量时抛出。
         */
        int8_t get_noise_floor();

        /* ==================== 监控模式 ==================== */

        /**
         * @brief 启动监控模式（非阻塞）
         *
         * 配置混杂模式并开始异步接收 802.11 帧。
         * 必须先调用 init() 成功完成初始化。
         *
         * 执行步骤：
         *   1. 验证 initialized_ == true
         *   2. 调用 MonitorSession::start()
         *      - 配置 RX 过滤器为混杂模式
         *      - 分配 RX 缓冲区
         *      - 提交第一个异步 URB 到 EP4
         *
         * @throws InvalidStateException 如果驱动未初始化
         * @throws UsbException USB 操作失败时抛出
         *
         * @note 此方法是**非阻塞**的。实际的帧接收发生在 poll() 或 rx_loop() 中
         * @note 可以在单独的线程中调用 rx_loop() 进行阻塞式接收
         *
         * @see stop_monitor(), is_monitoring(), poll(), rx_loop()
         */
        void start_monitor();

        /**
         * @brief 停止监控模式并释放资源
         *
         * 取消待处理的 RX URB 并释放资源。
         * 必须与 poll()/rx_loop() 串行调用；跨线程退出请先调用 request_stop()。
         *
         * @note 安全地重复调用（幂等操作）
         * @note 停止后可以再次调用 start_monitor() 重新启动
         *
         * @see start_monitor(), is_monitoring()
         */
        void stop_monitor();

        /**
         * @brief 请求接收循环退出，但不释放 USB 资源。
         *
         * 该方法只修改原子运行标志，控制线程可以调用它，然后等待
         * rx_loop() 线程结束，再由资源拥有线程调用 stop_monitor()。
         */
        void request_stop() noexcept;

        /**
         * @brief 检查监控是否正在运行
         * @return true 如果 start_monitor() 已调用且 stop_monitor() 未调用
         *
         * @note 使用 atomic<bool> 保证跨线程可见性
         */
        bool is_monitoring() const noexcept;

        /* ==================== 帧接收轮询 ==================== */

        /**
         * @brief 轮询一次 USB 事件（非阻塞）
         *
         * 处理所有已完成的 USB 传输（主要是 RX URB）。
         * 如果有帧到达，会自动解析并调用注册的回调函数。
         *
         * @param timeout_ms 超时时间（毫秒），0 表示立即返回
         *
         * @return 成功返回 0，错误返回负值
         *
         * @throws InvalidStateException 如果监控未启动
         *
         * @note 此方法是非阻塞的，适合在事件循环中使用
         * @note 典型用法：while (is_monitoring()) { poll(100); }
         *
         * @see start_monitor(), rx_loop(), is_monitoring()
         */
        int poll(int timeout_ms = 100);

        /**
         * @brief 阻塞式接收循环
         *
         * 持续轮询直到 request_stop()/stop_monitor() 被调用或发生致命错误。
         * 内部实现为 while (is_monitoring()) { poll(timeout_ms); }
         *
         * @param timeout_ms 每次 poll 的超时时间（毫秒），默认 100ms
         *
         * @note 此方法会阻塞当前线程！应在单独的线程中运行
         * @note 要从另一个线程退出循环，应调用 request_stop()；循环退出后再调用 stop_monitor()
         *
         * 使用示例：
         * @code
         * driver.init(config);
         * driver.start_monitor();
         *
         * // 在后台线程中运行
         * std::thread t([&]() { driver.rx_loop(100); });
         *
         * // 主线程做其他事情...
         * std::this_thread::sleep_for(std::chrono::seconds(60));
         *
         * driver.request_stop();
         * t.join();
         * driver.stop_monitor();
         * @endcode
         *
         * @see stop_monitor(), poll(), start_monitor()
         */
        void rx_loop(int timeout_ms = 100);

        /* ==================== 帧注入 ==================== */

        /**
         * @brief 注入 802.11 帧到空中接口
         *
         * 构造完整的数据包并通过 EP3 发送到芯片。
         * 不需要启动监控模式即可使用（仅需 init() 完成）。
         *
         * @param frame 原始 802.11 帧数据指针（不含 FCS）
         * @param frame_len 帧长度（字节，最大 2304）
         * @param rate_idx 内核速率索引：0=安全默认速率；1-4=B 长前导码，
         *                 7-14=G 模式；15-22=HT20 MCS0-7 LGI；23-30=HT20
         *                 MCS0-7 SGI；31-38=HT40 MCS0-7 LGI；39-46=HT40
         *                 MCS0-7 SGI；也可传入
         *                 0x80-0xFF 的内核原始 PHY 速率编码
         *
         * @throws InvalidStateException 如果驱动未初始化
         * @throws UsbException USB 传输失败时抛出
         * @throws std::invalid_argument 参数无效时抛出
         *
         * @note 常用速率索引：
         *       - 0: 安全默认速率（5GHz 为 6Mbps，2.4GHz 为 1Mbps）
         *       - 11: 54 Mbps (OFDM)
         *       - 22: MCS7 (HT20 LGI)
         *       - 30: MCS7 (HT20 SGI)
         *       - 38: MCS7 (HT40 LGI)
         *       - 46: MCS7 (HT40 SGI)
         *
         * @warning 注入前确保信道正确且射频前端已启用
         *
         * @see start_monitor(), MonitorSession::inject_frame()
         */
        void inject_frame(const uint8_t *frame, int frame_len, int rate_idx = 0);

        /* ==================== 回调与事件 ==================== */

        /**
         * @brief 设置帧接收回调函数
         *
         * 注册一个回调，当接收到有效的 802.11 帧时调用。
         * 帧数据已经剥离了 80 字节 RX 描述符和尾部 4 字节 padding。
         *
         * @param cb 回调函数（使用 nullptr 清除回调）
         * @param user_data 用户自定义数据（透传给回调的第 6 个参数）
         *
         * @note 回调在 poll()/rx_loop() 的调用线程中执行
         * @note 回调应尽可能短，避免阻塞 RX URB 重新提交
         *
         * 使用示例：
         * @code
         * struct MyContext {
         *     int count{0};
         * } ctx;
         *
         * driver.set_rx_callback([](const ssv6xxx::RxFrameView& frame,
         *                            void* user_data) {
         *     auto* ctx = static_cast<MyContext*>(user_data);
         *     ctx->count++;
         *     if (ctx->count % 100 == 0) {
         *         std::printf("已收到 %d 包\n", ctx->count);
         *     }
         * }, &ctx);
         * @endcode
         *
         * @see RxFrameCallback 类型定义 (ssv6xxx_types.hpp)
         */
        void set_rx_callback(RxFrameCallback cb, void *user_data = nullptr);

        /* ==================== PCap 输出 ==================== */

        /**
         * @brief 打开 PCap 文件用于写入
         *
         * 创建 pcap 文件并将后续接收到的帧以 libpcap 格式写入。
         * 帧数据包含 radiotap 头部（RSSI、频率、信道等信息）。
         *
         * @param filepath 文件路径（绝对路径或相对路径）
         *
         * @throws std::runtime_error 文件无法创建时抛出
         *
         * @note 链路层类型：LINKTYPE_IEEE802__RADIO (127)
         * @note 如果已有打开的 pcap 文件，会先关闭它
         * @note 析构时会自动关闭 pcap 文件
         *
         * @see close_pcap(), is_pcap_open()
         */
        void open_pcap(const std::string &filepath);

        /**
         * @brief 关闭 PCap 文件
         *
         * 刷新缓冲区并关闭文件句柄。
         * 可以安全地重复调用（幂等操作）。
         *
         * @note 通常不需要手动调用，析构函数会自动处理
         */
        void close_pcap();

        /**
         * @brief 检查 PCap 文件是否已打开
         * @return true 如果 pcap 文件当前处于打开状态
         */
        bool is_pcap_open() const noexcept;

        /* ==================== 统计信息 ==================== */

        /**
         * @brief 获取当前统计信息快照
         * @return MonitorStats 结构体副本（包含 RX/TX 计数器）
         *
         * @note 返回的是快照，后续修改不影响此对象
         */
        MonitorStats get_stats() const noexcept;

        /**
         * @brief 重置所有统计计数器为零
         */
        void reset_stats();

        /**
         * @brief 打印统计摘要到日志
         *
         * 输出 RX/TX 数据包数、错误数、吞吐量、运行时间等。
         */
        void print_stats() const;

        /* ==================== 状态查询 ==================== */

        /**
         * @brief 检查驱动是否已完成初始化
         * @return true 如果 init() 已成功完成且 shutdown() 未调用
         *
         * @note 此方法只读取原子状态标志，可安全用于状态观察。
         */
        bool is_initialized() const noexcept
        {
            return initialized_.load(std::memory_order_acquire);
        }

        /**
         * @brief 获取芯片 ID 字符串
         * @return 芯片型号（如 "SSV6256P"），如果未初始化则返回空字符串
         *
         * @see ChipInitializer::chip_id()
         */
        std::string get_chip_id() const;

        /**
         * @brief 获取当前信道号
         * @return 当前信道号（1-14 或 36-165），如果未初始化则返回 0
         */
        uint8_t get_current_channel() const noexcept;

    private:
        /* ==================== 内部状态 ==================== */

        std::shared_ptr<spdlog::logger> logger_; ///< 异步日志记录器

        std::unique_ptr<UsbTransport> usb_;       ///< USB 传输层（唯一所有权）
        std::unique_ptr<ChipInitializer> chip_;   ///< 芯片初始化器（唯一所有权）
        std::unique_ptr<MonitorSession> monitor_; ///< 监控会话（唯一所有权）

        std::atomic<bool> initialized_{false}; ///< 初始化完成标志

        /* init() 尚未创建 MonitorSession 时，先保存结构化回调配置。 */
        RxFrameCallback rx_callback_;
        void *rx_callback_user_data_{nullptr};
        bool rx_callback_configured_{false};

        /* ==================== 私有辅助方法 ==================== */

        /**
         * @brief 确保驱动已初始化
         *
         * 如果 initialized_ == false，抛出 InvalidStateException。
         * 用于在需要初始化的操作前进行前置条件检查。
         *
         * @throws InvalidStateException 如果驱动未初始化
         */
        void ensure_initialized() const;

        /**
         * @brief 确保监控模式已启动
         *
         * 如果 !is_monitoring()，抛出 InvalidStateException。
         * 用于在需要监控的操作前进行前置条件检查。
         *
         * @throws InvalidStateException 如果监控未启动
         */
        void ensure_monitoring() const;
    };

} // namespace ssv6xxx
