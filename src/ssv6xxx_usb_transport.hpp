/*
 * ssv6xxx_usb_transport.hpp - SSV6X5X USB 传输层 (C++14)
 *
 * 封装 libusb 操作，提供：
 *   - USB 设备打开/关闭（RAII 资源管理）
 *   - 寄存器读/写（CMD/RSP 协议或 EP0 控制传输）
 *   - 固件下载（USB 控制端点批量写入）
 *   - 异步 RX URB 管理（供监控会话使用）
 *
 * 设计特点：
 *   - RAII 模式：构造函数初始化，析构函数自动清理
 *   - 双模式寄存器访问：固件加载前用 CMD/RSP，加载后用 EP0
 *   - 异常安全：所有操作抛出类型化的异常
 *   - 线程安全：单线程设计（非线程安全）
 *
 * 使用示例：
 * @code
 * auto logger = spdlog::stdout_color_mt("usb");
 * ssv6xxx::UsbTransport usb(logger);
 *
 * try {
 *     usb.open(0x8065, 0x6000);  // 打开 SSV6x5x 设备
 *
 *     uint32_t chip_id = 0;
 *     usb.read_reg(ADR_CHIP_ID_0, &chip_id);
 *     logger->info("芯片 ID: 0x{:08X}", chip_id);
 *
 *     usb.close();  // 或依赖析构函数自动关闭
 * } catch (const ssv6xxx::UsbException& e) {
 *     logger->error("USB 错误: {}", e.what());
 * }
 * @endcode
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <libusb-1.0/libusb.h>
#include <memory>
#include <spdlog/spdlog.h>
#include <string>
#include <vector>

#include "ssv6xxx_exceptions.hpp"
#include "ssv6xxx_types.hpp"
#include "ssv6xxx_usb_protocol.hpp"

namespace ssv6xxx
{

    /* ==================== 前向声明 ==================== */
    class UsbTransport;

    /* ==================== 端点信息结构体 ==================== */

    /**
     * @brief USB 端点信息配置
     *
     * 定义 SSV6x5x 芯片的 USB 端点布局。
     * 这些端点号在芯片的 USB 描述符中是固定的。
     */
    struct EndpointInfo {
        uint8_t cmd_ep{0x01};      ///< EP1 OUT - 命令端点（寄存器读写）
        uint8_t rsp_ep{0x82};      ///< EP2 IN  - 响应端点（读操作结果）
        uint8_t tx_ep{0x03};       ///< EP3 OUT - TX 数据端点（帧注入）
        uint8_t rx_ep{0x84};       ///< EP4 IN  - RX 数据端点（帧接收）
        uint16_t packet_size{512}; ///< 最大包大小（全速 USB）
    };

    /** 可供用户选择的 USB 设备信息。 */
    struct UsbDeviceInfo {
        uint16_t vendor_id{0};
        uint16_t product_id{0};
        uint8_t bus{0};
        uint8_t address{0};
        std::string port_path;
        std::string manufacturer;
        std::string product;
        std::string serial;

        std::string location() const;
    };

    /* ==================== USB 传输层类 ==================== */

    /**
     * @brief USB 传输层 - 封装 libusb 操作
     *
     * 职责：
     *   1. **设备管理**：打开/关闭 USB 设备，分离内核驱动
     *   2. **寄存器访问**：提供统一的 read/write 接口，自动选择协议
     *   3. **固件下载**：通过 USB 控制端点批量写入 SRAM
     *   4. **异步传输**：提交和管理 RX URB（供 MonitorSession 使用）
     *   5. **数据传输**：同步 bulk IN/OUT（TX 帧注入）
     *
     * 设计模式：
     *   - **RAII**：析构函数自动调用 close()，确保资源释放
     *   - **策略模式**：通过函数指针切换寄存器访问方式
     *   - **门面模式**：封装复杂的 libusb API 为简洁接口
     *
     * 线程安全：否（设计为单线程使用）
     * 异常安全：强异常安全保证（RAII + 异常抛出）
     */
    class UsbTransport
    {
    public:
        /* ==================== 类型别名 ==================== */

        /**
         * @brief 寄存器读取函数指针类型
         *
         * 用于实现可切换的寄存器访问策略：
         *   - 固件加载前：CMD/RSP 协议
         *   - 固件加载后：EP0 控制传输
         */
        using ReadRegFn = int (UsbTransport::*)(uint32_t, uint32_t *);

        /**
         * @brief 寄存器写入函数指针类型
         */
        using WriteRegFn = int (UsbTransport::*)(uint32_t, uint32_t);

        /* ==================== 构造/析构 ==================== */

        /**
         * @brief 构造函数
         *
         * 初始化内部状态，但不打开 USB 设备。
         * 必须显式调用 open() 才能开始通信。
         *
         * @param logger spdlog 异步 logger（必须非空）
         * @throws std::invalid_argument 如果 logger 为 nullptr
         *
         * @note 日志应从外部注入，支持灵活的日志配置
         */
        explicit UsbTransport(std::shared_ptr<spdlog::logger> logger);

        /**
         * @brief 析构函数（自动调用 close()）
         *
         * 如果设备仍然打开，自动释放资源：
         *   1. 取消所有异步传输
         *   2. 释放接口
         *   3. 关闭设备句柄
         *   4. 释放 libusb context
         *
         * 幂等操作：即使 close() 已经被调用过也不会出错
         */
        ~UsbTransport();

        /* 禁止拷贝和移动（资源唯一性） */
        UsbTransport(const UsbTransport &)            = delete;
        UsbTransport &operator=(const UsbTransport &) = delete;
        UsbTransport(UsbTransport &&)                 = delete;
        UsbTransport &operator=(UsbTransport &&)      = delete;

        /* ==================== 设备管理 ==================== */

        /**
         * @brief 打开 USB 设备
         *
         * 执行完整的设备初始化流程：
         *   1. 初始化 libusb context（如果尚未初始化）
         *   2. 通过 VID/PID 枚举并查找设备
         *   3. 打开设备句柄
         *   4. 分离内核驱动（如果已绑定 ssv6x5x 模块）
         *   5. 声明接口 0（独占访问）
         *
         * @param vendor_id  厂商 ID（默认 0x8065 - SSV）
         * @param product_id 产品 ID（默认 0x6000 - SSV6x5x）
         *
         * @throws UsbException 在以下情况：
         *   - libusb 初始化失败
         *   - 设备未找到（VID/PID 不匹配）
         *   - 无法打开设备句柄
         *   - 权限不足（需要 root/sudo）
         *   - 内核驱动分离失败
         *   - 接口声明失败
         *
         * @warning 需要 root/sudo 权限
         * @warning 一次只能有一个进程声明接口
         */
        void open(const UsbDeviceSelector &selector = UsbDeviceSelector{},
                  uint16_t vendor_id = 0x8065, uint16_t product_id = 0x6000,
                  int android_fd = -1);

        /* 保留旧版显式 VID/PID 调用方式。 */
        void open(uint16_t vendor_id, uint16_t product_id)
        {
            open(UsbDeviceSelector{}, vendor_id, product_id);
        }

        /** 枚举所有匹配 VID/PID 的设备，不声明接口、不改变硬件状态。 */
        static std::vector<UsbDeviceInfo>
        list_devices(uint16_t vendor_id = 0x8065, uint16_t product_id = 0x6000);

        const UsbDeviceInfo &selected_device() const noexcept
        {
            return selected_device_;
        }

        /**
         * @brief 关闭 USB 设备并释放资源
         *
         * 按顺序执行清理操作：
         *   1. 取消所有挂起的异步传输（URB）
         *   2. 释放接口 0
         *   3. 可选：重新绑定内核驱动（恢复内核控制）
         *   4. 关闭设备句柄
         *   5. 释放 libusb context
         *
         * 幂等操作：可重复调用而不出错或抛出异常。
         * 即使设备已经断开连接也能安全调用。
         */
        void close();

        /**
         * @brief 检查设备是否已打开且可用
         * @return true 如果设备已打开且 dev_handle_ 有效
         */
        bool is_open() const noexcept
        {
            return dev_handle_ != nullptr;
        }

        /* ==================== 寄存器访问 ==================== */

        /**
         * @brief 读寄存器（32 位）
         *
         * 根据当前固件状态自动选择传输方式：
         *   - **固件加载前**：CMD/RSP 协议（EP1/EP2 bulk 传输）
         *   - **固件加载后**：EP0 控制传输（更高效，无需序列号匹配）
         *
         * @param addr  寄存器地址（32 位对齐）
         * @param value [输出] 读取的值（必须有效指针）
         * @return 成功返回 0，失败返回负错误码（libusb 错误码）
         *
         * @warning 调用者需检查返回值或捕获异常
         * @see write_reg(), set_bits()
         */
        int read_reg(uint32_t addr, uint32_t *value);

        /**
         * @brief 写寄存器（32 位）
         *
         * 自动选择与 read_reg 相同的传输方式。
         *
         * @param addr  寄存器地址（32 位对齐）
         * @param value 要写入的值
         * @return 成功返回 0，失败返回负错误码
         *
         * @see read_reg(), set_bits()
         */
        int write_reg(uint32_t addr, uint32_t value);

        /**
         * @brief 设置/清除寄存器位（原子操作）
         *
         * 执行 read-modify-write 序列：
         *   1. 读取当前值：`current = read_reg(addr)`
         *   2. 修改位域：`value = (current | set_mask) & ~clr_mask`
         *   3. 写回新值：`write_reg(addr, value)`
         *
         * 典型用途：
         * @code
         * // 设置位 0-7，清除位 8-15
         * usb.set_bits(ADR_REG, 0xFF, 0xFF00);
         * @endcode
         *
         * @param addr     寄存器地址
         * @param set_mask 要设置的位掩码（OR 操作）
         * @param clr_mask 要清除的位掩码（AND NOT 操作）
         * @return 成功返回 0，失败返回负错误码
         */
        int set_bits(uint32_t addr, uint32_t set_mask, uint32_t clr_mask);

        /* ==================== 固件下载 ==================== */

        /**
         * @brief 通过 USB 控制端点下载固件到 SRAM
         *
         * 协议细节：
         *   - 使用 bRequest=0xF0, VENDOR OUT (LIBUSB_REQUEST_TYPE_VENDOR)
         *   - 以 512 字节块写入 SRAM 地址 0x00 开始的位置
         *   - 完全绕过 CMD/RSP 协议（固件尚未运行）
         *
         * @param firmware_data 固件二进制数据指针（必须有效）
         * @param size          数据大小（字节，通常 < 256KB）
         *
         * @throws UsbException 在以下情况：
         *   - firmware_data 为 nullptr
         *   - size 为 0
         *   - USB 控制传输失败
         *   - 写入字节数不匹配
         *
         * @warning 此操作必须在 open() 之后、启动 MCU 之前调用
         * @warning 固件路径由调用者负责验证和读取
         */
        void download_firmware(const uint8_t *firmware_data, size_t size,
                               uint32_t start_addr = 0);

        /* ==================== 异步传输（供 MonitorSession 使用）
         * ==================== */

        /**
         * @brief 提交异步 bulk IN 传输（RX URB）
         *
         * 用于监控模式的连续帧接收。
         * 当帧到达时，libusb 会调用 callback 参数通知完成。
         *
         * @param buf       接收缓冲区（调用者负责生命周期管理）
         * @param buf_len   缓冲区大小（建议 >= MAX_RX_BUFFER_SIZE）
         * @param callback  完成回调函数（LIBUSB_CALL 约定）
         *                  回调参数：struct libusb_transfer*
         * @param user_data 传递给回调的用户自定义数据
         *
         * @return URB 句柄（用于后续 cancel_async_transfer）
         *         失败时返回 nullptr（但通常会抛出 UsbException）
         *
         * @throws UsbException 提交失败时抛出
         *
         * @warning 缓冲区在回调完成前必须保持有效
         * @warning 必须定期调用 handle_events() 以处理完成事件
         *
         * 使用示例：
         * @code
         * // 在 MonitorSession 中使用
         * auto callback = [](struct libusb_transfer* xfer) {
         *     if (xfer->status == LIBUSB_TRANSFER_COMPLETED) {
         *         process_frame(xfer->buffer, xfer->actual_length);
         *     }
         *     // 重新提交 URB 以继续接收
         *     libusb_submit_transfer(xfer);
         * };
         *
         * auto urb = usb.submit_async_rx(rx_buf, sizeof(rx_buf), callback, this);
         * @endcode
         */
        libusb_transfer *submit_async_rx(uint8_t *buf, int buf_len,
                                         libusb_transfer_cb_fn callback,
                                         void *user_data);

        /**
         * @brief 取消异步传输
         *
         * 取消之前通过 submit_async_rx() 提交的 URB。
         * 取消是异步的，URB 可能仍在处理中。
         *
         * @param xfer URB 句柄（由 submit_async_rx 返回）
         *             如果为 nullptr 则忽略
         *
         * @note 取消后应调用 handle_events() 等待取消完成
         */
        void cancel_async_transfer(libusb_transfer *xfer);

        /**
         * @brief 处理 USB 事件（阻塞式超时等待）
         *
         * 必须在主循环中定期调用以处理异步传输完成事件。
         * 这是 libusb 事件驱动的核心机制。
         *
         * @param timeout_ms 超时时间（毫秒）
         *                   - 0：立即返回（非阻塞轮询）
         *                   - >0：阻塞等待最多 timeout_ms 毫秒
         *                   - -1：无限等待（不推荐）
         *
         * 行为：
         *   - 阻塞直到有事件发生或超时
         *   - 有事件时调用对应的 URB 完成回调
         *   - 返回后可以检查是否有新的帧需要处理
         *
         * @warning 必须在 submit_async_rx() 之后调用
         * @warning 不要在回调中再次调用此函数（可能导致死锁）
         *
         * 使用示例：
         * @code
         * // 主循环
         * while (running_) {
         *     usb.handle_events(100);  // 等待最多 100ms
         *     // 处理接收到的帧...
         * }
         * @endcode
         */
        void handle_events(int timeout_ms = 100);

        /* ==================== 批量数据传输 ==================== */

        /**
         * @brief 同步 bulk OUT 传输（TX 帧注入）
         *
         * 用于发送 TX 数据帧到芯片。
         * 帧格式：[ssv6200_tx_desc (80 字节)] [802.11 frame]
         *
         * @param endpoint 端点号（通常 EP3 = 0x03）
         * @param data     数据指针（包含 TX 描述符 + 帧）
         * @param len      数据长度（描述符 + 帧的总长度）
         *
         * @return 实际发送的字节数（成功时 >= 0）
         *         -1 表示错误（检查 errno 或捕获异常）
         *
         * @throws UsbException 传输失败时可能抛出
         *
         * @warning 数据长度不能超过 endpoint 的最大包大小
         * @warning TX 描述符必须正确填充（80 字节）
         */
        int bulk_write(uint8_t endpoint, const uint8_t *data, int len);

        /** 清除指定端点的 halt/stall 状态 */
        int clear_halt(uint8_t endpoint);

        /* ==================== 特殊操作 ==================== */

        /**
         * @brief 系统平台复位（用于 MAC 复位序列）
         *
         * 使用 sysplf_reset 命令绕过正常的 CMD/RSP 序列匹配，
         * 因为复位会导致设备侧序列号计数器清零。
         *
         * @param addr 寄存器地址（通常是平台控制寄存器）
         * @param mask 位掩码（要复位的位）
         *
         * @note 仅在 MAC 复位阶段使用（Task 6 的阶段 2）
         * @warning 此操作会导致设备短暂无响应
         */
        void sysplf_reset(uint32_t addr, uint32_t mask);

        /**
         * @brief 读取芯片 ID 信息
         *
         * 读取 ADR_CHIP_ID_[0-3] 寄存器并格式化为人类可读字符串。
         *
         * @return 芯片 ID 字符串（如 "SSV6256P", "SSV6200" 等）
         *         格式："SSVXXXX"
         *
         * @throws UsbException 读取失败时抛出
         *
         * @note 应在 open() 后立即调用以验证设备身份
         */
        std::string read_chip_id();

        /* ==================== 寄存器访问模式切换 ==================== */

        /**
         * @brief 切换到 EP0 控制传输模式（固件加载后使用）
         *
         * 固件加载完成后，切换到更高效的 EP0 控制传输模式。
         * EP0 模式无需 CMD/RSP 序列号匹配，减少开销。
         *
         * 调用时机：
         * - 固件下载完成且 MCU 启用后
         * - shutdown() 前应切换回 bulk 模式
         *
         * @see switch_to_bulk_mode()
         *
         * 使用示例：
         * @code
         * usb.download_firmware(fw_data, fw_size);
         * usb.switch_to_ep0_mode();  // 切换到高效模式
         * // ... 后续寄存器访问使用 EP0 ...
         * usb.switch_to_bulk_mode(); // 关闭前切回 bulk 模式
         * @endcode
         */
        void switch_to_ep0_mode();

        /**
         * @brief 切换回 CMD/RSP bulk 传输模式（固件加载前/关闭时使用）
         *
         * 切换回默认的 CMD/RSP 协议模式，通过 EP1/EP2 bulk 端点通信。
         *
         * 调用时机：
         * - shutdown() 前（确保关闭序列能正常工作）
         * - 固件未加载或 MCU 未运行时
         *
         * @see switch_to_ep0_mode()
         */
        void switch_to_bulk_mode();

        /**
         * @brief 探测 EP0 模式是否可用（使用短超时）
         *
         * 在切换到 EP0 模式后调用，验证固件是否准备好处理 EP0 控制传输。
         * 使用较短的超时时间（500ms）避免长时间阻塞。
         *
         * @param test_addr 测试寄存器地址（默认使用 FW_VERSION_REG）
         * @return true 如果 EP0 可用，false 如果不可用
         *
         * @note 此方法不会抛出异常，失败时返回 false
         */
        bool probe_ep0(uint32_t test_addr = 0xC0000100);

        /**
         * @brief 发送固件 HCI host command
         *
         * 通过 TX bulk 端点发送 HOST_CMD 数据包；可选等待固件在 RX 端点返回
         * HOST_EVENT 响应。用于 MRX_MODE / RFPHY_OPS 等固件控制命令。
         */
        int send_host_cmd(uint8_t h_cmd, uint32_t sub_h_cmd, const void *payload,
                          uint32_t payload_len, bool wait_response = false);

    private:
        /* ==================== 日志 ==================== */
        std::shared_ptr<spdlog::logger> logger_; ///< 异步日志记录器

        /* ==================== libusb 上下文 ==================== */
        libusb_context *ctx_{nullptr};              ///< libusb 会话上下文
        libusb_device_handle *dev_handle_{nullptr}; ///< USB 设备句柄

        /* ==================== 端点信息 ==================== */
        EndpointInfo ep_; ///< USB 端点配置

        /* ==================== 命令/响应缓冲区 ==================== */
        uint8_t cmd_buf_[64]{}; ///< 命令发送缓冲区
        uint8_t rsp_buf_[64]{}; ///< 响应接收缓冲区

        /* ==================== 序列号管理 ==================== */
        uint16_t sequence_{0};         ///< CMD/RSP 序列号计数器
        uint8_t host_cmd_sequence_{1}; ///< 固件阻塞命令的唯一序列号（1..127）

        UsbDeviceInfo selected_device_; ///< 当前已打开设备的稳定位置信息

        /* ==================== 芯片标识 ==================== */
        uint8_t chip_id_str_[24]{}; ///< 芯片 ID 字符串缓存

        /* ==================== 可切换的寄存器访问函数指针 ==================== */
        ReadRegFn read_reg_fn_{nullptr};   ///< 当前读寄存器函数
        WriteRegFn write_reg_fn_{nullptr}; ///< 当前写寄存器函数

        /* ==================== 私有辅助方法 ==================== */

        /** 通过 CMD/RSP 协议读寄存器（固件加载前） */
        int read_reg_cmd(uint32_t addr, uint32_t *value);

        /** 通过 CMD/RSP 协议写寄存器（固件加载前） */
        int write_reg_cmd(uint32_t addr, uint32_t value);

        /** 通过 EP0 控制传输读寄存器（固件加载后） */
        int read_reg_ep0(uint32_t addr, uint32_t *value);

        /** 通过 EP0 控制传输写寄存器（固件加载后） */
        int write_reg_ep0(uint32_t addr, uint32_t value);

        /** 分离内核驱动（如果已绑定 ssv6x5x 模块） */
        void detach_kernel_driver();

        /** 获取下一个 CMD/RSP 序列号（循环递增） */
        uint16_t next_seq();

        /** 清空响应端点的残留数据（防止污染后续读取） */
        void drain_rsp();

        /** 清除已知端点的 STALL 状态（恢复传输能力） */
        void clear_known_halts();

        /** CMD/RSP 协议核心方法 */
        int __usb_cmd(uint8_t cmd, void *data, uint32_t data_len, void *result);
        int usb_send_cmd(uint8_t cmd, uint16_t seq, const void *data,
                         uint32_t data_len);
        int usb_recv_rsp(int size, int *rsp_len);
    };

} // namespace ssv6xxx
