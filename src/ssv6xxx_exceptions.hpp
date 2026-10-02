/*
 * ssv6xxx_exceptions.hpp - SSV6X5X 异常类层次结构 (C++14)
 *
 * 定义驱动框架使用的异常类型：
 *   - DriverException      : 基础异常类（所有驱动的异常的父类）
 *   - UsbException         : USB 设备相关错误
 *   - InitException        : 芯片初始化相关异常
 *   - InvalidStateException: 操作状态无效异常
 *
 * 使用示例：
 * @code
 * try {
 *     driver.init(config);
 * } catch (const ssv6xxx::UsbException& e) {
 *     logger->error("USB 错误: {}", e.what());
 * } catch (const ssv6xxx::InitException& e) {
 *     logger->error("初始化失败: {}", e.what());
 * } catch (const ssv6xxx::DriverException& e) {
 *     logger->error("驱动错误: {}", e.what());
 * }
 * @endcode
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <stdexcept>
#include <string>

namespace ssv6xxx
{

    /* ==================== 异常基类 ==================== */

    /**
     * @brief 驱动异常基类
     *
     * 所有 SSV6xxx 驱动相关的异常都继承自此类。
     * 可以通过 catch(DriverException&) 捕获所有驱动异常。
     *
     * 设计原则：
     *   - 继承 std::runtime_error 以兼容标准异常处理
     *   - 提供统一的错误消息格式
     *   - 支持多态捕获和类型识别
     */
    class DriverException : public std::runtime_error
    {
    public:
        /**
         * @brief 构造函数（std::string 版本）
         * @param msg 错误消息
         */
        explicit DriverException(const std::string &msg)
            : std::runtime_error(msg)
        {
        }

        /**
         * @brief 构造函数（C 字符串版本）
         * @param msg 错误消息
         */
        explicit DriverException(const char *msg)
            : std::runtime_error(msg)
        {
        }

        /**
         * @brief 虚析构函数
         *
         * 确保通过基类指针删除派生类对象时的正确行为。
         */
        virtual ~DriverException() = default;
    };

    /* ==================== USB 异常 ==================== */

    /**
     * @brief USB 设备相关异常
     *
     * 在以下情况抛出：
     *   - USB 设备未找到（VID/PID 不匹配）
     *   - 权限不足（需要 root/sudo 访问 USB 设备）
     *   - USB 传输失败（超时、设备断开）
     *   - libusb 初始化失败
     *   - 接口声明或分离内核驱动失败
     *
     * 错误消息格式：以 "USB 错误: " 为前缀
     *
     * 使用示例：
     * @code
     * if (!device_found) {
     *     throw ssv6xxx::UsbException("未找到 SSV6x5x 设备 (VID=0x8065,
     * PID=0x6000)");
     * }
     * @endcode
     */
    class UsbException : public DriverException
    {
    public:
        /**
         * @brief 构造函数（自动添加 "USB 错误: " 前缀）
         * @param msg 具体的错误描述
         */
        explicit UsbException(const std::string &msg)
            : DriverException("USB 错误: " + msg)
        {
        }

        /**
         * @brief 构造函数（C 字符串版本）
         * @param msg 具体的错误描述
         */
        explicit UsbException(const char *msg)
            : DriverException(std::string("USB 错误: ") + msg)
        {
        }

        ~UsbException() override = default;
    };

    /* ==================== 初始化异常 ==================== */

    /**
     * @brief 芯片初始化相关异常
     *
     * 在以下情况抛出：
     *   - 固件文件不存在或无法读取
     *   - 芯片 ID 不匹配或不支持
     *   - 寄存器操作超时（轮询失败）
     *   - 固件下载失败（USB 传输错误）
     *   - 固件验证失败（版本读取错误）
     *   - PHY/RF 校准表加载失败
     *   - 信道设置失败
     *
     * 错误消息格式：以 "初始化失败: " 为前缀
     *
     * 使用示例：
     * @code
     * if (chip_id != EXPECTED_CHIP_ID) {
     *     throw ssv6xxx::InitException(
     *         std::format("芯片 ID 不匹配: 期望 0x{:04X}, 实际 0x{:04X}",
     *                     EXPECTED_CHIP_ID, chip_id));
     * }
     * @endcode
     */
    class InitException : public DriverException
    {
    public:
        /**
         * @brief 构造函数（自动添加 "初始化失败: " 前缀）
         * @param msg 具体的错误描述
         */
        explicit InitException(const std::string &msg)
            : DriverException("初始化失败: " + msg)
        {
        }

        /**
         * @brief 构造函数（C 字符串版本）
         * @param msg 具体的错误描述
         */
        explicit InitException(const char *msg)
            : DriverException(std::string("初始化失败: ") + msg)
        {
        }

        ~InitException() override = default;
    };

    /* ==================== 状态无效异常 ==================== */

    /**
     * @brief 操作状态无效异常
     *
     * 在以下情况抛出：
     *   - 未初始化就调用需要初始化的操作（如 start_monitor）
     *   - 监控未启动就调用停止操作
     *   - 重复调用 init() 而未先调用 shutdown()
     *   - 在错误的阶段执行不允许的操作
     *
     * 错误消息格式：以 "无效状态: " 为前缀
     *
     * 使用示例：
     * @code
     * void SSV6xxxDriver::start_monitor() {
     *     if (!initialized_) {
     *         throw InvalidStateException("驱动未初始化，请先调用 init()");
     *     }
     *     // ... 启动监控
     * }
     * @endcode
     */
    class InvalidStateException : public DriverException
    {
    public:
        /**
         * @brief 构造函数（自动添加 "无效状态: " 前缀）
         * @param msg 具体的状态错误描述
         */
        explicit InvalidStateException(const std::string &msg)
            : DriverException("无效状态: " + msg)
        {
        }

        /**
         * @brief 构造函数（C 字符串版本）
         * @param msg 具体的状态错误描述
         */
        explicit InvalidStateException(const char *msg)
            : DriverException(std::string("无效状态: ") + msg)
        {
        }

        ~InvalidStateException() override = default;
    };

} // namespace ssv6xxx
