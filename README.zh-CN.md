# SSV6256P 用户空间 WiFi 驱动（libusb）

[English](README.md)

这是一个面向 SSV6256P / SSV6X5X USB WiFi 芯片的 C++14 用户空间驱动，支持监控模式接收和原始 802.11 帧注入；不提供 STA、AP 或 P2P 网络功能。

## 环境要求

- CMake 3.10 或更新版本，以及支持 C++14 的编译器
- libusb 1.0 和 spdlog
- 构建命令行工具需要 CLI11（设置 `SSV6XXX_BUILD_CLI=OFF` 时不需要）
- 当前用户必须有权访问 USB 网卡。下面的 Linux 示例使用 `sudo`。

Debian 或 Ubuntu 可安装：

```bash
sudo apt install cmake g++ pkg-config libusb-1.0-0-dev libspdlog-dev libcli11-dev
```

macOS 使用 Homebrew 时：

```bash
brew install cmake libusb spdlog cli11
```

默认固件已通过仓库内的内核驱动固件头文件编译进驱动。默认情况下不需要创建符号链接或另行提供固件文件。若要加载外部固件，请用 `-f` 指定路径，并使用名称不以 `ssv6x5x-sw.bin` 结尾的文件；路径以该默认文件名结尾时会选择内嵌固件。

```bash
sudo ./build/sv6256p_monitor -c 6 -f /path/to/custom_firmware.bin
```

## 构建

在本仓库根目录运行：

```bash
cmake -S . -B build
cmake --build build -j4
```

这会构建 `libssv6xxx`、`sv6256p_monitor` 和 `ssv_dual_link_test`。如果只需要可复用的库，可关闭 CLI 构建目标：

```bash
cmake -S . -B build-lib -DSSV6XXX_BUILD_CLI=OFF
cmake --build build-lib -j4
```

## 快速开始：监控与抓包

```bash
# 查看可用选项
./build/sv6256p_monitor --help

# 列出匹配的网卡及其物理 USB 端口路径
sudo ./build/sv6256p_monitor --list-devices

# 监控信道 6 并打印数据帧摘要
sudo ./build/sv6256p_monitor -c 6

# 将接收帧（含 radiotap 元数据）写入 pcap 文件
sudo ./build/sv6256p_monitor -c 6 -b ht20 -o capture.pcap -v
```

按 Ctrl+C 可正常停止监控。使用 `tcpdump -nn -r capture.pcap -c 10` 查看抓包内容。网卡会停留在选定的信道；该工具不会自动跳频。

如果连接了多张匹配的网卡，程序会要求显式指定设备，不会随意选择其中一张。建议使用 `--usb-port`：物理端口路径在重新插拔后通常保持稳定。USB 总线号和设备地址适合临时调试，但重新连接后可能变化。同一张网卡同一时刻只能由一个进程或驱动对象占用。

请在不同终端分别运行，并为每张网卡设置不同的 MAC 地址。

```bash
sudo ./build/sv6256p_monitor --usb-port 1.4 -c 6 -m 02:11:22:33:44:54
sudo ./build/sv6256p_monitor --usb-port 1.2 -c 6 -m 02:11:22:33:44:52
```

## CLI 选项

| 选项 | 说明 | 默认值 |
|---|---|---|
| `-f, --firmware <路径>` | 固件文件路径；外部固件请使用自定义文件名 | 内嵌固件 |
| `-c, --channel <信道>` | 信道编号，1–165 | `149` |
| `-b, --bandwidth <模式>` | `ht20`、`ht40+` 或 `ht40-` | `ht20` |
| `--rf-channel <信道>` | 为 HT40 覆盖 RF 合成器信道 | 自动 |
| `-o, --output <文件>` | 将接收帧写入 pcap 文件 | 只打印摘要 |
| `-t, --timeout <毫秒>` | RX 轮询超时 | `100` 毫秒 |
| `-i, --inject <文件>` | 从十六进制文本文件注入原始 802.11 帧 | 关闭 |
| `--tx-rate-index <n>` | TX 速率索引：`0` 使用安全默认值；`1–46` 或 `128–255` 指定固定编码速率 | `0` |
| `--tx-power-index <n>` | TX 功率索引，`0–127`；`-1` 保留工厂校准值（不是 dBm） | `-1` |
| `-m, --mac <mac>` | 网卡 MAC 地址 | `00:11:22:33:44:55` |
| `--list-devices` | 列出匹配的网卡后退出 | 关闭 |
| `--usb-port <路径>` | 按物理 USB 端口路径选择，例如 `1.4` | 仅发现一张网卡时自动选择 |
| `--usb-bus <n>` | 按 USB 总线号选择 | 任意 |
| `--usb-address <n>` | 按当前 USB 设备地址选择 | 任意 |
| `--bus-clock <模式>` | 板级总线时钟：`auto`、`40` 或 `80` MHz | `auto` |
| `--regulator <模式>` | 板级稳压器：`auto`、`ldo` 或 `dcdc` | `auto` |
| `-v, --verbose` | 输出详细日志 | 关闭 |
| `--dump-regs` | 寄存器转储占位选项；目前尚未实现转储 | 关闭 |

