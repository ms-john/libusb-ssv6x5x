/*
 * SSV6256P 用户空间 WiFi 监控/注入演示程序 (C++14)
 *
 * 完整的初始化和使用演示
 * 使用 libusb 进行 USB 传输。
 *
 * 用法：
 *   sudo ./sv6256p_monitor [选项]
 *
 * 选项：
 *   -f <固件路径>   固件路径（默认：firmware/ssv6x5x-sw.bin）
 *   -c <信道>       信道（2.4GHz/5GHz，默认：149）
 *   -b <带宽>       带宽：ht20, ht40+, ht40-（默认：ht20）
 *   --rf-channel N  覆盖 RF 合成器信道用于 HT40 测试
 *   -o <文件>       输出 pcap 文件
 *   -t <毫秒>       RX 超时（毫秒）（默认：100）
 *   -i <帧>         从文件注入原始 802.11 帧（十六进制转储）
 *   -m <mac>        MAC 地址（默认：00:11:22:33:44:55）
 *   -v              详细输出
 *   --dump-regs     初始化后转储关键寄存器
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ssv6xxx_descriptors.hpp"
#include "ssv6xxx_regs.hpp"
#include "ssv6xxx_types.hpp"
#include "ssv6xxx_usb_transport.hpp"
#include <ssv6xxx/ssv6xxx.hpp>

#include <CLI/CLI.hpp>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <spdlog/spdlog.h>
#include <string>
#ifdef _WIN32
#include <windows.h>
static void usleep(unsigned int us)
{
    ::Sleep((us + 999) / 1000);
}
#else
#include <unistd.h>
#endif

/* ==================== 全局变量 ==================== */
static std::shared_ptr<spdlog::logger> g_logger;
static volatile sig_atomic_t g_running = 1;

/**
 * @brief 信号处理函数（异步信号安全版本）
 *
 * 注意：此函数必须是异步信号安全的！
 * 只能调用异步信号安全的函数：
 * - _Exit()
 * - signal()
 * - write() (系统调用)
 * - 原子操作
 *
 * 禁止调用：
 * - printf/fprintf/spdlog
 * - malloc/free
 * - 任何非重入函数
 */
static void sigint_handler(int sig)
{
    (void)sig;
    g_running = 0;

#ifdef _WIN32
    /* Windows 下仅设置标志，避免在信号上下文中做复杂 IO。 */
#else
    /* 使用 write() 输出消息（异步信号安全） */
    const char msg[] = "\n[捕获到 SIGINT，正在关闭...]\n";
    write(STDERR_FILENO, msg, sizeof(msg) - 1);
#endif
}

/**
 * @brief 从文件读取十六进制数据并注入帧
 *
 * @param driver 驱动实例引用
 * @param filepath 十六进制文件路径
 * @param rate_idx 固定发送速率索引；0 使用安全默认速率
 * @return 成功注入的帧数，失败返回负值
 */
static int inject_from_file(ssv6xxx::SSV6xxxDriver &driver,
                            const std::string &filepath, int rate_idx)
{
    constexpr useconds_t INJECT_FRAME_GAP_US = 5000;
    constexpr useconds_t INJECT_DRAIN_US     = 200000;

    FILE *fp = fopen(filepath.c_str(), "r");
    if (!fp) {
        fprintf(stderr, "无法打开注入文件: %s (%s)\n", filepath.c_str(),
                strerror(errno));
        return -errno;
    }

    char line[8192];
    int count = 0;

    while (fgets(line, sizeof(line), fp)) {
        /* 跳过空行和注释 */
        if (line[0] == '\n' || line[0] == '#')
            continue;

        /* 解析十六进制字符串 */
        uint8_t frame[MAX_FRAME_SIZE];
        int len       = 0;
        const char *p = line;

        while (*p && len < MAX_FRAME_SIZE) {
            while (*p == ' ' || *p == '\t' || *p == '\r')
                p++;
            if (!*p || *p == '\n')
                break;

            unsigned byte;
            if (sscanf(p, "%02x", &byte) != 1)
                break;
            frame[len++] = static_cast<uint8_t>(byte);
            p += 2;
        }

        if (len <= 0)
            continue;

        try {
            driver.inject_frame(frame, len, rate_idx);
            printf("[INJECT] 已发送 %d bytes, rate_idx=%d\n", len, rate_idx);
            count++;

            /* bulk OUT 完成只表示数据已交给芯片，不代表帧已经离开射频。
             * 给固件 TX 队列留出调度时间，避免批量注入时瞬间塞满队列。 */
            usleep(INJECT_FRAME_GAP_US);
        } catch (const std::exception &e) {
            fprintf(stderr, "[INJECT] 失败: %s\n", e.what());
        }
    }

    fclose(fp);

    if (count > 0) {
        printf("[INJECT] 等待硬件 TX 队列发送完成...\n");
        fflush(stdout);
        usleep(INJECT_DRAIN_US);
    }

    return count;
}

