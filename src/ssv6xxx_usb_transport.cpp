/*
 * ssv6xxx_usb_transport.cpp - SSV6X5X USB 传输层实现 (C++14)
 *
 * 从 ssv6xxx_usb.cpp 迁移并重构为面向对象风格。
 *
 * 主要迁移内容：
 *   - 设备打开/关闭（RAII 资源管理）
 *   - 寄存器访问（CMD/RSP + EP0 双模式）
 *   - 固件下载（USB 控制端点批量写入）
 *   - 异步 RX URB 管理
 *   - 特殊操作（sysplf_reset, read_chip_id）
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ssv6xxx_usb_transport.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <sstream>
#include <spdlog/spdlog.h>
#include <unistd.h>
#include <vector>

#include "ssv6xxx_descriptors.hpp"
#include "ssv6xxx_regs.hpp"

namespace
{

    /* ==================== 常量定义 ==================== */

    constexpr int TRANSACTION_TIMEOUT = 3000; ///< USB 传输超时（毫秒）
    constexpr int USB_CMD_SEQUENCE    = 255;  ///< 序列号最大值
    constexpr int MAX_FW_BLOCK        = 512;  ///< 固件下载块大小
    constexpr int MAX_RETRY_INNER     = 5;    ///< 内部发送/接收重试次数
    constexpr int MAX_CMD_RETRY       = 3;    ///< 外部命令循环重试次数
    constexpr int EP0_MAX_RETRY =
        10; ///< EP0 控制传输最大重试次数（避免长时间阻塞）

#define CMD_HDR_SIZE (sizeof(Ssv6xxxCmdHdr) - sizeof(Ssv6xxxPayload))

    /* 固件下载地址重映射常量 */
    constexpr uint32_t SKIP_START      = 0x10000; ///< 跳过区域起始地址
    constexpr uint32_t SKIP_END        = 0x10B00; ///< 跳过区域结束地址
    constexpr uint32_t REMAP_THRESHOLD = 0x10000; ///< 重映射阈值
    constexpr uint32_t REMAP_OFFSET    = 0xF0000; ///< 重映射偏移量

    std::string usb_port_path(libusb_device *device)
    {
        uint8_t ports[8]{};
        int count = libusb_get_port_numbers(device, ports, sizeof(ports));
        if (count <= 0)
            return {};

        std::ostringstream out;
        for (int i = 0; i < count; ++i) {
            if (i != 0)
                out << '.';
            out << static_cast<unsigned>(ports[i]);
        }
        return out.str();
    }

    std::string usb_string(libusb_device_handle *handle, uint8_t index)
    {
        if (!handle || index == 0)
            return {};

        unsigned char value[256]{};
        int len = libusb_get_string_descriptor_ascii(handle, index, value,
                                                     static_cast<int>(sizeof(value)));
        return len > 0 ? std::string(reinterpret_cast<char *>(value), len)
                       : std::string{};
    }

    ssv6xxx::UsbDeviceInfo inspect_usb_device(libusb_device *device,
                                              const libusb_device_descriptor &desc,
                                              bool read_strings)
    {
        ssv6xxx::UsbDeviceInfo info;
        info.vendor_id  = desc.idVendor;
        info.product_id = desc.idProduct;
        info.bus        = libusb_get_bus_number(device);
        info.address    = libusb_get_device_address(device);
        info.port_path  = usb_port_path(device);

        if (read_strings) {
            libusb_device_handle *handle = nullptr;
            if (libusb_open(device, &handle) == 0) {
                info.manufacturer = usb_string(handle, desc.iManufacturer);
                info.product      = usb_string(handle, desc.iProduct);
                info.serial       = usb_string(handle, desc.iSerialNumber);
                libusb_close(handle);
            }
        }
        return info;
    }

    uint32_t read_le_u32(const uint8_t *data)
    {
        return static_cast<uint32_t>(data[0]) |
               (static_cast<uint32_t>(data[1]) << 8) |
               (static_cast<uint32_t>(data[2]) << 16) |
               (static_cast<uint32_t>(data[3]) << 24);
    }

} // anonymous namespace

namespace ssv6xxx
{

    std::string UsbDeviceInfo::location() const
    {
        std::ostringstream out;
        out << "bus=" << static_cast<unsigned>(bus)
            << " address=" << static_cast<unsigned>(address);
        if (!port_path.empty())
            out << " port=" << port_path;
        return out.str();
    }

    /* ==================== 构造/析构 ==================== */

    UsbTransport::UsbTransport(std::shared_ptr<spdlog::logger> logger)
        : logger_(std::move(logger))
    {
        if (!logger_) {
            throw std::invalid_argument("UsbTransport: logger 不能为空");
        }

        /* 初始化缓冲区 */
        std::memset(cmd_buf_, 0, sizeof(cmd_buf_));
        std::memset(rsp_buf_, 0, sizeof(rsp_buf_));
        std::memset(chip_id_str_, 0, sizeof(chip_id_str_));

        /* 默认使用 CMD/RSP 协议（固件加载前） */
        read_reg_fn_  = nullptr; // 将在 read_reg/write_reg 中动态选择
        write_reg_fn_ = nullptr;

        logger_->debug("[USB] 构造函数完成");
    }

    UsbTransport::~UsbTransport()
    {
        close();
        logger_->debug("[USB] 析构函数完成");
    }

    /* ==================== 设备管理 ==================== */

