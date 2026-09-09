#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

#include "pico/time.h"

#include "bus.h"
#include "usb.h"
#include "log.h"
#include "led.h"
#include "surreal_boot.h"
#include "bootfiles_data.h"

#define DFU_DNLOAD        1
#define DFU_ABORT         4
#define CUSTOM_BOOT       8

/*
 * ============================================================
 * Experimental fast payload transport
 * ============================================================
 *
 * The upstream implementation starts at 0x80 bytes because larger
 * OUT control transfers have historically been unreliable with the
 * RP2350 PIO USB host + Apple DFU combination.
 *
 * This branch intentionally starts at 0x200 and falls back to
 * smaller powers of two when the target rejects/loses a transfer.
 *
 * Override these at compile time when testing another configuration:
 *
 *   -DSURREALBOOT_INITIAL_TRANSFER_SIZE=0x100
 *   -DSURREALBOOT_MIN_TRANSFER_SIZE=0x40
 *   -DSURREALBOOT_MAX_BLOCK=131072
 */
#ifndef SURREALBOOT_INITIAL_TRANSFER_SIZE
#define SURREALBOOT_INITIAL_TRANSFER_SIZE 0x200u
#endif

#ifndef SURREALBOOT_MIN_TRANSFER_SIZE
#define SURREALBOOT_MIN_TRANSFER_SIZE 0x40u
#endif

#ifndef SURREALBOOT_TRANSFER_RETRY_SETTLE_MS
#define SURREALBOOT_TRANSFER_RETRY_SETTLE_MS 50u
#endif

#define INITIAL_TRANSFER_SIZE \
    SURREALBOOT_INITIAL_TRANSFER_SIZE

#define MIN_TRANSFER_SIZE \
    SURREALBOOT_MIN_TRANSFER_SIZE

/*
 * Print a progress line every this many bytes sent.
 * Keeps the hot path silent.
 */
#define PROGRESS_INTERVAL_BYTES (256u * 1024u)

/*
 * Keep failure detection short while testing larger transfers.
 */
#define CTRL_TIMEOUT_MS 100u

/*
 * Decompressed payload staging window.
 *
 * The transport packet size and the decompression window are deliberately
 * independent. A 128 KiB window allows a larger amount of payload to be
 * resident in SRAM before being split into USB transfers.
 */
#ifndef SURREALBOOT_MAX_BLOCK
#define SURREALBOOT_MAX_BLOCK (128u * 1024u)
#endif

#define MAX_BLOCK SURREALBOOT_MAX_BLOCK

/*
 * Dynamic flash payload definitions (for web-flashed universal firmwares).
 * When booted without compile-time embedded payload, surrealboot scans
 * flash addresses for an SBPT header.
 */
#define FLASH_PAYLOAD_OFFSET  (0x10040000u)
#define FLASH_PAYLOAD_MAGIC   (0x53425054u) /* "SBPT" */

struct flash_payload_chunk {
    uint32_t compressed_size;
    uint32_t uncompressed_size;
};

struct flash_payload_header {
    uint32_t magic;
    uint32_t version;
    uint32_t uncompressed_size;
    uint16_t chunk_count;
    uint16_t reserved;
};

/*
 * One decompression window.
 *
 * NOTE:
 * This is intentionally kept as a single buffer in this iteration.
 * The next iteration can turn this into double-buffered/pipelined
 * decompression after the 0x200 transport path is proven stable.
 */
static uint8_t boot_scratch[MAX_BLOCK]
    __attribute__((aligned(4)));

static uint32_t working_transfer_size =
    INITIAL_TRANSFER_SIZE;

/*
 * ============================================================
 * LZ4
 * ============================================================
 */

static int read_len(
    const uint8_t *src,
    uint32_t size,
    uint32_t *pos,
    uint32_t *len
) {
    uint32_t value = *len;

    if (value != 15u) {
        *len = value;
        return 0;
    }

    while (true) {
        if (*pos >= size) {
            return -1;
        }

        uint8_t x = src[(*pos)++];

        value += x;

        if (x != 255u) {
            break;
        }
    }

    *len = value;

    return 0;
}

