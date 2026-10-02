/*
 * ssv6xxx_monitor_session.hpp - SSV6X5X 监控/注入会话 (C++14)
 *
 * 封装帧接收（RX）和帧注入（TX）功能：
 *   - 异步 RX URB 提交与回调处理
 *   - 802.11 帧解析（剥离硬件描述符）
 *   - TX 帧注入（预置 80 字节 TX 描述符）
 *   - PCap 文件输出支持
 *   - 统计信息跟踪
 *
 * 设计特点：
 *   - 依赖 UsbTransport 进行底层 USB 通信
 *   - 从构造函数注入 logger 和 USB 传输层引用
 *   - 使用 RxFrameCallback 类型别名（来自 ssv6xxx_types.hpp）
 *   - RAII 管理：析构时自动停止监控并关闭 pcap
 *
 * 使用示例：
 * @code
 * auto logger = spdlog::stdout_color_mt("monitor");
 * ssv6xxx::UsbTransport usb(logger);
 * usb.open();
 *
 * ssv6xxx::MonitorSession monitor(usb, logger);
 *
 * // 设置帧接收回调
 * monitor.set_callback([](const ssv6xxx::RxFrameView& frame, void*) {
 *     std::printf("[RX] %d bytes, RSSI=%d dBm\n",
 *                 frame.length, frame.metadata.rssi_dbm);
 * });
 *
 * // 可选：输出到 pcap 文件
 * monitor.open_pcap("capture.pcap");
 *
 * // 启动监控模式
 * monitor.start();
 *
 * // 主循环（轮询或阻塞式接收）
 * while (monitor.is_running()) {
 *     monitor.poll(100);  // 100ms 超时
 * }
 *
 * // 或使用阻塞式循环（在单独线程中运行）
 * // monitor.rx_loop();  // 会阻塞直到 stop() 被调用
 * @endcode
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <libusb-1.0/libusb.h>
#include <memory>
#include <mutex>
#include <string>

#include <spdlog/spdlog.h>

#include "ssv6xxx_descriptors.hpp"
#include "ssv6xxx_exceptions.hpp"
#include "ssv6xxx_types.hpp"

/* 前向声明 */
namespace ssv6xxx
{
    class UsbTransport;
}

namespace ssv6xxx
{

    /* ==================== 统计信息结构体 ==================== */

    /**
     * @brief 监控会话统计信息
     *
     * 跟踪 RX/TX 的数据包数量和错误计数。
     * 用于性能监控和调试诊断。
     */
    struct MonitorStats {
        /* RX 统计 */
        uint64_t rx_packets{0}; ///< 接收到的有效数据包总数
        uint64_t rx_bytes{0};   ///< 接收到的总字节数（不含硬件描述符）
        uint64_t rx_errors{0};  ///< 接收错误次数（CRC 失败、描述符无效等）
        uint64_t rx_dropped{0}; ///< 丢弃的数据包数（缓冲区满等）

        /* TX 统计 */
        uint64_t tx_packets{0}; ///< 成功注入的数据包总数
        uint64_t tx_bytes{0};   ///< 注入的总字节数（不含 TX 描述符）
        uint64_t tx_errors{0};  ///< 注入失败次数（USB 错误、参数无效等）

        /* 时间戳 */
        uint64_t start_time_ms{0};   ///< 会话开始时间（Unix 时间戳，毫秒）
        uint64_t last_rx_time_ms{0}; ///< 最后一次接收到帧的时间

        /**
         * @brief 重置所有统计计数器
         */
        void reset()
        {
            rx_packets      = 0;
            rx_bytes        = 0;
            rx_errors       = 0;
            rx_dropped      = 0;
            tx_packets      = 0;
            tx_bytes        = 0;
            tx_errors       = 0;
            start_time_ms   = 0;
            last_rx_time_ms = 0;
        }

        /**
         * @brief 计算 RX 吞吐量（字节/秒）
         * @return 平均吞吐量，如果会话时间太短则返回 0
         */
        double rx_throughput_bps() const
        {
            if (start_time_ms == 0 || last_rx_time_ms <= start_time_ms) {
                return 0.0;
            }
            double duration_sec =
                static_cast<double>(last_rx_time_ms - start_time_ms) / 1000.0;
            if (duration_sec < 0.001) { // 少于 1ms
                return 0.0;
            }
            return static_cast<double>(rx_bytes) / duration_sec;
        }

