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

#include "efi.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qemu/option.h"
#include "sysemu/blockdev.h"
#include "hw/sysbus.h"
#include "hw/loader.h"
#include "hw/qdev-properties.h"
#include "hw/block/flash.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define ee(...) fprintf(stderr, "E efi: " __VA_ARGS__)
#define ii(...) fprintf(stderr, "I efi: " __VA_ARGS__)

#define SECURITY_CORE 0x03
#define SECTION_TE 0x12

#define FV_SIGNATURE "_FVH"
#define TE_SIGNATURE "VZ"

#define RESET_VECTOR_SIZE 16

/* EFI_FIRMWARE_VOLUME_HEADER, edk2/MdePkg/Include/Pi/PiFirmwareVolume.h */
struct fv_hdr {
    uint8_t reset_vector[RESET_VECTOR_SIZE];
    uint64_t guid[2];
    uint64_t fv_len; /* including this header */
    uint32_t sign;
    uint32_t attrs;
    uint16_t hdr_len;
    /* The rest of original FV header is ignored */
};

/* EFI_FFS_FILE_HEADER, edk2/MdePkg/Include/Pi/PiFirmwareFile.h */
struct file_hdr {
    uint64_t guid[2];
    uint16_t integrity;
    uint8_t type;
    uint8_t attrs;
    uint8_t size[3];
    uint8_t state;
};

/* EFI_COMMON_SECTION_HEADER, edk2/MdePkg/Include/Pi/PiFirmwareFile.h */
struct sect_hdr {
    uint8_t size[3];
    uint8_t type;
};

/* EFI_TE_IMAGE HEADER, edk2/MdePkg/Include/IndustryStandard/PeImage.h */
struct te_hdr {
    uint16_t sign; /* 'VZ' */
    uint16_t mach;
    uint8_t sects_num;
    uint8_t subsys;
    uint16_t stripped_size;
    uint32_t entry_point;
    uint32_t code_base;
    uint64_t image_base;
    struct data_dir {
        uint32_t virt_addr;
        uint32_t size;
    } data_dir;
};

#ifdef DEBUG
static void print_te_hdr(struct te_hdr *te)
{
    printf("\n\033[2m> TE header (sizeof %zu)\n"
        "---------------------+-------------\n"
        "  mach               | 0x%04x\n"
        "  sects_num          | 0x%02x\n"
        "  subsys             | 0x%02x\n"
        "  stripped_size      | 0x%04x\n"
        "  entry_point        | 0x%08x\n"
        "  code_base          | 0x%08x\n"
        "  image_base         | 0x%zx\n"
        "  data_dir.virt_addr | 0x%08x\n"
        "  data_dir.size      | 0x%08x\n"
        "---------------------+-------------\n"
        "\033[0m\n",
        sizeof(*te),
        te->mach,
        te->sects_num,
        te->subsys,
        te->stripped_size,
        te->entry_point,
        te->code_base,
        te->image_base,
        te->data_dir.virt_addr,
        te->data_dir.size);
}
#else
#define print_te_hdr(te) ;
#endif

static inline uint32_t get_size(uint8_t size[3])
{
    return (uint32_t) (size[0] | size[1] <<  8 | size[2] << 16) & 0x0fff;
}

static inline uint8_t *align4_ptr(uint8_t *ptr)
{
    return (uint8_t *) (((uint64_t) ptr + 3) & ~3ULL);
}

static inline uint8_t *align8_ptr(uint8_t *ptr)
{
    return (uint8_t *) (((uint64_t) ptr + 7) & ~7ULL);
}

static inline bool reset_vector_empty(uint8_t *buf)
{
    int i;

    for (i = 0; i < RESET_VECTOR_SIZE; ++i) {
        if (buf[i] != 0) {
            break;
        }
    }

    return i == RESET_VECTOR_SIZE;
}

static inline bool check_te_sign(struct te_hdr *te)
{
    uint16_t sign = *(uint16_t *) TE_SIGNATURE;
    return (te->sign == sign);
}