/* ==================== 主函数 ==================== */
int main(int argc, char *argv[])
{
    /*
     * 用户空间驱动初始化流程（C++14 新 API）
     *
     * 简化的用户空间流程：
     *   1. 解析命令行参数 (CLI11)
     *   2. 创建驱动实例 + logger
     *   3. 调用 driver.init(config) → 自动执行 7 阶段硬件初始化
     *   4. 可选：设置回调、打开 pcap、启动监控
     *   5. 主循环：poll() 或 rx_loop()
     *   6. 析构自动清理所有资源
     */

    std::string fw_path = "firmware/ssv6x5x-sw.bin";
    std::string pcap_path;
    std::string inject_path;
    std::string mac_addr  = "00:11:22:33:44:55";
    std::string bandwidth = "ht20";
    std::string usb_port;
    std::string bus_clock = "auto";
    std::string regulator = "auto";
    int channel           = 149; // 默认使用 5GHz 信道
    int tx_rate_idx       = 0;
    int tx_power_idx      = -1;
    int rf_channel        = 0;
    int rx_timeout        = 100;
    int usb_bus           = -1;
    int usb_address       = -1;
    int list_devices      = 0;
    int verbose           = 0;
    int dump_regs         = 0;
    int ret               = 0;

    CLI::App app{"SSV6256P 用户空间 WiFi 监控/注入演示程序"};

    app.add_option("-f,--firmware", fw_path, "固件路径")->capture_default_str();
    app.add_option("-c,--channel", channel, "信道（2.4GHz/5GHz）")
        ->capture_default_str()
        ->check(CLI::Range(1, 165));
    app.add_option("-b,--bandwidth", bandwidth, "带宽：ht20, ht40+, ht40-")
        ->capture_default_str()
        ->check([](const std::string &str) {
            if (str == "ht20" || str == "ht40+" || str == "ht40-")
                return std::string();
            return std::string("带宽必须是 ht20, ht40+, 或 ht40-");
        });
    app.add_option("--rf-channel", rf_channel,
                   "覆盖 RF 合成器信道用于 HT40 测试");
    app.add_option("-o,--output", pcap_path, "输出 pcap 文件");
    app.add_option("-t,--timeout", rx_timeout, "RX 超时（毫秒）")
        ->capture_default_str()
        ->check(CLI::PositiveNumber);
    app.add_option("-i,--inject", inject_path,
                   "从文件注入原始 802.11 帧（十六进制转储）");
    app.add_option("--tx-rate-index", tx_rate_idx,
                   "固定 TX 速率：0=安全默认；内核索引 0-46（HT20 MCS 15-30，HT40 MCS 31-46）；或 128-255 原始编码")
        ->capture_default_str()
        ->check(CLI::Range(0, 255));
    app.add_option("--tx-power-index", tx_power_idx,
                   "TX 功率索引：0-127；-1 保留工厂校准默认值（不是 dBm）")
        ->capture_default_str()
        ->check(CLI::Range(-1, 127));
    app.add_option("-m,--mac", mac_addr, "MAC 地址")->capture_default_str();
    app.add_flag("--list-devices", list_devices, "列出 SSV USB 网卡及其位置后退出");
    app.add_option("--usb-port", usb_port,
                   "按物理 USB 端口路径选择（例如 1.4，推荐）");
    app.add_option("--usb-bus", usb_bus, "按 USB 总线号选择")
        ->check(CLI::Range(0, 255));
    app.add_option("--usb-address", usb_address, "按 USB 设备地址选择")
        ->check(CLI::Range(1, 255));
    app.add_option("--bus-clock", bus_clock, "板级总线时钟：auto, 40, 80")
        ->capture_default_str()
        ->check([](const std::string &str) {
            if (str == "auto" || str == "40" || str == "80")
                return std::string();
            return std::string("总线时钟必须是 auto, 40 或 80");
        });
    app.add_option("--regulator", regulator, "板级稳压器：auto, ldo, dcdc")
        ->capture_default_str()
        ->check([](const std::string &str) {
            if (str == "auto" || str == "ldo" || str == "dcdc")
                return std::string();
            return std::string("稳压器必须是 auto, ldo 或 dcdc");
        });
    app.add_flag("-v,--verbose", verbose, "详细输出");
    app.add_flag("--dump-regs", dump_regs, "初始化后转储关键寄存器");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError &e) {
        return app.exit(e);
    }

    /* 设置日志格式和级别 */
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l$%$] %v");
    if (verbose) {
        spdlog::set_level(spdlog::level::debug);
    } else {
        spdlog::set_level(spdlog::level::info);
    }

    /* 创建 logger */
    g_logger = spdlog::default_logger();
    g_logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    if (list_devices) {
        try {
            const auto devices = ssv6xxx::UsbTransport::list_devices();
            if (devices.empty()) {
                printf("未发现 SSV6x5x USB 网卡。\n");
                return 1;
            }

            printf("发现 %zu 张 SSV6x5x USB 网卡：\n", devices.size());
            for (size_t i = 0; i < devices.size(); ++i) {
                const auto &device = devices[i];
                printf("  [%zu] bus=%u address=%u port=%s", i,
                       static_cast<unsigned>(device.bus),
                       static_cast<unsigned>(device.address),
                       device.port_path.empty() ? "?" : device.port_path.c_str());
                if (!device.product.empty())
                    printf(" product=\"%s\"", device.product.c_str());
                if (!device.serial.empty())
                    printf(" serial=\"%s\"", device.serial.c_str());
                printf("\n");
            }
            printf("推荐使用 --usb-port <port> 选择，物理端口在重新插拔后更稳定。\n");
            return 0;
        } catch (const std::exception &e) {
            fprintf(stderr, "枚举 USB 网卡失败: %s\n", e.what());
            return 1;
        }
    }

    /* 注册信号处理 */