        /**
         * @brief 计算平均 RSSI（需要外部维护总和）
         * @param rssi_sum RSSI 总和（由调用者累加）
         * @return 平均 RSSI 值（dBm），如果没有收到包则返回 0
         */
        double avg_rssi(double rssi_sum) const
        {
            if (rx_packets == 0) {
                return 0.0;
            }
            return rssi_sum / static_cast<double>(rx_packets);
        }
    };

    /* ==================== 监控会话类 ==================== */

    /**
     * @brief 监控/注入会话 - 管理帧接收和注入
     *
     * 职责：
     *   1. **异步 RX**：提交 URB 到 EP4，处理完成回调
     *   2. **帧解析**：剥离 80 字节 ssv6006_rx_desc（含 WORD5-WORD7 PHY 信息）和尾部 4 字节 padding
     *                 → 输出原始 802.11 帧
     *   3. **回调分发**：将解析后的帧传递给用户注册的 RxFrameCallback
     *   4. **PCap 输出**：可选地将帧写入 libpcap 格式文件
     *   5. **TX 注入**：预置 80 字节 ssv6200_tx_desc，通过 EP3 发送
     *   6. **统计跟踪**：维护 MonitorStats 结构体
     *
     * 线程安全：
     *   - request_stop() 可从控制线程请求接收循环退出；资源释放仍由拥有线程执行
     *   - is_running() 使用 atomic<bool> 保证可见性
     *   - 回调函数在 poll()/rx_loop() 的调用线程中执行
     *
     * 生命周期：
     *   - 构造后必须调用 start() 才能接收帧
     *   - 析构时会自动调用 stop()
     *   - 如果打开了 pcap 文件，析构时会自动关闭
     *
     * @note UsbTransport 的生命周期必须长于 MonitorSession
     */
    class MonitorSession
    {
    public:
        /* ==================== 类型别名 ==================== */

        /**
         * @brief 帧接收回调类型（复用 ssv6xxx_types.hpp 中的定义）
         *
         * 当从硬件接收到完整的 802.11 帧时调用。
         * 帧数据已经剥离了 80 字节 RX 描述符和尾部 4 字节 padding。
         *
         * @param frame     结构化帧视图；数据指针仅在回调执行期间有效
         * @param user_data 用户自定义数据（透传）
         */
        /* ==================== 构造/析构 ==================== */

        /**
         * @brief 构造函数
         *
         * 初始化内部状态，但不启动监控。
         * 必须显式调用 start() 开始接收帧。
         *
         * @param usb USB 传输层引用（必须已打开且已完成初始化）
         * @param logger spdlog 异步 logger（必须非空）
         *
         * @throws std::invalid_argument 如果参数无效
         *
         * @warning 在调用 start() 前，确保芯片已完成完整初始化
         *         （ChipInitializer::full_init() 已成功执行）
         *
         * @note UsbTransport 的生命周期必须长于 MonitorSession
         */
        explicit MonitorSession(UsbTransport &usb,
                                std::shared_ptr<spdlog::logger> logger);

        /**
         * @brief 析构函数（自动调用 stop() 和 close_pcap()）
         *
         * 如果监控仍在运行，自动停止。
         * 如果 pcap 文件已打开，自动关闭。
         */
        ~MonitorSession();

        /* 禁止拷贝和移动：usb_ 为引用成员，且持有 libusb URB / 缓冲区等
         * 与地址绑定的资源；不完整移动会导致停止路径与完成回调错乱。 */
        MonitorSession(const MonitorSession &)            = delete;
        MonitorSession &operator=(const MonitorSession &) = delete;
        MonitorSession(MonitorSession &&other)            = delete;
        MonitorSession &operator=(MonitorSession &&other) = delete;

        /* ==================== 监控控制 ==================== */

        /**
         * @brief 启动监控模式
         *
         * 执行步骤：
         *   1. 配置混杂模式过滤器（如果尚未配置）
         *   2. 配置 HCI RX 聚合（减少中断开销）
         *   3. 分配 RX 缓冲区
         *   4. 设置 running_ 标志为 true，再提交第一个异步 RX URB
         *   5. 失败路径会退休 URB 并释放缓冲区
         *
         * @throws UsbException USB 操作失败时抛出
         * @throws InvalidStateException 如果已经在运行中
         *
         * @note 此方法是非阻塞的。实际的帧接收发生在 poll() 或 rx_loop() 中
         *
         * @see stop(), is_running(), poll(), rx_loop()
         */
        void start();

