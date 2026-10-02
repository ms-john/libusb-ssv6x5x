#pragma once

#include <cstddef>
#include <cstdint>

namespace ssv6xxx
{
    namespace embedded_firmware
    {

        const uint8_t *kernel_data() noexcept;
        size_t kernel_size() noexcept;

    } // namespace embedded_firmware
} // namespace ssv6xxx