`auto` 会保留或检测板卡当前的时钟和稳压器设置。只有确认板卡使用 DCDC 时才应强制指定 `--regulator dcdc`；在 LDO 板卡上强制选择 DCDC 可能导致 USB 端点无响应，直到断电重启板卡。

## 注入数据帧

CLI 按行读取帧。每行用空格分隔的十六进制字节表示一个 802.11 帧；空行和以 `#` 开头的行会被忽略。注入操作在驱动初始化后执行，完成后程序退出，不会启动监控模式。

```text
# 完整的 Probe Request 帧
40 00 00 00 ff ff ff ff ff ff 02 11 22 33 44 55 ff ff ff ff ff ff 00 00 00 00 01 08 82 84 8b 96 0c 12 18 24 32 04 30 48 60 6c
```

```bash
sudo ./build/sv6256p_monitor -c 6 -i frames.hex --tx-rate-index 0
```

应用程序也可以调用 `SSV6xxxDriver::inject_frame()` 发送帧；下面示例中的 `rate_index = 0` 表示使用安全默认速率选择。

```cpp
#include <cstdint>
#include <ssv6xxx/ssv6xxx.hpp>

uint8_t frame[] = {
    0x40, 0x00, 0x00, 0x00, // Probe Request 帧控制字段和持续时间
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 广播目的地址
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, // 源地址
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // 广播 BSSID
    0x00, 0x00, // 序列控制字段
    0x00, 0x00, // 空 SSID 信息元素
    0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, // 支持速率
    0x32, 0x04, 0x30, 0x48, 0x60, 0x6c // 扩展支持速率
};

ssv6xxx::SSV6xxxDriver adapter;
ssv6xxx::DriverConfig config;
config.default_channel = 6;
adapter.init(config);
adapter.inject_frame(frame, sizeof(frame), 0);
```

## 库的使用方法

一次 `init()` 成功后，一个 `SSV6xxxDriver` 对象会独占一张物理 USB 网卡，直到调用 `shutdown()` 或对象析构。RX 回调在调用 `poll()` 的线程中运行。`RxFrameView` 仅在回调执行期间有效；如需之后继续使用帧数据，请先复制。

```cpp
#include <ssv6xxx/ssv6xxx.hpp>

#include <atomic>
#include <cstdint>
#include <cstdio>

int main() {
    ssv6xxx::SSV6xxxDriver adapter;
    ssv6xxx::DriverConfig config;
    config.default_channel = 6;
    config.bandwidth = ssv6xxx::ChannelBandwidth::HT20;

    std::atomic<uint64_t> packet_count{0};
    adapter.init(config);
    adapter.set_rx_callback(
        [&packet_count](const ssv6xxx::RxFrameView& frame, void*) {
            ++packet_count;
            std::printf("RX %d bytes, %u MHz, RSSI %d dBm, rate %u Mbps\n",
                        frame.length, frame.metadata.frequency_mhz,
                        frame.metadata.rssi_dbm, frame.metadata.rate_mbps);
        });

    adapter.open_pcap("capture.pcap"); // 可选
    adapter.start_monitor();
    while (packet_count.load() < 100)
        adapter.poll(100); // 驱动 libusb 事件并调用回调

    adapter.stop_monitor();
    adapter.close_pcap();
    adapter.shutdown(); // 可选；析构函数也会清理资源
}
```

基本生命周期为：`init()` → 设置回调 → 可选地打开 pcap → `start_monitor()` → 重复调用 `poll(timeout_ms)` → `stop_monitor()` → 调用 `shutdown()` 或由析构函数清理。仅发送数据时，在 `init()` 后调用 `inject_frame()` 即可，无需启动监控模式。

### 集成到其他 CMake 项目

将本仓库作为子目录加入工程，并链接导出的 target：

```cmake
set(SSV6XXX_BUILD_CLI OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libusb-ssv6x5x)

add_executable(my_capture_app main.cpp)
target_link_libraries(my_capture_app PRIVATE ssv6xxx::ssv6xxx)
```

应用程序包含 `<ssv6xxx/ssv6xxx.hpp>` 即可。关闭 CLI 构建目标后不需要 CLI11，但仍需要 libusb 和 spdlog。

### 信道环境测量

网卡完成初始化并在当前信道观测一段时间后，可以读取噪声底和 EDCCA/FCS 计数器：