#ifdef _WIN32
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);
#else
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = sigint_handler;
        /* 不设置 SA_RESTART，让系统调用被中断时返回 EINTR */
        sa.sa_flags = 0;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
    }
#endif

    try {
        /* ============================================================
         * 步骤 1: 创建驱动实例
         * ============================================================ */
        g_logger->info("========================================");
        g_logger->info("  SSV6256P 用户空间驱动 v2.0 (C++14)");
        g_logger->info("========================================");

        ssv6xxx::SSV6xxxDriver driver(g_logger);

        /* ============================================================
         * 步骤 2: 配置并初始化驱动
         * ============================================================ */
        ssv6xxx::DriverConfig config;

        config.firmware_path        = fw_path;
        config.default_channel      = static_cast<uint8_t>(channel);
        config.rf_channel           = static_cast<uint8_t>(rf_channel);
        config.tx_power_index       = tx_power_idx;
        config.verbose              = verbose != 0;
        config.usb_device.bus       = usb_bus;
        config.usb_device.address   = usb_address;
        config.usb_device.port_path = usb_port;

        if (bus_clock == "40") {
            config.bus_clock = ssv6xxx::BusClock::MHZ_40;
        } else if (bus_clock == "80") {
            config.bus_clock = ssv6xxx::BusClock::MHZ_80;
        }

        if (regulator == "ldo") {
            config.voltage_regulator = ssv6xxx::VoltageRegulator::LDO;
        } else if (regulator == "dcdc") {
            config.voltage_regulator = ssv6xxx::VoltageRegulator::DCDC;
        }

        /* 解析带宽参数 */
        if (bandwidth == "ht20") {
            config.bandwidth = ssv6xxx::ChannelBandwidth::HT20;
        } else if (bandwidth == "ht40+") {
            config.bandwidth = ssv6xxx::ChannelBandwidth::HT40_PLUS;
        } else if (bandwidth == "ht40-") {
            config.bandwidth = ssv6xxx::ChannelBandwidth::HT40_MINUS;
        }

        /* 解析 MAC 地址 */
        if (mac_addr.length() == 17) { // XX:XX:XX:XX:XX:XX
            unsigned mac[6];
            if (sscanf(mac_addr.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x", &mac[0],
                       &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) == 6) {
                for (int i = 0; i < 6; i++) {
                    config.mac_addr[i] = static_cast<uint8_t>(mac[i]);
                }
            }
        }

        /* 执行完整初始化（7 阶段） */
        g_logger->info("开始初始化...");
        driver.init(config);
        g_logger->info("✅ 初始化完成！芯片 ID: {}", driver.get_chip_id());

        /* ============================================================
         * 步骤 3: 可选操作 - 转储寄存器
         * ============================================================ */
        if (dump_regs) {
            g_logger->info("\n--- 关键寄存器状态 ---");
            /* TODO: 添加寄存器转储功能 */
            g_logger->info("(寄存器转储功能待实现)");
        }

        /* ============================================================
         * 步骤 4: 可选操作 - 帧注入
         * ============================================================ */
        if (!inject_path.empty()) {
            g_logger->info("\n--- 帧注入 ---");
            int injected = inject_from_file(driver, inject_path, tx_rate_idx);
            if (injected > 0) {
                g_logger->info("成功注入 {} 帧", injected);
            }
            g_logger->info("注入完成，退出程序。");
            return 0; /* 仅注入模式，不进入监控循环 */
        }

        /* ============================================================
         * 步骤 5: 设置 RX 回调
         * ============================================================ */
        driver.set_rx_callback(
            [](const ssv6xxx::RxFrameView &frame, void *user_data) {
                (void)user_data;

                static uint64_t packet_count = 0;
                packet_count++;

                /* 每 100 个包打印一次摘要 */
                if (packet_count % 100 == 1 || frame.length > 500) {
                    printf("[RX #%lu] %d bytes @ %d MHz | RSSI=%d dBm | Rate=%d Mbps\n",
                           (unsigned long)packet_count,
                           frame.length,
                           frame.metadata.frequency_mhz,
                           frame.metadata.rssi_dbm,
                           frame.metadata.rate_mbps);
                    fflush(stdout);
                }
            },
            nullptr /* user_data */
        );

        /* ============================================================
         * 步骤 6: 打开 PCap 文件（可选）
         * ============================================================ */
        if (!pcap_path.empty()) {
            g_logger->info("打开 PCap 文件: {}", pcap_path);
            driver.open_pcap(pcap_path);
        }

        /* ============================================================
         * 步骤 7: 启动监控模式
         * ============================================================ */
        g_logger->info("========================================");
        g_logger->info("  启动监控模式 (信道 {}, {})", channel, bandwidth);
        g_logger->info("  按 Ctrl+C 停止");
        g_logger->info("========================================");

        driver.start_monitor();

        /* ============================================================
         * 步骤 8: 主接收循环
         * ============================================================ */
        g_logger->info("进入主循环...\n");

        uint64_t last_stats_time             = 0;
        constexpr uint64_t STATS_INTERVAL_MS = 10000; // 每 10 秒打印统计

        while (g_running && driver.is_monitoring()) {
            /* 轮询帧接收 */
            int result = driver.poll(rx_timeout);

            if (result < 0 && result != -ETIMEDOUT) {
                g_logger->error("poll 错误: {}，继续运行...", result);
                usleep(100000); // 错误后等待 100ms
                continue;
            }

            /* 定期打印统计信息 */
            auto now        = std::chrono::steady_clock::now().time_since_epoch();
            uint64_t now_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now).count());

            if (now_ms - last_stats_time >= STATS_INTERVAL_MS) {
                driver.print_stats();
                last_stats_time = now_ms;
            }
        }

        /* ============================================================
         * 步骤 9: 清理（RAII 自动处理）
         * ============================================================ */
        g_logger->info("\n正在停止...");

        driver.stop_monitor();
        driver.close_pcap();

        /* 打印最终统计 */
        g_logger->info("最终统计:");
        driver.print_stats();

        g_logger->info("========================================");
        g_logger->info("  ✅ 程序正常退出");
        g_logger->info("========================================");

    } catch (const ssv6xxx::UsbException &e) {
        g_logger->critical("USB 错误: {}", e.what());
        ret = -1;
    } catch (const ssv6xxx::InitException &e) {
        g_logger->critical("初始化错误: {}", e.what());
        ret = -2;
    } catch (const ssv6xxx::InvalidStateException &e) {
        g_logger->critical("状态错误: {}", e.what());
        ret = -3;
    } catch (const std::exception &e) {
        g_logger->critical("未知错误: {}", e.what());
        ret = -99;
    }

    /* RAII: driver 析构自动调用 shutdown/close/cleanup */
    return ret < 0 ? 1 : 0;
}