    std::vector<UsbDeviceInfo> UsbTransport::list_devices(uint16_t vendor_id,
                                                          uint16_t product_id)
    {
        libusb_context *context = nullptr;
        int ret                 = libusb_init(&context);
        if (ret < 0) {
            throw UsbException(std::string("libusb_init 失败: ") +
                               libusb_error_name(ret));
        }

        libusb_device **list = nullptr;
        ssize_t count        = libusb_get_device_list(context, &list);
        if (count < 0) {
            libusb_exit(context);
            throw UsbException(std::string("libusb_get_device_list 失败: ") +
                               libusb_error_name(static_cast<int>(count)));
        }

        std::vector<UsbDeviceInfo> devices;
        for (ssize_t i = 0; i < count; ++i) {
            libusb_device_descriptor desc{};
            if (libusb_get_device_descriptor(list[i], &desc) < 0)
                continue;
            if (desc.idVendor == vendor_id && desc.idProduct == product_id)
                devices.push_back(inspect_usb_device(list[i], desc, false));
        }

        libusb_free_device_list(list, 1);
        libusb_exit(context);
        return devices;
    }

    void UsbTransport::open(const UsbDeviceSelector &selector, uint16_t vendor_id,
                            uint16_t product_id, int android_fd)
    {
        if (dev_handle_) {
            throw UsbException("设备已经打开，请先调用 close()");
        }

        if (android_fd >= 0) {
            int option_ret = libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
            if (option_ret < 0)
                throw UsbException("无法禁用 USB 设备枚举");
        }
        logger_->info("[USB] 正在初始化 libusb context...");
        int ret = libusb_init(&ctx_);
        if (ret < 0) {
            throw UsbException(std::string("libusb_init 失败: ") +
                               libusb_error_name(ret));
        }

        if (android_fd >= 0) {
            ret = libusb_wrap_sys_device(ctx_, static_cast<intptr_t>(android_fd), &dev_handle_);
            if (ret < 0) {
                libusb_exit(ctx_);
                ctx_ = nullptr;
                throw UsbException(std::string("无法打开 Android USB 描述符: ") + libusb_error_name(ret));
            }
            selected_device_ = UsbDeviceInfo{};
        } else {
        /* 查找设备 */
        logger_->info("[USB] 正在查找设备 (VID=0x{:04X}, PID=0x{:04X})...", vendor_id,
                      product_id);
        libusb_device **list;
        ssize_t cnt = libusb_get_device_list(ctx_, &list);
        if (cnt < 0) {
            libusb_exit(ctx_);
            ctx_ = nullptr;
            throw UsbException(std::string("libusb_get_device_list 失败: ") +
                               std::to_string(cnt));
        }

        std::vector<libusb_device *> candidates;
        std::vector<UsbDeviceInfo> candidate_info;
        for (ssize_t i = 0; i < cnt; i++) {
            libusb_device_descriptor desc{};
            int r = libusb_get_device_descriptor(list[i], &desc);
            if (r < 0)
                continue;
            if (desc.idVendor == vendor_id && desc.idProduct == product_id) {
                UsbDeviceInfo info = inspect_usb_device(list[i], desc, false);
                bool matches =
                    (selector.bus < 0 || selector.bus == info.bus) &&
                    (selector.address < 0 || selector.address == info.address) &&
                    (selector.port_path.empty() || selector.port_path == info.port_path);
                if (matches) {
                    candidates.push_back(list[i]);
                    candidate_info.push_back(std::move(info));
                }
            }
        }

        if (candidates.empty()) {
            libusb_free_device_list(list, 1);
            libusb_exit(ctx_);
            ctx_ = nullptr;
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "未找到符合选择条件的 SSV6x5x 设备 (VID=0x%04X, PID=0x%04X)",
                     vendor_id, product_id);
            throw UsbException(std::string(buf));
        }

        if (candidates.size() > 1) {
            std::ostringstream message;
            message << "检测到 " << candidates.size()
                    << " 张 SSV6x5x 网卡，必须使用 --usb-port 或 "
                       "--usb-bus/--usb-address 指定目标：";
            for (const auto &info : candidate_info)
                message << " [" << info.location() << ']';
            libusb_free_device_list(list, 1);
            libusb_exit(ctx_);
            ctx_ = nullptr;
            throw UsbException(message.str());
        }

        libusb_device *found = candidates.front();
        selected_device_     = candidate_info.front();
        libusb_ref_device(found);
        libusb_free_device_list(list, 1);

        /* 打开设备 */
        logger_->info("[USB] 正在打开设备 ({})...", selected_device_.location());
        ret = libusb_open(found, &dev_handle_);
        libusb_unref_device(found);
        if (ret < 0) {
            libusb_exit(ctx_);
            ctx_        = nullptr;
            dev_handle_ = nullptr;
            throw UsbException(std::string("无法打开设备: ") + libusb_error_name(ret));
        }

        }
        /* 分离内核驱动（如果已绑定） */
        detach_kernel_driver();

        /* 声明接口 0 */
        logger_->info("[USB] 正在声明接口 0...");
        ret = libusb_claim_interface(dev_handle_, 0);
        if (ret < 0) {
            libusb_close(dev_handle_);
            dev_handle_ = nullptr;
            libusb_exit(ctx_);
            ctx_ = nullptr;
            throw UsbException(std::string("无法声明接口: ") + libusb_error_name(ret));
        }

