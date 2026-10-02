#include "ssv6xxx_embedded_firmware.hpp"

#include "ssv6x5x-sw.h"

namespace ssv6xxx
{
    namespace embedded_firmware
    {

        const uint8_t *kernel_data() noexcept
        {
            return ssv6x5x_sw_bin;
        }

        size_t kernel_size() noexcept
        {
            return static_cast<size_t>(ssv6x5x_sw_bin_len);
        }

    } // namespace embedded_firmware
} // namespace ssv6xxx
