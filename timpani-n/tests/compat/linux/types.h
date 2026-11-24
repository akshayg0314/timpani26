// SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
// SPDX-License-Identifier: MIT

/* Test-only stand-in for <linux/types.h>. tests/CMakeLists.txt adds this
 * directory to the include path only on hosts that lack the Linux UAPI
 * headers, so the scheduler rules can be unit-tested there as well. The BPF
 * program and the daemon always use the real header. */

#pragma once

#include <stdint.h>

typedef uint8_t __u8;
typedef uint16_t __u16;
typedef uint32_t __u32;
typedef uint64_t __u64;
typedef int8_t __s8;
typedef int16_t __s16;
typedef int32_t __s32;
typedef int64_t __s64;
