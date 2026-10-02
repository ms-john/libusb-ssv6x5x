# SSV6256P Userspace WiFi Driver (libusb)

[简体中文](README.zh-CN.md)

A C++14 userspace driver for the SSV6256P / SSV6X5X USB WiFi chipset. It supports monitor-mode reception and raw 802.11 frame injection; it does not provide STA, AP, or P2P networking.

## Requirements

- CMake 3.10 or newer and a C++14 compiler
- libusb 1.0 and spdlog
- CLI11 to build the command-line tools (not needed when `SSV6XXX_BUILD_CLI=OFF`)
- Permission to access the USB adapter. The Linux examples below use `sudo`.

For example, on Debian or Ubuntu:

```bash
sudo apt install cmake g++ pkg-config libusb-1.0-0-dev libspdlog-dev libcli11-dev
```

On macOS with Homebrew:

```bash
brew install cmake libusb spdlog cli11
```

The default firmware is compiled into the driver from the bundled kernel-driver firmware header. You do not need to create a symlink or provide a separate firmware file for the default setup. To load an external image, pass its path with `-f` and use a filename other than `ssv6x5x-sw.bin`; paths ending in that default filename select the embedded image.

```bash
sudo ./build/sv6256p_monitor -c 6 -f /path/to/custom_firmware.bin
```

## Build

Run these commands from this repository's root directory:

```bash
cmake -S . -B build
cmake --build build -j4
```

This builds `libssv6xxx`, `sv6256p_monitor`, and `ssv_dual_link_test`. To build only the reusable library, disable the CLI targets:

```bash
cmake -S . -B build-lib -DSSV6XXX_BUILD_CLI=OFF
cmake --build build-lib -j4
```

## Quick start: monitor and capture

```bash
# Show available options
./build/sv6256p_monitor --help

# List matching adapters and their physical USB port paths
sudo ./build/sv6256p_monitor --list-devices

# Monitor channel 6 and print packet summaries
sudo ./build/sv6256p_monitor -c 6

# Save received frames, including radiotap metadata, to a pcap file
sudo ./build/sv6256p_monitor -c 6 -b ht20 -o capture.pcap -v
```

Press Ctrl+C to stop monitoring cleanly. Use `tcpdump -nn -r capture.pcap -c 10` to inspect a capture. The adapter stays on the selected channel; this tool does not perform channel hopping.

When more than one matching adapter is connected, the program requires an explicit selector instead of choosing an arbitrary device. Prefer `--usb-port`, whose physical port path remains stable after reconnecting. The USB bus and address selectors are useful for temporary debugging but can change after reconnecting. Only one process or driver object can own an adapter at a time.

Run each command in a separate terminal and assign each adapter a distinct MAC address.

```bash
sudo ./build/sv6256p_monitor --usb-port 1.4 -c 6 -m 02:11:22:33:44:54
sudo ./build/sv6256p_monitor --usb-port 1.2 -c 6 -m 02:11:22:33:44:52
```

## CLI options

| Option | Description | Default |
|---|---|---|
| `-f, --firmware <path>` | Firmware file path; use a custom filename for an external image | Built-in firmware |
| `-c, --channel <ch>` | Channel number, 1–165 | `149` |
| `-b, --bandwidth <mode>` | `ht20`, `ht40+`, or `ht40-` | `ht20` |
| `--rf-channel <ch>` | Override RF synthesizer channel for HT40 | Automatic |
| `-o, --output <file>` | Write received frames to a pcap file | Print summaries only |
| `-t, --timeout <ms>` | RX polling timeout | `100` ms |
| `-i, --inject <file>` | Inject raw 802.11 frames from a hex text file | Off |
| `--tx-rate-index <n>` | TX rate index: `0` uses the safe default; `1–46` or `128–255` select fixed encoded rates | `0` |
| `--tx-power-index <n>` | TX power index, `0–127`; `-1` keeps factory calibration (not a dBm value) | `-1` |
| `-m, --mac <mac>` | Adapter MAC address | `00:11:22:33:44:55` |
| `--list-devices` | List matching adapters and exit | Off |
| `--usb-port <path>` | Select by physical USB port path, such as `1.4` | Automatic if one adapter is found |
| `--usb-bus <n>` | Select by USB bus number | Any |
| `--usb-address <n>` | Select by current USB device address | Any |
| `--bus-clock <mode>` | Board bus clock: `auto`, `40`, or `80` MHz | `auto` |
| `--regulator <mode>` | Board regulator: `auto`, `ldo`, or `dcdc` | `auto` |
| `-v, --verbose` | Enable verbose logging | Off |
| `--dump-regs` | Register dump placeholder; the dump is not implemented yet | Off |