        /**
         * @brief 停止监控模式
         *
         * 执行步骤：
         *   1. 设置 running_ 标志为 false
         *   2. 取消待处理的异步 RX URB，并等待完成回调退休
         *   3. 确认 URB 已退休后再释放 RX 缓冲区（避免 UAF）
         *
         * @note 此方法必须与 poll()/rx_loop() 串行调用；跨线程退出请使用
         *       request_stop()，资源释放由执行 poll()/rx_loop() 的线程完成。
         * @note 可以安全地重复调用（幂等操作）
         *
         * @see start(), is_running()
         */
        void stop();

        /**
         * @brief 请求接收循环退出，但不释放 USB 传输资源。
         *
         * 该方法只修改原子运行标志，适合由控制线程调用；随后应等待
         * poll()/rx_loop() 所在线程退出，再由资源拥有线程调用 stop()。
         */
        void request_stop() noexcept;

        /**
         * @brief 检查监控是否正在运行
         * @return true 如果 start() 已调用且 stop() 未调用
         *
         * @note 使用 atomic<bool> 保证跨线程可见性
         */
        bool is_running() const noexcept
        {
            return running_.load();
        }

        /* ==================== 帧接收 ==================== */

        /**
         * @brief 设置帧接收回调函数
         *
         * 注册一个回调函数，当接收到有效的 802.11 帧时调用。
         * 每次调用都会替换之前的回调（不支持多个监听器）。
         *
         * @param cb 回调函数（使用 nullptr 清除回调）
         * @param user_data 用户自定义数据（透传给回调）
         *
         * @note 回调函数在 poll() 或 rx_loop() 的调用线程中执行
         * @note 回调执行时间应尽可能短，避免阻塞 RX URB 重新提交
         *
         * 使用示例：
         * @code
         * monitor.set_callback([](const ssv6xxx::RxFrameView& frame,
         *                          void* user_data) {
         *     // 处理 frame.data 和 frame.metadata...
         * }, &my_context);  // 传递用户数据
         * @endcode
         */
        void set_callback(RxFrameCallback cb, void *user_data = nullptr);

        /**
         * @brief 处理接收到的原始 USB 数据
         *
         * 从 USB RX 缓冲区解析帧：
         *   1. 解析 ssv6006_rx_desc（80 字节）
         *   2. 解析 WORD5-WORD7 PHY 信息（phy_rate、RSSI、SNR、HT40、SGI、MCS）
         *   3. 提取原始 802.11 帧（剩余字节）
         *   4. 调用用户回调（如果已注册）
         *   5. 写入 pcap 文件（如果已打开）
         *   6. 更新统计信息
         *
         * @param data 原始 USB 数据指针（包含硬件描述符）
         * @param len 数据长度（字节）
         *
         * @return 成功返回 0，错误返回负值：
         *         - -EINVAL: 参数无效或数据太短
         *         - -EPROTO: 描述符格式错误
         *
         * @note 此方法通常由内部 URB 完成回调自动调用
         * @note 外部也可以手动调用此方法处理缓冲的数据
         *
         * @see set_callback(), open_pcap()
         */
        int process_rx_data(const uint8_t *data, int len);

        /**
         * @brief 轮询一次 USB 事件（非阻塞）
         *
         * 处理所有已完成的 USB 传输（主要是 RX URB）。
         * 如果有帧到达，会自动调用 process_rx_data() 并重新提交 URB。
         *
         * @param timeout_ms 超时时间（毫秒），0 表示立即返回
         *
         * @return 成功返回 0，错误返回负值：
         *         - -EBADF: 设备未打开或监控未启动
         *         - EINTR: 被信号中断
         *         - ETIMEOUT: 超时（正常情况）
         *
         * @note 此方法是非阻塞的，适合在事件循环中使用
         * @note 典型用法：while (running) { monitor.poll(100); }
         *
         * @see rx_loop(), is_running()
         */
        int poll(int timeout_ms = 100);

