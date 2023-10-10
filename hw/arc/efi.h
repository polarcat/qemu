/*
 * UEFI PI support
 *
 * Copyright (c) 2023 Basemark Oy
 *
 * Author: Aliaksei Katovich @ basemark.com
 *
 * Released under the GNU General Public License, version 2
 *
 */

#include "qemu/osdep.h"
#include "exec/cpu-defs.h"

#define EFI_INVALID_ENTRY_POINT 0xffffffff

/**
 * Find and return UEFI firmware entry point.
 *
 * @param path to UEFI FD file.
 *
 * @return UEFI firmware entry point on success, EFI_INVALID_ENTRY_POINT on
 * failure.
 *
 */
target_ulong efi_probe_firmware(const char *file);