`auto` preserves or detects the board's current clock and regulator settings. Force `--regulator dcdc` only when the board is known to use DCDC; selecting DCDC on an LDO board can stop USB endpoints from responding until the board is power-cycled.

## Frame injection

The CLI reads one frame per line. Write each 802.11 frame as space-separated hexadecimal bytes; blank lines and lines beginning with `#` are ignored. Injection runs after initialization and exits without starting monitor mode.

```text
# Complete probe request frame
40 00 00 00 ff ff ff ff ff ff 02 11 22 33 44 55 ff ff ff ff ff ff 00 00 00 00 01 08 82 84 8b 96 0c 12 18 24 32 04 30 48 60 6c
```

```bash
sudo ./build/sv6256p_monitor -c 6 -i frames.hex --tx-rate-index 0
```

Applications can also send a frame through `SSV6xxxDriver::inject_frame()`; the example below uses the safe default rate selection (`rate_index = 0`).

```cpp
#include <cstdint>
#include <ssv6xxx/ssv6xxx.hpp>

uint8_t frame[] = {
    0x40, 0x00, 0x00, 0x00, // Probe Request frame control and duration
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // Broadcast destination
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, // Source
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, // Broadcast BSSID
    0x00, 0x00, // Sequence control
    0x00, 0x00, // Empty SSID information element
    0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24, // Supported rates
    0x32, 0x04, 0x30, 0x48, 0x60, 0x6c // Extended supported rates
};

ssv6xxx::SSV6xxxDriver adapter;
ssv6xxx::DriverConfig config;
config.default_channel = 6;
adapter.init(config);
adapter.inject_frame(frame, sizeof(frame), 0);
```

## Library usage

One `SSV6xxxDriver` object exclusively owns one physical USB adapter from a successful `init()` until `shutdown()` or destruction. The RX callback runs on the thread calling `poll()`. An `RxFrameView` is valid only for the duration of the callback; copy the frame if it needs to outlive the callback.

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

    adapter.open_pcap("capture.pcap"); // Optional
    adapter.start_monitor();
    while (packet_count.load() < 100)
        adapter.poll(100); // Drives libusb events and invokes the callback

    adapter.stop_monitor();
    adapter.close_pcap();
    adapter.shutdown(); // Optional; the destructor also cleans up
}
```

The basic lifecycle is `init()` → set callback → optionally open pcap → `start_monitor()` → repeatedly call `poll(timeout_ms)` → `stop_monitor()` → `shutdown()` or let the destructor clean up. For TX-only use, call `inject_frame()` after `init()`; monitor mode does not need to be started.

### CMake integration

When this repository is included as a subdirectory, link against the exported target:

```cmake
set(SSV6XXX_BUILD_CLI OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libusb-ssv6x5x)

add_executable(my_capture_app main.cpp)
target_link_libraries(my_capture_app PRIVATE ssv6xxx::ssv6xxx)
```

The application includes `<ssv6xxx/ssv6xxx.hpp>`. CLI11 is not needed when the CLI targets are disabled; libusb and spdlog are still required.

### Channel survey

After the adapter has initialized and observed the channel, the API can read its noise floor and EDCCA/FCS counters:

```cpp
adapter.reset_channel_survey();
// Let the adapter observe the channel for the desired interval.
const ssv6xxx::ChannelSurvey survey = adapter.read_channel_survey();

if (survey.noise_floor_valid)
    std::printf("noise floor: %d dBm (raw=%u)\n",
                survey.noise_floor_dbm, survey.raw_noise_power);
if (survey.edcca_valid)
    std::printf("channel busy: %llu / %llu us\n",
                static_cast<unsigned long long>(survey.busy_us),
                static_cast<unsigned long long>(survey.period_us));