static int lz4_decompress_block(
    const uint8_t *src,
    uint32_t src_size,
    uint8_t *dst,
    uint32_t dst_capacity,
    uint32_t expected_size
) {
    uint32_t sp = 0;
    uint32_t dp = 0;

    while (sp < src_size) {

        uint8_t token = src[sp++];

        uint32_t literals = token >> 4;

        if (read_len(
                src,
                src_size,
                &sp,
                &literals
            ) != 0) {
            return -1;
        }

        if (literals > src_size - sp) {
            return -1;
        }

        if (literals > dst_capacity - dp) {
            return -1;
        }

        for (uint32_t i = 0; i < literals; ++i) {
            dst[dp++] = src[sp++];
        }

        if (sp == src_size) {
            break;
        }

        if (sp + 2 > src_size) {
            return -1;
        }

        uint32_t offset =
            (uint32_t)src[sp] |
            ((uint32_t)src[sp + 1] << 8);

        sp += 2;

        if (offset == 0 || offset > dp) {
            return -1;
        }

        uint32_t match =
            (token & 0x0F) + 4u;

        if ((token & 0x0F) == 15u) {

            uint32_t extra = 15u;

            if (read_len(
                    src,
                    src_size,
                    &sp,
                    &extra
                ) != 0) {
                return -1;
            }

            match = extra + 4u;
        }

        if (match > dst_capacity - dp) {
            return -1;
        }

        uint32_t from = dp - offset;

        for (uint32_t i = 0; i < match; ++i) {
            dst[dp++] = dst[from + i];
        }
    }

    return dp == expected_size ? 0 : -1;
}

/*
 * ============================================================
 * DFU
 * ============================================================
 */

static int dfu_download_chunk(
    bus_t *b,
    const uint8_t *buf,
    uint16_t len,
    uint32_t offset,
    uint32_t ordinal
) {
    struct usb_setup_req_header {
        uint8_t  bmRequestType;
        uint8_t  bRequest;
        uint16_t wValue;
        uint16_t wIndex;
        uint16_t wLength;
    } __attribute__((packed));

    struct usb_setup_req_header req = {
        .bmRequestType = 0x21,
        .bRequest      = DFU_DNLOAD,
        .wValue        = (uint16_t)(ordinal - 1),
        .wIndex        = 0,
        .wLength       = len,
    };

    int rc = bus_control_xfer(
        b,
        (const uint8_t *)&req,
        (uint8_t *)buf,
        len,
        false,
        CTRL_TIMEOUT_MS
    );

    if (rc != 0) {
        INFO(
            "[DFU] FAILED rc=%d offset=0x%08lx len=0x%04x",
            rc,
            (unsigned long)offset,
            (unsigned)len
        );
    }

    return rc;
}

/*
 * Try the current transfer size.
 *
 * On failure the current transfer is retried at a smaller power-of-two
 * size. The same USB request ordinal is used so that the failed request
 * is not counted as a successfully delivered DFU block.
 */
static int dfu_download_adaptive(
    bus_t *b,
    const uint8_t *buf,
    uint32_t len,
    uint32_t offset,
    uint32_t ordinal
) {
    while (true) {

        uint16_t send_len =
            len > working_transfer_size
            ? (uint16_t)working_transfer_size
            : (uint16_t)len;

        int rc = dfu_download_chunk(
            b,
            buf,
            send_len,
            offset,
            ordinal
        );

        if (rc == 0) {
            return send_len;
        }

        if (rc != -2 && rc != -1) {
            INFO(
                "[DFU] unrecoverable error rc=%d",
                rc
            );

            return rc;
        }

        if (rc == -2) {
            INFO(
                "[DFU] timeout at chunk size 0x%lx",
                (unsigned long)working_transfer_size
            );
        } else {
            INFO(
                "[DFU] pipe stall at chunk size 0x%lx",
                (unsigned long)working_transfer_size
            );
        }

        if (working_transfer_size <= MIN_TRANSFER_SIZE) {
            INFO(
                "[DFU] minimum transfer size 0x%lx also failed (rc=%d)",
                (unsigned long)working_transfer_size,
                rc
            );

            return rc;
        }

        INFO(
            "[DFU] resetting USB bus before retry"
        );

        usb_bus_reset_open_ep0();

        /*
         * Smaller settle delay than the original 150 ms. This is intended
         * to reduce the penalty of probing an overly aggressive transfer
         * size while still giving the USB state machine time to recover.
         */
        sleep_ms(
            SURREALBOOT_TRANSFER_RETRY_SETTLE_MS
        );

        working_transfer_size >>= 1;

        if (working_transfer_size < MIN_TRANSFER_SIZE) {
            working_transfer_size = MIN_TRANSFER_SIZE;
        }

        INFO(
            "[DFU] fallback transfer size = 0x%lx",
            (unsigned long)working_transfer_size
        );
    }
}