        /**
         * @brief 阻塞式接收循环
         *
         * 持续轮询 USB 事件直到 request_stop()/stop() 被调用或发生致命错误。
         * 内部实现为 while (is_running()) { poll(timeout_ms); }
         *
         * @param timeout_ms 每次 poll 的超时时间（毫秒），默认 100ms
         *
         * @return 正常退出返回 0，错误返回负值
         *
         * @note 此方法会阻塞当前线程！通常应在单独的线程中运行
         * @note 要从另一个线程退出循环，应调用 request_stop()；循环退出后再调用 stop()
         *
         * 使用示例：
         * @code
         * // 在主线程中启动监控
         * monitor.start();
         *
         * // 在后台线程中运行接收循环
         * std::thread rx_thread([&]() {
         *     monitor.rx_loop(100);  // 阻塞直到 stop()
         * });
         *
         * // 主线程可以做其他事情...
         * // ...
         *
         * // 请求停止、等待线程结束，再释放资源
         * monitor.request_stop();
         * rx_thread.join();
         * monitor.stop();
         * @endcode
         *
         * @see stop(), poll(), start()
         */
        void rx_loop(int timeout_ms = 100);

        /* ==================== 帧注入 ==================== */

        /**
         * @brief 注入 802.11 帧到空中接口
         *
         * 构造完整的 TX 数据包：
         *   1. 创建 ssv6200_tx_desc（80 字节）
         *   2. 填充帧长度、速率索引、MAC 地址等字段
         *   3. 合并 [tx_desc(80)] [frame(len)] → 发送到 EP3
         *
         * @param frame 原始 802.11 帧数据指针（不含 FCS）
         * @param frame_len 帧长度（字节，最大 2304）
         * @param rate_idx 内核速率索引：0=安全默认速率；1-4=B 长前导码，
         *                 7-14=G 模式；15-22=HT20 MCS0-7 LGI；23-30=HT20
         *                 MCS0-7 SGI；31-38=HT40 MCS0-7 LGI；39-46=HT40
         *                 MCS0-7 SGI；也可传入
         *                 0x80-0xFF 的内核原始 PHY 速率编码
         *
         * @throws UsbException USB 传输失败时抛出
         * @throws std::invalid_argument 参数无效时抛出
         *
         * @note 速率索引对应关系（常用值）：
         *       - 0: 安全默认速率（5GHz 为 6Mbps，2.4GHz 为 1Mbps）
         *       - 1: 1 Mbps (DSSS)
         *       - 2: 2 Mbps (DSSS)
         *       - 5: 6 Mbps (OFDM)
         *       - 7: 12 Mbps (OFDM)
         *       - 9: 24 Mbps (OFDM)
         *       - 11: 54 Mbps (OFDM)
         *       - 15: MCS0 (HT20 LGI)
         *       - 22: MCS7 (HT20 LGI)
         *       - 23: MCS0 (HT20 SGI)
         *       - 31: MCS0 (HT40 LGI)
         *       - 39: MCS0 (HT40 SGI)
         *       （具体映射取决于固件版本）
         *
         * @warning 注入前确保监控模式已启动且信道正确
         *
         * 使用示例：
         * @code
         * // 构造一个 Beacon 帧（简化示例）
         * uint8_t beacon[256];
         * int beacon_len = build_beacon(beacon, sizeof(beacon));
         *
         * // 注入 Beacon（自动选择速率）
         * monitor.inject_frame(beacon, beacon_len, 0);
         *
         * // 以固定速率 54Mbps 注入
         * monitor.inject_frame(beacon, beacon_len, 11);
         * @endcode
         */
        void inject_frame(const uint8_t *frame, int frame_len, int rate_idx = 0);

        /* ==================== PCap 输出 ==================== */

        /**
         * @brief 打开 PCap 文件用于写入
         *
         * 创建一个新的 pcap 文件并写入全局头部。
         * 后续接收到的帧会以 libpcap 格式追加到此文件。
         *
         * @param filepath 文件路径（绝对路径或相对路径）
         *
         * @throws std::runtime_error 文件无法创建时抛出
         *
         * @note 如果已有打开的 pcap 文件，会先关闭它
         * @note 链路层类型为 LINKTYPE_IEEE802__RADIO (127)
         *       包含 radiotap 头部用于记录 RSSI、频率等信息
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
        bool is_pcap_open() const noexcept
        {
            return (pcap_file_ != nullptr);
        }

        /* ==================== 统计信息 ==================== */

        /**
         * @brief 获取当前统计信息的副本
         * @return MonitorStats 结构体快照
         *
         * @note 返回的是快照，后续的修改不会影响已返回的对象
         */
        MonitorStats get_stats() const noexcept;

        /**
         * @brief 重置所有统计计数器为零
         *
         * @note 同时重置 start_time_ms 为当前时间
         */
        void reset_stats();

