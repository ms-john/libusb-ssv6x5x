/*
 * SSV6X5X USB command/response wire protocol structures.
 *
 * These packed structures describe bytes transferred on EP1/EP2. They are
 * internal implementation details of UsbTransport, not part of the public API.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#pragma once

#include <cstdint>

struct Ssv6xxxReadReg {
    uint32_t addr;
} __attribute__((packed));

struct Ssv6xxxReadRegResult {
    uint32_t value;
} __attribute__((packed));

struct Ssv6xxxWriteReg {
    uint32_t addr;
    uint32_t value;
} __attribute__((packed));

union Ssv6xxxPayload {
    Ssv6xxxReadReg rreg;
    Ssv6xxxReadRegResult rreg_res;
    Ssv6xxxWriteReg wreg;
} __attribute__((packed));

struct Ssv6xxxCmdHdr {
    uint8_t plen;
    uint8_t cmd;
    uint16_t seq;
    Ssv6xxxPayload payload;
} __attribute__((packed));