```

Check each `*_valid` field before using its reading. The noise floor is a live PHY register value and remains invalid until the PHY has produced a measurement. `raw_noise_power` exposes the hardware code for calibration checks.

## Manual capture check

There is no automated hardware test suite. With the adapter connected, these commands provide a basic build and capture check:

```bash
cmake -S . -B build-check
cmake --build build-check -j4
./build-check/sv6256p_monitor --help
sudo ./build-check/sv6256p_monitor -c 6 -b ht20 -o capture.pcap -v
# Press Ctrl+C after packets arrive, then inspect the capture:
tcpdump -nn -r capture.pcap -c 10
```

A successful capture initializes without USB or firmware errors, reports received frames, and produces packets that `tcpdump` can decode. Only one process or driver object can claim a given adapter at a time.

## Driver behavior and USB details

Initialization follows the kernel driver's `tu_ssv6xxx_init_mac` path:

1. Enumerate the USB adapter and read its chip ID.
2. Reset the MAC and configure HCI, MMU, TX/RX thresholds, TSF, addresses, filters, and RX routing.
3. Download firmware and verify its running version.
4. Enable monitor mode and set the selected channel.

In monitor mode, the USB RX stream contains an 80-byte `ssv6006_rx_desc` followed by the 802.11 frame and padding. The driver strips the hardware descriptor before invoking the callback or writing the pcap record.

Frame injection prepends an 80-byte `ssv6200_tx_desc` before sending a raw 802.11 frame through the USB bulk TX endpoint.

| Endpoint | Direction | Type | Purpose |
|---|---|---|---|
| EP1 (`0x01`) | OUT | Bulk | Commands |
| EP2 (`0x82`) | IN | Bulk | Responses |
| EP3 (`0x03`) | OUT | Bulk | Injected frames |
| EP4 (`0x84`) | IN | Bulk | Received frames |

Register reads and writes use a command/response exchange over EP1/EP2. Firmware is downloaded through a vendor control request (`bRequest = 0xF0`) in chunks of up to 512 bytes.

### USB RX accelerator

Monitor RX enables the EP4 USB RX accelerator by setting bit 3 of register `0x700041AC` through the EP1/EP2 register protocol. On clean exit, the driver stops the RX transfer first, clears that accelerator bit, then shuts down the chip. This teardown order allows the adapter to be used by a later process without physically reconnecting it.

Do not use vendor request `0xF0` to write `0x700041AC`; that request is reserved for firmware download. Do not enable all bits of `0x700041AC` for monitor RX, and do not shut down the chip while the RX transfer and accelerator are active.

## Source files

| Path | Description |
|---|---|
| `include/ssv6xxx/ssv6xxx.hpp` | Public library entry point |
| `src/ssv6xxx_driver.hpp/.cpp` | One-adapter driver API |
| `src/ssv6xxx_usb_transport.hpp/.cpp` | libusb transport and device selection |
| `src/ssv6xxx_chip_init.hpp/.cpp` | Chip and firmware initialization |
| `src/ssv6xxx_monitor_session.hpp/.cpp` | Capture, injection, and pcap output |
| `src/ssv6xxx_types.hpp` | Public configuration and frame types |
| `src/ssv6xxx_regs.hpp` | Register map and bitfields |
| `src/ssv6xxx_descriptors.hpp` | Hardware descriptors and 802.11 helpers |
| `kernel-driver/L.SMAC.19Q3.2126.02_r3408/include/ssv6x5x-sw.h` | Firmware array embedded in the driver |
| `src/main.cpp` | `sv6256p_monitor` command-line application |
| `CMakeLists.txt` | Library and executable build targets |

## License

The [`LICENSE`](LICENSE) file contains the GNU Affero General Public License v3.0 for material whose copyright holders authorize its use. It does not supersede existing per-file licenses: the userspace driver contains `SPDX-License-Identifier: GPL-2.0-only` notices, and the bundled kernel driver contains third-party copyright and license notices that must remain intact. In particular, the kernel-derived driver cannot be redistributed as an AGPL-3.0-only work without the relevant copyright holders' permission. Consult each file's notice before redistribution.