        /* 获取端点信息 */
        libusb_config_descriptor *config;
        ret = libusb_get_active_config_descriptor(libusb_get_device(dev_handle_), &config);
        if (ret == 0 && config) {
            for (int if_idx = 0; if_idx < config->bNumInterfaces; if_idx++) {
                const libusb_interface *intf = &config->interface[if_idx];
                for (int alts = 0; alts < intf->num_altsetting; alts++) {
                    const libusb_interface_descriptor *alt = &intf->altsetting[alts];
                    for (int ep = 0; ep < alt->bNumEndpoints; ep++) {
                        const libusb_endpoint_descriptor *epd = &alt->endpoint[ep];
                        uint8_t epnum                         = epd->bEndpointAddress & 0x0F;

                        switch (epnum) {
                            case SSV_EP_CMD:
                                ep_.cmd_ep      = epd->bEndpointAddress;
                                ep_.packet_size = epd->wMaxPacketSize;
                                logger_->info("[USB] CMD EP: 0x{:02X} (max {})",
                                              epd->bEndpointAddress, epd->wMaxPacketSize);
                                break;
                            case SSV_EP_RSP:
                                ep_.rsp_ep = epd->bEndpointAddress;
                                logger_->info("[USB] RSP EP: 0x{:02X} (max {})",
                                              epd->bEndpointAddress, epd->wMaxPacketSize);
                                break;
                            case SSV_EP_TX:
                                ep_.tx_ep = epd->bEndpointAddress;
                                logger_->info("[USB] TX  EP: 0x{:02X} (max {})",
                                              epd->bEndpointAddress, epd->wMaxPacketSize);
                                break;
                            case SSV_EP_RX:
                                ep_.rx_ep = epd->bEndpointAddress;
                                logger_->info("[USB] RX  EP: 0x{:02X} (max {})",
                                              epd->bEndpointAddress, epd->wMaxPacketSize);
                                break;
                        }
                    }
                }
            }
            libusb_free_config_descriptor(config);
        }
        /* 清理端点状态并排空响应缓冲区 */
        clear_known_halts();
        drain_rsp();