static uint32_t get_sec_entry_point(struct file_hdr *file)
{
    uint8_t *ptr = (uint8_t *) file + sizeof(*file);
    uint8_t *end = ptr + get_size(file->size);

    ptr = align4_ptr(ptr);
    while (ptr < end) {
        struct te_hdr *te;
        struct sect_hdr *sect = (struct sect_hdr *) ptr;

        if (sect->type != SECTION_TE) {
            ptr += get_size(sect->size);
            ptr = align4_ptr(ptr);
            continue;
        }

        te = (struct te_hdr *) (ptr + sizeof(*sect));
        if (!check_te_sign(te)) {
            ee("Bad TE signature 0x%x\n", te->sign);
            return EFI_INVALID_ENTRY_POINT;
        }

        print_te_hdr(te);
        return te->entry_point;
    }

    return EFI_INVALID_ENTRY_POINT;
}

static uint32_t get_entry_point(uint8_t *blob, struct fv_hdr *fv_hdr)
{
    uint8_t *ptr = blob + fv_hdr->hdr_len;
    uint8_t *end = ptr + fv_hdr->fv_len;

    ptr = align8_ptr(ptr);
    while (ptr < end) {
        struct file_hdr *file = (struct file_hdr *) ptr;

        if (file->type == SECURITY_CORE) {
            return get_sec_entry_point(file);
        }

        ptr += get_size(file->size);
        ptr = align8_ptr(ptr);
    }

    return EFI_INVALID_ENTRY_POINT;
}

/*
 * Load UEFI BFV into ROM
 */
static uint32_t load_firmware(int fd, size_t fsize, struct fv_hdr *fv_hdr)
{
    uint32_t entry;
    uint8_t *blob = g_malloc(fsize);

    if (!blob) {
        ee("Failed to allocate %zu bytes\n", fsize);
        return EFI_INVALID_ENTRY_POINT;
    }

    lseek(fd, 0, SEEK_SET);
    if (read(fd, blob, fsize) != fsize) {
        ee("Failed to read %zu bytes from %d\n", fsize, fd);
        entry = EFI_INVALID_ENTRY_POINT;
    } else {
        entry = get_entry_point(blob, fv_hdr);
        rom_add_blob_fixed("uefi", blob, fsize, 0);
        ii("SEC entry point 0x%x\n", entry);
    }

    g_free(blob);
    return entry;
}

target_ulong efi_probe_firmware(const char *file)
{
    target_ulong entry;
    int fd;
    struct fv_hdr fv;
    uint32_t fv_sign = *(uint32_t *) FV_SIGNATURE;

    if (!file) {
        return EFI_INVALID_ENTRY_POINT;
    }

    ii("Open '%s'\n", file);

    fd = open(file, O_RDONLY | O_BINARY);
    if (fd < 0) {
        ee("Failed to open '%s': %s\n", file, strerror(errno));
        return EFI_INVALID_ENTRY_POINT;
    }

    lseek(fd, 0, SEEK_SET);
    if (read(fd, &fv, sizeof(fv)) != sizeof(fv)) {
        ee("Failed to read '%s': %s\n", file, strerror(errno));
        entry = 0;
        goto out;
    } else if (fv.sign != fv_sign) {
        ee("Bad FV signature 0x%x != 0x%x\n", fv.sign, fv_sign);
        entry = 0;
        goto out;
    } else if (!reset_vector_empty(fv.reset_vector)) {
        ii("FV reset vector is not empty, will use it\n");
        entry = 0;
        goto out;
    } else {
        /*
         * Even though we are only interested in the very first
         * firmware volume at this stage we still load entire file.
         * This makes it possible to run at least UEFI PI SEC+PEI
         * stages from a single binary blob. The blob can contain
         * multiple firmware volumes.
         */
        size_t fsize = lseek(fd, 0, SEEK_END);
        entry = load_firmware(fd, fsize, &fv);
        if (fsize != fv.fv_len) {
            ii("First FV size %zu, file size %zu\n", fsize, fv.fv_len);
        }
    }

out:
    close(fd);
    return entry;
}