        /**
         * @brief 打印统计摘要到日志
         *
         * 输出格式：
         * @verbatim
         * [MONITOR] 统计摘要:
         *   RX: 1234 packets (456789 bytes), 5 errors, 2 dropped
         *   TX: 56 packets (12345 bytes), 0 errors
         *   吞吐量: 12345.67 Bps
         *   运行时间: 36.7 秒
         * @endverbatim
         */
        void print_stats() const;

        /* ==================== 高级配置 ==================== */

        /**
         * @brief 设置当前信道号（用于 radiotap 头部）
         *
         * 更新内部存储的信道号，用于生成正确的 radiotap 头部。
         * 当信道切换时应调用此方法同步更新。
         *
         * @param channel 新的信道号（2.4G: 1-14, 5G: 36-165）
         *
         * @note 此方法不会实际切换硬件信道！仅影响 pcap 中的元数据
         *       实际信道切换应通过 ChipInitializer::set_channel() 完成
         */
        void set_channel(uint8_t channel)
        {
            channel_ = channel;
        }

        /**
         * @brief 获取当前信道号
         * @return 当前信道号
         */
        uint8_t get_channel() const noexcept
        {
            return channel_;
        }

    private:
        /* ==================== 引用 ==================== */
        UsbTransport &usb_; ///< USB 传输层引用（不拥有所有权）

        /* ==================== 日志 ==================== */
        std::shared_ptr<spdlog::logger> logger_; ///< 异步日志记录器

        /* ==================== 状态标志 ==================== */
        std::atomic<bool> running_{false}; ///< 监控运行状态（线程安全）

        /* ==================== RX 相关 ==================== */
        uint8_t *rx_buf_{nullptr};          ///< RX 缓冲区指针（动态分配）
        int rx_buf_size_{0};                ///< RX 缓冲区大小（字节）
        libusb_transfer *rx_xfer_{nullptr}; ///< 异步 RX URB 句柄

        /* ==================== 回调 ==================== */
        mutable std::mutex callback_mutex_; ///< 保护回调替换与快照复制
        RxFrameCallback rx_cb_{nullptr};    ///< 帧接收回调函数
        void *cb_user_data_{nullptr};       ///< 回调用户数据

        /* ==================== 统计 ==================== */
        mutable std::mutex stats_mutex_; ///< 保护统计快照与接收线程的并发访问
        MonitorStats stats_;             ///< 统计信息结构体

        /* ==================== PCap 输出 ==================== */
        FILE *pcap_file_{nullptr}; ///< pcap 文件句柄
        std::string pcap_path_;    ///< 当前 pcap 文件路径

        /* ==================== 信道信息 ==================== */
        uint8_t channel_{6}; ///< 当前信道号（用于 radiotap）

        /* ==================== 私有辅助方法 ==================== */

        /** 配置混杂模式过滤器（start() 时调用） */
        void configure_monitor_mode();

        /** 配置 HCI RX 聚合参数 */
        void configure_hci_rx_aggr();

        /** 分配 RX 缓冲区 */
        void allocate_rx_buffer();

        /** 释放 RX 缓冲区 */
        void free_rx_buffer();

        /** 提交异步 RX URB */
        void arm_rx_transfer();

        /**
         * @brief 取消 RX URB 并泵事件直到完成回调摘除该传输
         * @return true 表示 URB 已安全退休；false 表示超时，缓冲区已泄漏以避免 UAF
         */
        bool retire_rx_transfer();

        /** 处理一次异步 RX 完成 */
        void handle_rx_transfer(libusb_transfer *xfer);

        /** libusb 异步 RX 回调桥接 */
        static void LIBUSB_CALL rx_transfer_cb(libusb_transfer *xfer);

        /** 处理单个 MPDU（不含 HCI RX 聚合头） */
        int process_rx_mpdu(const uint8_t *data, int len);

        /** 处理 HCI RX 聚合缓冲区 */
        int process_rx_aggregate(const uint8_t *data, int len);

        /** 写入 pcap 全局头部 */
        void write_pcap_global_header();

        /** 将帧写入 pcap 文件（带 radiotap 头部） */
        void write_pcap_packet(const uint8_t *frame, int len, uint8_t rate,
                               int8_t rssi, uint16_t freq);

        /** 构建 TX 描述符 */
        void build_tx_descriptor(Ssv6200TxDesc *desc, const uint8_t *frame,
                                 int frame_len, int rate_idx);

        /** 获取当前时间戳（毫秒） */
        static uint64_t current_time_ms();
    };

} // namespace ssv6xxx