        sequence_          = 0;
        host_cmd_sequence_ = 1;
        logger_->info("[USB] 设备打开成功：{}", selected_device_.location());
    }

    void UsbTransport::close()
    {
        if (!dev_handle_ && !ctx_)
            return;

        logger_->info("[USB] 正在关闭设备...");

        /* 取消所有异步传输、清理端点、释放接口 */
        if (dev_handle_) {
            clear_known_halts();
            drain_rsp();
        }

        if (dev_handle_) {
            libusb_release_interface(dev_handle_, 0);
            libusb_close(dev_handle_);
            dev_handle_ = nullptr;
        }

        if (ctx_) {
            libusb_exit(ctx_);
            ctx_ = nullptr;
        }

        selected_device_ = UsbDeviceInfo{};

        logger_->info("[USB] 设备已关闭");
    }

    /* ==================== 私有辅助方法 ==================== */

    uint16_t UsbTransport::next_seq()
    {
        sequence_ = sequence_ % USB_CMD_SEQUENCE;
        sequence_++;
        return sequence_;
    }

    void UsbTransport::drain_rsp()
    {
        int rsp_len;
        int retry = 4;
        while (retry--) {
            int ret = libusb_bulk_transfer(dev_handle_, ep_.rsp_ep, rsp_buf_,
                                           static_cast<int>(sizeof(Ssv6xxxCmdHdr)),
                                           &rsp_len, 50);
            if (ret < 0)
                break;
        }
    }

    void UsbTransport::clear_known_halts()
    {
        if (ep_.cmd_ep)
            libusb_clear_halt(dev_handle_, ep_.cmd_ep);
        if (ep_.rsp_ep)
            libusb_clear_halt(dev_handle_, ep_.rsp_ep);
        if (ep_.tx_ep)
            libusb_clear_halt(dev_handle_, ep_.tx_ep);
        if (ep_.rx_ep)
            libusb_clear_halt(dev_handle_, ep_.rx_ep);
    }

    void UsbTransport::detach_kernel_driver()
    {
        if (libusb_kernel_driver_active(dev_handle_, 0) == 1) {
            int ret = libusb_detach_kernel_driver(dev_handle_, 0);
            if (ret == 0) {
                logger_->info("[USB] 已分离内核驱动 (ssv6x5x)");
            } else {
                logger_->warn("[USB] 分离内核驱动失败: {}", libusb_error_name(ret));
            }
        }
    }

    /* ==================== CMD/RSP 协议核心方法 ==================== */

    int UsbTransport::usb_send_cmd(uint8_t cmd, uint16_t seq, const void *data,
                                   uint32_t data_len)
    {
        int transferred = 0;
        auto *hdr       = reinterpret_cast<Ssv6xxxCmdHdr *>(cmd_buf_);

        std::memset(hdr, 0, sizeof(Ssv6xxxCmdHdr));
        hdr->plen = data_len & 0xFF;
        hdr->cmd  = cmd;
        hdr->seq  = seq;
        if (data && data_len > 0)
            std::memcpy(&hdr->payload, data, data_len);

        int total_len = static_cast<int>(data_len + CMD_HDR_SIZE);
        int ret       = libusb_bulk_transfer(dev_handle_, ep_.cmd_ep, cmd_buf_, total_len,
                                             &transferred, TRANSACTION_TIMEOUT);
        if (ret < 0) {
            logger_->warn("[USB] CMD 发送失败: {}", libusb_error_name(ret));
        }
        return ret;
    }

    int UsbTransport::usb_recv_rsp(int size, int *rsp_len)
    {
        int transferred = 0;
        int ret         = libusb_bulk_transfer(dev_handle_, ep_.rsp_ep, rsp_buf_, size,
                                               &transferred, TRANSACTION_TIMEOUT);
        if (ret < 0) {
            *rsp_len = 0;
        } else {
            *rsp_len = transferred;
        }
        return ret;
    }

    int UsbTransport::__usb_cmd(uint8_t cmd, void *data, uint32_t data_len,
                                void *result)
    {
        int ret = -1, rsp_len = 0, i;
        uint16_t sequence;
        Ssv6xxxCmdHdr *rsphdr;
        uint32_t retry_times;

        sequence = next_seq();

        /* 内部重试发送 */
        retry_times = 0;
        do {
            ret = usb_send_cmd(cmd, sequence, data, data_len);
        } while (ret < 0 && (++retry_times < MAX_RETRY_INNER));

        if (ret < 0) {
            logger_->warn("[USB] __usb_cmd: 发送失败（{} 次重试后），seq={}",
                          retry_times, sequence);
            return ret;
        }

        /* 接收响应，循环匹配序列号 */
        for (i = 0; i < USB_CMD_SEQUENCE; i++) {
            retry_times = 0;
            do {
                ret = usb_recv_rsp(static_cast<int>(sizeof(Ssv6xxxCmdHdr)), &rsp_len);
            } while (ret < 0 && (++retry_times < MAX_RETRY_INNER));

            if (ret < 0) {
                drain_rsp();
                logger_->warn("[USB] __usb_cmd: 接收失败（{} 次重试后），seq={}",
                              retry_times, sequence);
                return ret;
            }
            if (rsp_len < CMD_HDR_SIZE) {
                logger_->warn("[USB] __usb_cmd: 响应过短（{} 字节）", rsp_len);
                return -1;
            }
            rsphdr = reinterpret_cast<Ssv6xxxCmdHdr *>(rsp_buf_);
            if (rsphdr->seq == sequence)
                break;
        }

        rsphdr = reinterpret_cast<Ssv6xxxCmdHdr *>(rsp_buf_);
        switch (rsphdr->cmd) {
            case SSV6200_CMD_WRITE_REG:
                break;
            case SSV6200_CMD_READ_REG:
                if (result)
                    std::memcpy(result, &rsphdr->payload, sizeof(Ssv6xxxReadRegResult));
                break;
            default:
                logger_->warn("[USB] __usb_cmd: 未知响应命令 {}", rsphdr->cmd);
                return -1;
        }

        return 0;
    }

    /* ==================== 寄存器访问 - CMD/RSP 模式（固件加载前）
     * ==================== */

    int UsbTransport::read_reg_cmd(uint32_t addr, uint32_t *value)
    {
        Ssv6xxxReadReg read_reg;
        Ssv6xxxReadRegResult result;

        std::memset(&read_reg, 0, sizeof(read_reg));
        read_reg.addr = addr;

        int ret =
            __usb_cmd(SSV6200_CMD_READ_REG, &read_reg, sizeof(read_reg), &result);
        if (ret == 0)
            *value = result.value;
        else
            *value = 0xFFFFFFFF;

        return ret;
    }

    int UsbTransport::write_reg_cmd(uint32_t addr, uint32_t value)
    {
        Ssv6xxxWriteReg write_reg;

        std::memset(&write_reg, 0, sizeof(write_reg));
        write_reg.addr  = addr;
        write_reg.value = value;

        return __usb_cmd(SSV6200_CMD_WRITE_REG, &write_reg, sizeof(write_reg),
                         nullptr);
    }

    /* ==================== 寄存器访问 - EP0 模式（固件加载后） ====================
     */

    int UsbTransport::read_reg_ep0(uint32_t addr, uint32_t *value)
    {
        Ssv6xxxReadRegResult result;
        uint16_t wValue = static_cast<uint16_t>(addr & 0xFFFF);
        uint16_t wIndex = static_cast<uint16_t>((addr >> 16) & 0xFFFF);

        for (int i = 0; i < EP0_MAX_RETRY; i++) {
            int ret = libusb_control_transfer(
                dev_handle_,
                LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR |
                    LIBUSB_RECIPIENT_DEVICE,
                VENDOR_REG_RW, wValue, wIndex,
                reinterpret_cast<unsigned char *>(&result),
                static_cast<uint16_t>(sizeof(result)), TRANSACTION_TIMEOUT);

            if (ret >= 0) {
                *value = result.value;
                return 0;
            }
        }

        logger_->warn("[USB] EP0 读寄存器 0x{:08x} 失败（{} 次重试）", addr,
                      EP0_MAX_RETRY);
        *value = 0xFFFFFFFF;
        return -1;
    }

    int UsbTransport::write_reg_ep0(uint32_t addr, uint32_t value)
    {
        uint16_t addr_lo = static_cast<uint16_t>(addr & 0xFFFF);
        uint16_t addr_hi = static_cast<uint16_t>((addr >> 16) & 0xFFFF);

        for (int i = 0; i < EP0_MAX_RETRY; i++) {
            /* 步骤 1：发送地址 */
            int ret = libusb_control_transfer(
                dev_handle_,
                LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
                    LIBUSB_RECIPIENT_DEVICE,
                VENDOR_REG_RW, addr_lo, addr_hi, nullptr, 0, TRANSACTION_TIMEOUT);

            if (ret < 0)
                continue;

            /* 步骤 2：发送值 */
            uint16_t val_lo = static_cast<uint16_t>(value & 0xFFFF);
            uint16_t val_hi = static_cast<uint16_t>((value >> 16) & 0xFFFF);

            ret = libusb_control_transfer(
                dev_handle_,
                LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR |
                    LIBUSB_RECIPIENT_DEVICE,
                VENDOR_REG_RW_WDATA, val_hi, val_lo, nullptr, 0, TRANSACTION_TIMEOUT);

            if (ret >= 0)
                return 0;
        }

        logger_->warn("[USB] EP0 写寄存器 0x{:08x} = 0x{:08x} 失败（{} 次重试）",
                      addr, value, EP0_MAX_RETRY);
        return -1;
    }

    /* ==================== 公共寄存器访问接口 ==================== */

    int UsbTransport::read_reg(uint32_t addr, uint32_t *value)
    {
        if (!dev_handle_) {
            throw UsbException("设备未打开，无法读取寄存器");
        }

        /* 如果函数指针未设置，默认使用 CMD/RSP 模式 */
        if (!read_reg_fn_) {
            return read_reg_cmd(addr, value);
        }
        return (this->*read_reg_fn_)(addr, value);
    }

    int UsbTransport::write_reg(uint32_t addr, uint32_t value)
    {
        if (!dev_handle_) {
            throw UsbException("设备未打开，无法写入寄存器");
        }

        /* 如果函数指针未设置，默认使用 CMD/RSP 模式 */
        if (!write_reg_fn_) {
            return write_reg_cmd(addr, value);
        }
        return (this->*write_reg_fn_)(addr, value);
    }

    int UsbTransport::set_bits(uint32_t addr, uint32_t set_mask,
                               uint32_t clr_mask)
    {
        uint32_t val;
        int ret = read_reg(addr, &val);
        if (ret < 0)
            return ret;

        val &= ~clr_mask;
        val |= set_mask;

        return write_reg(addr, val);
    }

    /* ==================== 固件下载 ==================== */

    void UsbTransport::download_firmware(const uint8_t *firmware_data, size_t size,
                                         uint32_t start_addr)
    {
        if (!firmware_data || size == 0) {
            throw UsbException("固件数据无效（空指针或大小为 0）");
        }
        if (!dev_handle_) {
            throw UsbException("设备未打开，无法下载固件");
        }

        /* 内核 hal.c: ILM64K/DLM128K 模式下，0x10000 以上地址需要 +0xF0000 重映射。
         */
        uint32_t sramcfg = 0;
        read_reg(0xC0000128, &sramcfg);
        bool need_remap = ((sramcfg & 0x02) == 0);

        if (start_addr == FW_START_ADDR) {
            logger_->info("[USB] SRAM 模式: {} (sramcfg=0x{:08X})",
                          (sramcfg & 0x02) ? "ILM160K" : "ILM64K", sramcfg);
            logger_->info("[USB] 地址重映射: {}", need_remap ? "启用" : "关闭");
        }

        int offset    = 0;
        uint32_t addr = start_addr;
        int ret       = 0, transfer;

        while (offset < static_cast<int>(size)) {
            int remaining = static_cast<int>(size - offset);
            transfer      = (remaining < MAX_FW_BLOCK) ? remaining : MAX_FW_BLOCK;

            /* 应用 SRAM 地址重映射（mode 0） */
            uint32_t hw_addr = addr;
            if (need_remap && hw_addr >= REMAP_THRESHOLD)
                hw_addr += REMAP_OFFSET;

            /* 跳过 0x10000-0x10AFF 区域（Turismo C0/D0 USB 硬件 bug） */
            if (addr < SKIP_END && addr + transfer > SKIP_START) {
                if (addr >= SKIP_START) {
                    if (addr + transfer <= SKIP_END) {
                        addr += transfer;
                        offset += transfer;
                        continue;
                    }

                    int prefix_skip  = static_cast<int>(SKIP_END - addr);
                    int suffix       = transfer - prefix_skip;
                    uint32_t skip_hw = SKIP_END;
                    if (need_remap)
                        skip_hw += REMAP_OFFSET;
                    ret = libusb_control_transfer(
                        dev_handle_, 0x40, FIRMWARE_DOWNLOAD,
                        static_cast<uint16_t>(skip_hw & 0xFFFF),
                        static_cast<uint16_t>((skip_hw >> 16) & 0xFFFF),
                        const_cast<unsigned char *>(firmware_data + offset + prefix_skip),
                        static_cast<uint16_t>(suffix), TRANSACTION_TIMEOUT);
                    if (ret < 0) {
                        char buf[256];
                        snprintf(buf, sizeof(buf), "固件下载失败（偏移 %d）：%s", offset,
                                 libusb_error_name(ret));
                        throw UsbException(std::string(buf));
                    }
                    addr += transfer;
                    offset += transfer;
                    continue;
                } else {
                    int prefix = static_cast<int>(SKIP_START - addr);
                    int suffix = transfer - prefix;
                    if (suffix > 0) {
                        uint32_t skip_hw = SKIP_END;
                        if (need_remap)
                            skip_hw += REMAP_OFFSET;
                        ret = libusb_control_transfer(
                            dev_handle_, 0x40, FIRMWARE_DOWNLOAD,
                            static_cast<uint16_t>(skip_hw & 0xFFFF),
                            static_cast<uint16_t>((skip_hw >> 16) & 0xFFFF),
                            const_cast<unsigned char *>(firmware_data + offset + prefix),
                            static_cast<uint16_t>(suffix), TRANSACTION_TIMEOUT);
                        if (ret < 0) {
                            char buf[256];
                            snprintf(buf, sizeof(buf), "固件下载失败（偏移 %d）：%s", offset,
                                     libusb_error_name(ret));
                            throw UsbException(std::string(buf));
                        }
                    }
                    addr += transfer;
                    offset += transfer;
                    continue;
                }
            }

            /* 通过 USB 控制端点写入 SRAM */
            ret = libusb_control_transfer(
                dev_handle_, 0x40, FIRMWARE_DOWNLOAD,
                static_cast<uint16_t>(hw_addr & 0xFFFF),
                static_cast<uint16_t>((hw_addr >> 16) & 0xFFFF),
                const_cast<unsigned char *>(firmware_data + offset), transfer,
                TRANSACTION_TIMEOUT);

            if (ret < 0) {
                char buf[256];
                snprintf(buf, sizeof(buf), "固件下载失败（偏移 %d）：%s", offset,
                         libusb_error_name(ret));
                throw UsbException(std::string(buf));
            }

            addr += transfer;
            offset += transfer;
        }
    }

    /* ==================== 异步传输 ==================== */

    libusb_transfer *UsbTransport::submit_async_rx(uint8_t *buf, int buf_len,
                                                   libusb_transfer_cb_fn callback,
                                                   void *user_data)
    {
        if (!dev_handle_) {
            throw UsbException("设备未打开，无法提交异步传输");
        }
        if (!buf || buf_len <= 0) {
            throw UsbException("无效的缓冲区参数");
        }

        auto *xfer = libusb_alloc_transfer(0);
        if (!xfer) {
            throw UsbException("分配 URB 失败（内存不足）");
        }

        libusb_fill_bulk_transfer(xfer, dev_handle_, ep_.rx_ep, buf, buf_len,
                                  callback, user_data, 0);

        int ret = libusb_submit_transfer(xfer);
        if (ret < 0) {
            libusb_free_transfer(xfer);
            char buf[128];
            snprintf(buf, sizeof(buf), "提交 URB 失败：%s", libusb_error_name(ret));
            throw UsbException(std::string(buf));
        }

        return xfer;
    }

    void UsbTransport::cancel_async_transfer(libusb_transfer *xfer)
    {
        if (!xfer || !dev_handle_)
            return;

        libusb_cancel_transfer(xfer);

        /* 等待取消完成 */
        timeval tv = {1, 0};
        libusb_handle_events_timeout(ctx_, &tv);
    }

    void UsbTransport::handle_events(int timeout_ms)
    {
        if (!ctx_)
            return;

        timeval tv;
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        libusb_handle_events_timeout(ctx_, &tv);
    }

    /* ==================== 批量数据传输 ==================== */

    int UsbTransport::bulk_write(uint8_t endpoint, const uint8_t *data, int len)
    {
        if (!dev_handle_)
            return -ENODEV;

        int transferred = 0;
        int ret         = libusb_bulk_transfer(dev_handle_, endpoint,
                                               const_cast<unsigned char *>(data), len,
                                               &transferred, TRANSACTION_TIMEOUT);
        if (ret < 0) {
            logger_->warn("[USB] TX 失败: {}", libusb_error_name(ret));
        }
        return ret;
    }

    int UsbTransport::clear_halt(uint8_t endpoint)
    {
        if (!dev_handle_)
            return -ENODEV;

        int ret = libusb_clear_halt(dev_handle_, endpoint);
        if (ret < 0) {
            logger_->warn("[USB] clear_halt(0x{:02X}) 失败: {}", endpoint,
                          libusb_error_name(ret));
        }
        return ret;
    }

    /* ==================== 特殊操作 ==================== */

    void UsbTransport::sysplf_reset(uint32_t addr, uint32_t mask)
    {
        if (!dev_handle_) {
            throw UsbException("设备未打开，无法执行系统复位");
        }

        int ret, rsp_len = 0;
        uint16_t sequence;

        Ssv6xxxWriteReg write_reg;
        std::memset(&write_reg, 0, sizeof(write_reg));
        write_reg.addr  = addr;
        write_reg.value = mask;

        sequence = next_seq();

        /* 直接发送命令（不使用重试包装器，因为复位会导致序列号错误） */
        ret = usb_send_cmd(SSV6200_CMD_WRITE_REG, sequence, &write_reg,
                           sizeof(write_reg));
        if (ret < 0) {
            logger_->warn("[USB] sysplf_reset: 发送失败（地址 0x{:08x}）", addr);
            return;
        }

        /* 读取响应（不匹配序列号，使用短超时） */
        ret = usb_recv_rsp(static_cast<int>(sizeof(Ssv6xxxCmdHdr)), &rsp_len);

        /* ★ MAC 复位后 USB 端点状态可能不确定，必须清理：
         *   1. 清除端点 STALL 状态（恢复传输能力）
         *   2. 排空 RSP 端点残留数据（防止污染后续读取）
         *   3. 重置序列号计数器（设备侧已清零）
         *
         * 内核驱动在 MAC 复位后不需要这些操作，因为内核的 USB URB
         * 框架会自动处理端点状态恢复。但 libusb 的用户空间实现
         * 需要手动清理。 */
        usleep(1000);
        clear_known_halts();
        drain_rsp();
        sequence_ = 0;
        logger_->debug("[USB] sysplf_reset: 端点已清理，序列号已重置");
    }

    std::string UsbTransport::read_chip_id()
    {
        if (!dev_handle_) {
            throw UsbException("设备未打开，无法读取芯片 ID");
        }

        uint32_t regval;
        uint8_t chip_id[24];

        /* 读取 4 个寄存器获取完整的芯片 ID */
        int ret = read_reg(ADR_CHIP_ID_3, &regval);
        if (ret < 0)
            throw UsbException("读取 ADR_CHIP_ID_3 失败");
        chip_id[0] = (regval >> 24) & 0xFF;
        chip_id[1] = (regval >> 16) & 0xFF;
        chip_id[2] = (regval >> 8) & 0xFF;
        chip_id[3] = regval & 0xFF;

        ret = read_reg(ADR_CHIP_ID_2, &regval);
        if (ret < 0)
            throw UsbException("读取 ADR_CHIP_ID_2 失败");
        chip_id[4] = (regval >> 24) & 0xFF;
        chip_id[5] = (regval >> 16) & 0xFF;
        chip_id[6] = (regval >> 8) & 0xFF;
        chip_id[7] = regval & 0xFF;

        ret = read_reg(ADR_CHIP_ID_1, &regval);
        if (ret < 0)
            throw UsbException("读取 ADR_CHIP_ID_1 失败");
        chip_id[8]  = (regval >> 24) & 0xFF;
        chip_id[9]  = (regval >> 16) & 0xFF;
        chip_id[10] = (regval >> 8) & 0xFF;
        chip_id[11] = regval & 0xFF;

        ret = read_reg(ADR_CHIP_ID_0, &regval);
        if (ret < 0)
            throw UsbException("读取 ADR_CHIP_ID_0 失败");
        chip_id[12] = (regval >> 24) & 0xFF;
        chip_id[13] = (regval >> 16) & 0xFF;
        chip_id[14] = (regval >> 8) & 0xFF;
        chip_id[15] = regval & 0xFF;
        chip_id[16] = 0;

        /* 跳过前导零字节 */
        uint8_t *c = chip_id;
        int i      = 0;
        while (*c == 0 && i < 16) {
            c++;
            i++;
        }

        if (*c != 0) {
            std::strncpy(reinterpret_cast<char *>(chip_id_str_),
                         reinterpret_cast<const char *>(c), sizeof(chip_id_str_) - 1);
            chip_id_str_[sizeof(chip_id_str_) - 1] = 0;
            logger_->info("[USB] 芯片 ID: {}",
                          reinterpret_cast<const char *>(chip_id_str_));
            return std::string(reinterpret_cast<const char *>(chip_id_str_));
        }

        throw UsbException("读取芯片 ID 失败（返回全零）");
    }

    /* ==================== 寄存器访问模式切换 ==================== */

    void UsbTransport::switch_to_ep0_mode()
    {
        if (!dev_handle_) {
            throw UsbException("设备未打开，无法切换到 EP0 模式");
        }

        logger_->info("[USB] 切换寄存器访问模式: CMD/RSP → EP0 控制传输");

        /* 设置函数指针为 EP0 模式实现 */
        read_reg_fn_  = &UsbTransport::read_reg_ep0;
        write_reg_fn_ = &UsbTransport::write_reg_ep0;

        logger_->info("[USB] 已切换到 EP0 控制传输模式（高效模式）");
    }

    void UsbTransport::switch_to_bulk_mode()
    {
        if (!dev_handle_) {
            throw UsbException("设备未打开，无法切换回 bulk 模式");
        }

        logger_->info("[USB] 切换寄存器访问模式: EP0 → CMD/RSP bulk 传输");

        /* 清除函数指针，恢复默认的 CMD/RSP 模式 */
        read_reg_fn_  = nullptr;
        write_reg_fn_ = nullptr;

        logger_->info("[USB] 已切换回 CMD/RSP bulk 传输模式（默认模式）");
    }

    bool UsbTransport::probe_ep0(uint32_t test_addr)
    {
        if (!dev_handle_)
            return false;

        /* 使用短超时进行探测（500ms 而不是 3000ms）*/
        constexpr int PROBE_TIMEOUT = 500;
        constexpr int PROBE_RETRIES = 2; // 只尝试 2 次

        Ssv6xxxReadRegResult result;
        uint16_t wValue = static_cast<uint16_t>(test_addr & 0xFFFF);
        uint16_t wIndex = static_cast<uint16_t>((test_addr >> 16) & 0xFFFF);

        for (int i = 0; i < PROBE_RETRIES; i++) {
            int ret = libusb_control_transfer(
                dev_handle_,
                LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR |
                    LIBUSB_RECIPIENT_DEVICE,
                VENDOR_REG_RW, wValue, wIndex,
                reinterpret_cast<unsigned char *>(&result),
                static_cast<uint16_t>(sizeof(result)), PROBE_TIMEOUT);

            if (ret >= 0) {
                return true;
            }
        }

        return false;
    }

    int UsbTransport::send_host_cmd(uint8_t h_cmd, uint32_t sub_h_cmd,
                                    const void *payload, uint32_t payload_len,
                                    bool wait_response)
    {
        constexpr int HCI_CMD_TIMEOUT = 3000;
        constexpr int HCI_HDR_LEN     = 16;
        constexpr int HCI_EVT_HDR_LEN = 12;
        constexpr int MAX_EVT_WAIT    = 100;

        if (!dev_handle_) {
            throw UsbException("设备未打开，无法发送 HCI host command");
        }

        /* Kernel HCI_SEND_CMD passes cfg_host_cmd directly to IF_SEND.  A host
         * command is already an HCI packet (c_type=HOST_CMD); unlike an 802.11 TX
         * frame it must not be prefixed with an 80-byte hardware TX descriptor. */
        const int total_len = HCI_HDR_LEN + static_cast<int>(payload_len);
        auto *buf           = new (std::nothrow) uint8_t[total_len];
        if (!buf) {
            return -ENOMEM;
        }
        std::memset(buf, 0, total_len);

        auto *hdr                = reinterpret_cast<SsvHostCmdHdr *>(buf);
        hdr->len                 = HCI_HDR_LEN + payload_len;
        hdr->c_type              = HOST_CMD_CTYPE;
        hdr->rsvd0               = 0;
        hdr->h_cmd               = h_cmd;
        hdr->sub_h_cmd           = sub_h_cmd;
        uint8_t command_sequence = 0;
        if (wait_response) {
            command_sequence = host_cmd_sequence_;
            if (++host_cmd_sequence_ == 128)
                host_cmd_sequence_ = 1;
        }

        /* r3408 HCI 层只把唯一序列写入 blocking_seq_no 的高字节。 */
        hdr->cmd_seq_no           = 0;
        hdr->blocking_seq_no      = wait_response
                                        ? ((static_cast<uint32_t>(command_sequence) << 24) |
                                      (static_cast<uint32_t>(h_cmd) << 16) |
                                      (sub_h_cmd & 0xFFFFu))
                                        : 0;
        const uint32_t target_seq = hdr->blocking_seq_no;

        if (payload && payload_len > 0) {
            std::memcpy(buf + HCI_HDR_LEN, payload, payload_len);
        }

        logger_->info(
            "[USB] 发送 HCI host cmd: h_cmd={} sub={} len={} blocking=0x{:08X}",
            h_cmd, sub_h_cmd, total_len, target_seq);

        int transferred = 0;
        int ret         = libusb_bulk_transfer(dev_handle_, ep_.tx_ep, buf, total_len,
                                               &transferred, HCI_CMD_TIMEOUT);
        delete[] buf;
        if (ret < 0) {
            logger_->warn("[USB] HCI host cmd 发送失败: {}", libusb_error_name(ret));
            return ret;
        }
        if (transferred != total_len) {
            logger_->warn("[USB] HCI host cmd 短写: {}/{} bytes", transferred,
                          total_len);
            return -EIO;
        }

        if (!wait_response) {
            return 0;
        }

        uint8_t evt_buf[USB_RX_BUF_SIZE];
        std::vector<uint8_t> pending;
        pending.reserve(sizeof(evt_buf) * 2);
        for (int i = 0; i < MAX_EVT_WAIT; i++) {
            int evt_len = 0;
            int r       = libusb_bulk_transfer(dev_handle_, ep_.rx_ep, evt_buf,
                                               sizeof(evt_buf), &evt_len, 50);
            if (r == LIBUSB_ERROR_TIMEOUT) {
                continue;
            }
            if (r < 0) {
                logger_->warn("[USB] HCI host cmd 等待响应失败: {}",
                              libusb_error_name(r));
                return r;
            }
            if (evt_len <= 0) {
                continue;
            }

            /* EP4 同时承载 802.11 RX、host event，并可能在前面带 12 字节
             * hci_rx_aggr_info。不能把一次 USB bulk 返回的起始地址直接当作
             * cfg_host_event；在连续数据中寻找具有唯一 blocking_seq_no 的
             * 12 字节事件头，可同时覆盖直传、聚合和 USB 分包三种情况。 */
            pending.insert(pending.end(), evt_buf, evt_buf + evt_len);
            for (size_t offset = 0; offset + HCI_EVT_HDR_LEN <= pending.size();
                 ++offset) {
                const uint32_t word0      = read_le_u32(pending.data() + offset);
                const uint16_t packet_len = static_cast<uint16_t>(word0 & 0xFFFFu);
                const uint8_t c_type      = static_cast<uint8_t>((word0 >> 16) & 0x7u);
                const uint8_t h_event     = static_cast<uint8_t>((word0 >> 24) & 0xFFu);
                if (c_type != HOST_EVENT_CTYPE) {
                    continue;
                }

                const uint32_t event_sequence =
                    read_le_u32(pending.data() + offset + 8);
                if (event_sequence == target_seq) {
                    logger_->info(
                        "[USB] 收到匹配 HCI host event: packet_len={} h_event={} "
                        "seq=0x{:08X}",
                        packet_len, h_event, event_sequence);
                    return 0;
                }
            }

            if (pending.size() > static_cast<size_t>(MAX_RX_PACKET_SIZE * 2))
                pending.erase(pending.begin(),
                              pending.end() - static_cast<ptrdiff_t>(MAX_RX_PACKET_SIZE));
        }

        logger_->warn(
            "[USB] HCI host cmd 未收到匹配响应: h_cmd={} sub={} blocking=0x{:08X}",
            h_cmd, sub_h_cmd, target_seq);
        return -1;
    }

} // namespace ssv6xxx