static int dfu_download_finish(
    bus_t *b
) {
    struct usb_setup_req_header {
        uint8_t  bmRequestType;
        uint8_t  bRequest;
        uint16_t wValue;
        uint16_t wIndex;
        uint16_t wLength;
    } __attribute__((packed));

    struct usb_setup_req_header req = {
        .bmRequestType = 0x21,
        .bRequest      = DFU_DNLOAD,
        .wValue        = 0,
        .wIndex        = 0,
        .wLength       = 0,
    };

    INFO(
        "[DFU] sending zero-length termination"
    );

    int rc = bus_control_xfer(
        b,
        (const uint8_t *)&req,
        NULL,
        0,
        false,
        CTRL_TIMEOUT_MS
    );

    INFO(
        "[DFU] termination rc=%d",
        rc
    );

    return rc;
}

static int custom_request(
    bus_t *b,
    uint8_t request
) {
    struct usb_setup_req_header {
        uint8_t  bmRequestType;
        uint8_t  bRequest;
        uint16_t wValue;
        uint16_t wIndex;
        uint16_t wLength;
    } __attribute__((packed));

    struct usb_setup_req_header req = {
        .bmRequestType = 0x21,
        .bRequest      = request,
        .wValue        = 0,
        .wIndex        = 0,
        .wLength       = 0,
    };

    INFO(
        "[DFU] control request %u",
        (unsigned)request
    );

    int rc = bus_control_xfer(
        b,
        (const uint8_t *)&req,
        NULL,
        0,
        false,
        CTRL_TIMEOUT_MS
    );

    INFO(
        "[DFU] request %u rc=%d",
        (unsigned)request,
        rc
    );

    return rc;
}

/*
 * ============================================================
 * Boot payload
 * ============================================================
 */

static const struct flash_payload_header *
find_flash_payload(
    uint32_t *out_base
) {
    static const uint32_t offsets[] = {
        0x10020000u,
        0x10040000u
    };

    for (
        size_t i = 0;
        i < sizeof(offsets) / sizeof(offsets[0]);
        i++
    ) {
        const struct flash_payload_header *hdr =
            (const struct flash_payload_header *)offsets[i];

        if (
            hdr->magic == FLASH_PAYLOAD_MAGIC &&
            hdr->chunk_count > 0
        ) {
            if (out_base) {
                *out_base = offsets[i];
            }

            return hdr;
        }
    }

    return NULL;
}