```cpp
adapter.reset_channel_survey();
// 让网卡按需观测当前信道一段时间。
const ssv6xxx::ChannelSurvey survey = adapter.read_channel_survey();

if (survey.noise_floor_valid)
    std::printf("noise floor: %d dBm (raw=%u)\n",
                survey.noise_floor_dbm, survey.raw_noise_power);
if (survey.edcca_valid)
    std::printf("channel busy: %llu / %llu us\n",
                static_cast<unsigned long long>(survey.busy_us),
                static_cast<unsigned long long>(survey.period_us));
```

使用读数前请检查对应的 `*_valid` 字段。噪声底来自实时 PHY 寄存器；PHY 尚未产生测量值时，该读数无效。`raw_noise_power` 可用于查看硬件原始码值和校准情况。

## 手动抓包检查

目前没有自动化硬件测试套件。连接网卡后，可用以下命令进行基本构建和抓包检查：

```bash
cmake -S . -B build-check
cmake --build build-check -j4
./build-check/sv6256p_monitor --help
sudo ./build-check/sv6256p_monitor -c 6 -b ht20 -o capture.pcap -v
# 收到数据帧后按 Ctrl+C 停止，再检查抓包：
tcpdump -nn -r capture.pcap -c 10
```

正常抓包时，初始化不应出现 USB 或固件错误，应能看到接收帧，并且 `tcpdump` 能解析 pcap 中的数据包。同一张网卡同一时刻只能由一个进程或驱动对象占用。

## 驱动行为与 USB 细节

初始化流程参考内核驱动的 `tu_ssv6xxx_init_mac`：

1. 枚举 USB 网卡并读取芯片 ID。
2. 复位 MAC，并配置 HCI、MMU、TX/RX 阈值、TSF、地址、过滤器和 RX 路由。
3. 下载固件并确认固件版本正在运行。
4. 启用监控模式并设置所选信道。

监控模式下，USB RX 数据流包含 80 字节的 `ssv6006_rx_desc`，其后是 802.11 帧和填充数据。驱动会在调用回调或写入 pcap 前剥离硬件描述符。

注入帧时，驱动会在原始 802.11 帧前添加 80 字节的 `ssv6200_tx_desc`，再通过 USB bulk TX 端点发送。

| 端点 | 方向 | 类型 | 用途 |
|---|---|---|---|
| EP1（`0x01`） | OUT | Bulk | 命令 |
| EP2（`0x82`） | IN | Bulk | 响应 |
| EP3（`0x03`） | OUT | Bulk | 注入帧 |
| EP4（`0x84`） | IN | Bulk | 接收帧 |

寄存器读写通过 EP1/EP2 上的命令与响应交互完成。固件通过 USB vendor control 请求下载（`bRequest = 0xF0`），每块最多 512 字节。

### USB RX 加速器

监控 RX 会通过 EP1/EP2 寄存器协议设置寄存器 `0x700041AC` 的 bit 3，以启用 EP4 USB RX 加速器。正常退出时，驱动会先停止 RX 传输，再清除该加速器位，最后关闭芯片。按此顺序清理后，后续进程通常可以直接使用网卡，无需重新插拔。

不要用 vendor 请求 `0xF0` 写入 `0x700041AC`；该请求专用于下载固件。监控 RX 不应启用 `0x700041AC` 的全部位；RX 传输和加速器仍在运行时也不要关闭芯片。

## 源码文件

| 路径 | 说明 |
|---|---|
| `include/ssv6xxx/ssv6xxx.hpp` | 库的公共入口 |
| `src/ssv6xxx_driver.hpp/.cpp` | 单网卡驱动 API |
| `src/ssv6xxx_usb_transport.hpp/.cpp` | libusb 传输和设备选择 |
| `src/ssv6xxx_chip_init.hpp/.cpp` | 芯片与固件初始化 |
| `src/ssv6xxx_monitor_session.hpp/.cpp` | 抓包、注入和 pcap 输出 |
| `src/ssv6xxx_types.hpp` | 公共配置与帧类型 |
| `src/ssv6xxx_regs.hpp` | 寄存器映射与位域定义 |
| `src/ssv6xxx_descriptors.hpp` | 硬件描述符和 802.11 辅助定义 |
| `kernel-driver/L.SMAC.19Q3.2126.02_r3408/include/ssv6x5x-sw.h` | 编译进驱动的固件数组 |
| `src/main.cpp` | `sv6256p_monitor` 命令行程序 |
| `CMakeLists.txt` | 库和可执行程序的构建目标 |

## 许可证

[`LICENSE`](LICENSE) 文件包含相关版权持有人授权适用部分的 GNU Affero 通用公共许可证第 3 版（AGPL-3.0）。该许可证不取代各文件原有的许可证：用户空间驱动包含 `SPDX-License-Identifier: GPL-2.0-only` 声明，随附的内核驱动也包含第三方版权和许可声明，必须予以保留。特别是，未经相关版权持有人许可，不能将基于内核驱动的代码作为仅适用 AGPL-3.0 的作品重新分发。重新分发前请查看每个文件中的声明。
