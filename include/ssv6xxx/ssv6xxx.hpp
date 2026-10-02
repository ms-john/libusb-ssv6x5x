/* Public entry point for the SSV6X5X userspace driver. */
#pragma once

/*
 * One SSV6xxxDriver instance exclusively owns one USB adapter. Create one
 * object per physical adapter; do not share an adapter between objects.
 */
#include "ssv6xxx_driver.hpp"