static int boot_internal(
    bus_t *b,
    void *ctx
) {
    (void)ctx;

    /*
     * Check for dynamic flash payload at 0x10020000 / 0x10040000.
     */
    uint32_t flash_base = 0;

    const struct flash_payload_header *fhdr =
        find_flash_payload(&flash_base);

    bool have_flash =
        (fhdr != NULL);

#if BOOTFILE_COUNT > 0
    bool have_embedded =
        (bootfiles[0].chunk_count > 0);
#else
    bool have_embedded = false;
#endif

    if (!have_flash && !have_embedded) {
        INFO(
            "[BOOT] no payload found "
            "(no compiled embedded payload and no valid SBPT header in flash)"
        );

        return -1;
    }

    const char *payload_name = NULL;

    uint32_t uncompressed_size = 0;
    uint16_t chunk_count = 0;

    const struct flash_payload_chunk *fchunks = NULL;
    const uint8_t *fblob_ptr = NULL;

    if (have_flash) {

        payload_name =
            (flash_base == 0x10020000u)
            ? "flash@0x10020000"
            : "flash@0x10040000";

        uncompressed_size =
            fhdr->uncompressed_size;

        chunk_count =
            fhdr->chunk_count;

        fchunks =
            (const struct flash_payload_chunk *)(
                flash_base +
                sizeof(struct flash_payload_header)
            );

        fblob_ptr =
            (const uint8_t *)(fchunks + chunk_count);
    }

#if BOOTFILE_COUNT > 0
    else {

        const struct bootfile_desc *boot =
            &bootfiles[0];

        payload_name =
            boot->name;

        uncompressed_size =
            boot->uncompressed_size;

        chunk_count =
            boot->chunk_count;
    }
#endif

    /*
     * Every boot operation begins with the experimentally selected
     * fast-path size.
     */
    working_transfer_size =
        INITIAL_TRANSFER_SIZE;

    uint64_t t_start =
        time_us_64();

    uint32_t last_log_pos =
        0;

    INFO("");
    INFO(
        "[BOOT] ========================================"
    );

    INFO(
        "[BOOT] Embedded boot payload (%s)",
        have_flash
            ? "FLASH SBPT"
            : "COMPILED"
    );

    INFO(
        "[BOOT] ========================================"
    );

    INFO(
        "[BOOT] name: %s",
        payload_name
    );

    INFO(
        "[BOOT] uncompressed size: %lu bytes",
        (unsigned long)uncompressed_size
    );

    INFO(
        "[BOOT] compressed blocks: %u",
        (unsigned)chunk_count
    );

    INFO(
        "[BOOT] initial DFU chunk size: 0x%lx",
        (unsigned long)working_transfer_size
    );

    INFO(
        "[BOOT] decompressed SRAM window: %lu bytes",
        (unsigned long)sizeof(boot_scratch)
    );

    INFO(
        "[BOOT] ========================================"
    );

    size_t sent = 0;
    uint32_t ordinal = 0;

    for (
        uint16_t block_index = 0;
        block_index < chunk_count;
        ++block_index
    ) {
        const uint8_t *chunk_start = NULL;

        uint32_t chunk_compressed_size = 0;
        uint32_t chunk_uncompressed_size = 0;

        if (have_flash) {

            chunk_compressed_size =
                fchunks[block_index].compressed_size;

            chunk_uncompressed_size =
                fchunks[block_index].uncompressed_size;

            chunk_start =
                fblob_ptr;

            fblob_ptr +=
                chunk_compressed_size;
        }

#if BOOTFILE_COUNT > 0
        else {

            const struct bootfile_chunk *chunk =
                &bootfiles[0].chunks[block_index];

            chunk_start =
                chunk->start;

            chunk_compressed_size =
                chunk->compressed_size;

            chunk_uncompressed_size =
                chunk->uncompressed_size;
        }
#endif

        if (
            chunk_uncompressed_size >
            MAX_BLOCK
        ) {
            INFO(
                "[BOOT] invalid block %u metadata",
                (unsigned)block_index
            );

            return -1;
        }

        /*
         * Decompress the complete block into SRAM before starting
         * its USB transfer.
         */
        int rc = lz4_decompress_block(
            chunk_start,
            chunk_compressed_size,
            boot_scratch,
            sizeof(boot_scratch),
            chunk_uncompressed_size
        );

        if (rc != 0) {
            INFO(
                "[LZ4] decode FAILED block=%u rc=%d",
                (unsigned)block_index,
                rc
            );

            return -1;
        }

        uint8_t *ptr =
            boot_scratch;

        uint32_t left =
            chunk_uncompressed_size;

        while (left > 0) {

            uint32_t chunk_size =
                left > working_transfer_size
                ? working_transfer_size
                : left;

            ++ordinal;

            int sent_now =
                dfu_download_adaptive(
                    b,
                    ptr,
                    chunk_size,
                    (uint32_t)sent,
                    ordinal
                );

            if (sent_now < 0) {
                INFO(
                    "[BOOT] DFU transfer failed "
                    "offset=0x%08lx rc=%d",
                    (unsigned long)sent,
                    sent_now
                );

                return sent_now;
            }

            ptr += sent_now;

            left -=
                (uint32_t)sent_now;

            sent +=
                (uint32_t)sent_now;

            if (
                sent - last_log_pos >=
                PROGRESS_INTERVAL_BYTES
            ) {
                uint64_t elapsed_us =
                    time_us_64() - t_start;

                uint32_t kbps =
                    elapsed_us > 0
                    ? (uint32_t)(
                        (uint64_t)sent * 1000u /
                        (elapsed_us / 1000u)
                    )
                    : 0u;

                INFO(
                    "[BOOT] %lu / %lu bytes "
                    "(%u KBps sz=0x%lx)",
                    (unsigned long)sent,
                    (unsigned long)uncompressed_size,
                    (unsigned)kbps,
                    (unsigned long)working_transfer_size
                );

                last_log_pos =
                    sent;
            }
        }
    }

    if (sent != uncompressed_size) {
        INFO(
            "[BOOT] size mismatch sent=%lu expected=%lu",
            (unsigned long)sent,
            (unsigned long)uncompressed_size
        );

        return -1;
    }

    {
        uint64_t total_us =
            time_us_64() - t_start;

        uint32_t elapsed_ms =
            (uint32_t)(total_us / 1000u);

        uint32_t kbps =
            total_us > 0
            ? (uint32_t)(
                (uint64_t)sent * 1000u /
                (total_us / 1000u)
            )
            : 0u;

        INFO(
            "[BOOT] ALL PAYLOAD BYTES SENT: "
            "%lu bytes in %lu ms (%u KBps)",
            (unsigned long)sent,
            (unsigned long)elapsed_ms,
            (unsigned)kbps
        );
    }

    INFO(
        "[BOOT] final transfer size: 0x%lx",
        (unsigned long)working_transfer_size
    );

    int finish_rc =
        dfu_download_finish(b);

    INFO(
        "[BOOT] DFU termination rc=%d",
        finish_rc
    );

    int boot_rc =
        custom_request(
            b,
            CUSTOM_BOOT
        );

    INFO(
        "[BOOT] CUSTOM_BOOT rc=%d",
        boot_rc
    );

    int abort_rc =
        custom_request(
            b,
            DFU_ABORT
        );

    INFO(
        "[BOOT] DFU_ABORT rc=%d",
        abort_rc
    );

    uint64_t total_us =
        time_us_64() - t_start;

    uint32_t elapsed_sec =
        (uint32_t)(total_us / 1000000u);

    INFO("");

    INFO(
        "[SUCCESS] ========================================"
    );

    INFO(
        "[SUCCESS] PAYLOAD DATA TRANSFER COMPLETE"
    );

    INFO(
        "[SUCCESS] ALL BYTES DELIVERED SUCCESSFULLY"
    );

    INFO(
        "[SUCCESS] FINAL TRANSFER SIZE: 0x%lx",
        (unsigned long)working_transfer_size
    );

    INFO(
        "[SUCCESS] ========================================"
    );

    INFO(
        "[SUCCESS] Took %lu seconds to transfer",
        (unsigned long)elapsed_sec
    );

    INFO(
        "[SUCCESS] Device should boot now!"
    );

    return 0;
}

int surreal_boot_run(void)
{
    const struct flash_payload_header *fhdr =
        find_flash_payload(NULL);

    bool have_flash =
        (fhdr != NULL);

#if BOOTFILE_COUNT > 0
    bool have_embedded =
        (bootfiles[0].chunk_count > 0);
#else
    bool have_embedded = false;
#endif

    if (!have_flash && !have_embedded) {
        INFO(
            "[BOOT] no payload found "
            "in flash or firmware image"
        );

        return -1;
    }

    INFO(
        "[BOOT] entering USB-host payload transfer stage"
    );

    return usb_bus_execute(
        boot_internal,
        NULL,
        0
    );
}
