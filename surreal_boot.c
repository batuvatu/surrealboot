#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "pico/time.h"
#include "pico/multicore.h"
#include "pico/platform.h"

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
 * FAST DFU TRANSPORT
 * ============================================================
 *
 * Experimental fast path:
 *
 *   0x200 -> 0x100 -> 0x80 -> 0x40
 *
 * The PIO USB control-pipe length field was widened to uint16_t
 * by build.sh because the original upstream declaration used
 * uint8_t even though the low-level transfer API accepts uint16_t.
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

#define PROGRESS_INTERVAL_BYTES \
    (256u * 1024u)

#define CTRL_TIMEOUT_MS \
    100u


/*
 * ============================================================
 * SRAM DECOMPRESSION PIPELINE
 * ============================================================
 *
 * RP2350 has 520 KiB SRAM.
 *
 * Use two large buffers:
 *
 *   A = 192 KiB
 *   B = 192 KiB
 *
 * Total payload staging:
 *
 *   384 KiB
 *
 * The remaining SRAM is left for USB, runtime, stacks, metadata,
 * multicore support and other firmware state.
 *
 * The size can be overridden at build time:
 *
 *   -DSURREALBOOT_DECOMP_BUFFER_SIZE=163840
 *
 * or another value appropriate for the board/build.
 */

#ifndef SURREALBOOT_DECOMP_BUFFER_SIZE
#define SURREALBOOT_DECOMP_BUFFER_SIZE \
    (192u * 1024u)
#endif

#define MAX_BLOCK \
    SURREALBOOT_DECOMP_BUFFER_SIZE

static uint8_t boot_buffer_a[MAX_BLOCK]
    __attribute__((aligned(4)));

static uint8_t boot_buffer_b[MAX_BLOCK]
    __attribute__((aligned(4)));


/*
 * ============================================================
 * DYNAMIC FLASH PAYLOAD
 * ============================================================
 */

#define FLASH_PAYLOAD_MAGIC \
    (0x53425054u) /* "SBPT" */

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
 * ============================================================
 * GLOBAL DFU STATE
 * ============================================================
 */

static uint32_t working_transfer_size =
    INITIAL_TRANSFER_SIZE;


/*
 * ============================================================
 * CORE 1 DECOMPRESSOR
 * ============================================================
 *
 * Core 0 owns USB.
 *
 * Core 1 is dedicated to decompression/prefetching.
 *
 * Job information lives in SRAM and the multicore FIFO is used
 * only as a wakeup/command channel.
 */

typedef struct {
    const uint8_t *src;
    uint32_t src_size;

    uint8_t *dst;
    uint32_t dst_capacity;

    uint32_t expected_size;
} decompression_job_t;

static volatile decompression_job_t
    core1_job;

static volatile int core1_result =
    0;

static volatile bool core1_busy =
    false;

static volatile bool core1_done =
    false;

static volatile bool core1_started =
    false;


/*
 * ============================================================
 * LZ4
 * ============================================================
 */

static int __not_in_flash_func(read_len)(
    const uint8_t *src,
    uint32_t size,
    uint32_t *pos,
    uint32_t *len
) {
    uint32_t value =
        *len;

    if (value != 15u) {
        *len = value;
        return 0;
    }

    while (true) {

        if (*pos >= size) {
            return -1;
        }

        uint8_t x =
            src[(*pos)++];

        value += x;

        if (x != 255u) {
            break;
        }
    }

    *len =
        value;

    return 0;
}


/*
 * Keep the complete decompressor in SRAM.
 *
 * This is important on the RP2350 because Core 1 will be reading
 * compressed data from XIP flash while Core 0 is servicing USB.
 */
static int __not_in_flash_func(lz4_decompress_block)(
    const uint8_t *src,
    uint32_t src_size,
    uint8_t *dst,
    uint32_t dst_capacity,
    uint32_t expected_size
) {
    uint32_t sp =
        0;

    uint32_t dp =
        0;

    while (sp < src_size) {

        uint8_t token =
            src[sp++];

        uint32_t literals =
            token >> 4;

        if (read_len(
                src,
                src_size,
                &sp,
                &literals
            ) != 0) {
            return -1;
        }

        if (literals >
            src_size - sp) {
            return -1;
        }

        if (literals >
            dst_capacity - dp) {
            return -1;
        }

        /*
         * Do not call libc memcpy here.
         *
         * The decompressor itself is deliberately resident in SRAM,
         * so the hot path remains under our control.
         */
        for (
            uint32_t i = 0;
            i < literals;
            ++i
        ) {
            dst[dp++] =
                src[sp++];
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

        if (
            offset == 0 ||
            offset > dp
        ) {
            return -1;
        }

        uint32_t match =
            (token & 0x0F) + 4u;

        if (
            (token & 0x0F) == 15u
        ) {
            uint32_t extra =
                15u;

            if (read_len(
                    src,
                    src_size,
                    &sp,
                    &extra
                ) != 0) {
                return -1;
            }

            match =
                extra + 4u;
        }

        if (
            match >
            dst_capacity - dp
        ) {
            return -1;
        }

        uint32_t from =
            dp - offset;

        /*
         * LZ4 matches can overlap, so this copy must be performed
         * byte-by-byte in forward order.
         */
        for (
            uint32_t i = 0;
            i < match;
            ++i
        ) {
            dst[dp++] =
                dst[from + i];
        }
    }

    if (dp != expected_size) {
        return -1;
    }

    return 0;
}


/*
 * ============================================================
 * CORE 1 WORKER
 * ============================================================
 */

static void core1_decompress_worker(void) {
    core1_started =
        true;

    while (true) {

        /*
         * Wait until Core 0 submits a job.
         *
         * The actual job description is stored in shared SRAM.
         * FIFO carries only a wakeup token.
         */
        uint32_t token =
            multicore_fifo_pop_blocking();

        if (token != 0x44454350u) {
            continue;
        }

        /*
         * Make the shared job fields visible before using them.
         */
        __dmb();

        core1_busy =
            true;

        core1_done =
            false;

        int rc =
            lz4_decompress_block(
                core1_job.src,
                core1_job.src_size,
                core1_job.dst,
                core1_job.dst_capacity,
                core1_job.expected_size
            );

        core1_result =
            rc;

        __dmb();

        core1_busy =
            false;

        core1_done =
            true;

        /*
         * Wake Core 0.
         */
        multicore_fifo_push_blocking(
            0x444F4E45u
        );
    }
}


/*
 * Start the dedicated decompression core exactly once.
 */
static void start_decompress_core(void) {
    if (core1_started) {
        return;
    }

    core1_started =
        false;

    core1_done =
        false;

    core1_busy =
        false;

    multicore_launch_core1(
        core1_decompress_worker
    );

    /*
     * Wait until Core 1 actually reached its worker.
     */
    while (!core1_started) {
        tight_loop_contents();
    }
}


/*
 * Submit a decompression job to Core 1.
 */
static void submit_decompression(
    const uint8_t *src,
    uint32_t src_size,
    uint8_t *dst,
    uint32_t dst_capacity,
    uint32_t expected_size
) {
    /*
     * Core 1 must not still be executing another job.
     */
    while (core1_busy) {
        tight_loop_contents();
    }

    core1_job.src =
        src;

    core1_job.src_size =
        src_size;

    core1_job.dst =
        dst;

    core1_job.dst_capacity =
        dst_capacity;

    core1_job.expected_size =
        expected_size;

    core1_result =
        0;

    core1_done =
        false;

    /*
     * Publish the shared job before sending the FIFO token.
     */
    __dmb();

    multicore_fifo_push_blocking(
        0x44454350u
    );
}


/*
 * Wait for Core 1 to complete its current job.
 */
static int wait_decompression(void) {

    while (!core1_done) {
        tight_loop_contents();
    }

    /*
     * Make Core 1's writes visible.
     */
    __dmb();

    return core1_result;
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
        .bmRequestType =
            0x21,

        .bRequest =
            DFU_DNLOAD,

        .wValue =
            (uint16_t)(ordinal - 1),

        .wIndex =
            0,

        .wLength =
            len,
    };

    int rc =
        bus_control_xfer(
            b,
            (const uint8_t *)&req,
            (uint8_t *)buf,
            len,
            false,
            CTRL_TIMEOUT_MS
        );

    if (rc != 0) {

        INFO(
            "[DFU] FAILED rc=%d "
            "offset=0x%08lx len=0x%04x",
            rc,
            (unsigned long)offset,
            (unsigned)len
        );
    }

    return rc;
}


/*
 * Adaptive transport.
 *
 * Start with 0x200.
 *
 * If that fails:
 *
 *     0x200 -> 0x100 -> 0x80 -> 0x40
 *
 * The currently proven 0x80 path therefore remains available as
 * a fallback for devices/USB states that reject larger transfers.
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

        int rc =
            dfu_download_chunk(
                b,
                buf,
                send_len,
                offset,
                ordinal
            );

        if (rc == 0) {
            return send_len;
        }

        if (
            rc != -2 &&
            rc != -1
        ) {
            INFO(
                "[DFU] unrecoverable error rc=%d",
                rc
            );

            return rc;
        }

        if (rc == -2) {

            INFO(
                "[DFU] timeout at "
                "chunk size 0x%lx",
                (unsigned long)
                    working_transfer_size
            );

        } else {

            INFO(
                "[DFU] pipe stall at "
                "chunk size 0x%lx",
                (unsigned long)
                    working_transfer_size
            );
        }

        if (
            working_transfer_size <=
            MIN_TRANSFER_SIZE
        ) {
            INFO(
                "[DFU] minimum transfer size "
                "0x%lx also failed (rc=%d)",
                (unsigned long)
                    working_transfer_size,
                rc
            );

            return rc;
        }

        INFO(
            "[DFU] resetting USB bus "
            "before retry"
        );

        usb_bus_reset_open_ep0();

        sleep_ms(
            SURREALBOOT_TRANSFER_RETRY_SETTLE_MS
        );

        working_transfer_size >>=
            1u;

        if (
            working_transfer_size <
            MIN_TRANSFER_SIZE
        ) {
            working_transfer_size =
                MIN_TRANSFER_SIZE;
        }

        INFO(
            "[DFU] fallback transfer size = 0x%lx",
            (unsigned long)
                working_transfer_size
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
        .bmRequestType =
            0x21,

        .bRequest =
            DFU_DNLOAD,

        .wValue =
            0,

        .wIndex =
            0,

        .wLength =
            0,
    };

    INFO(
        "[DFU] sending zero-length termination"
    );

    int rc =
        bus_control_xfer(
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
        .bmRequestType =
            0x21,

        .bRequest =
            request,

        .wValue =
            0,

        .wIndex =
            0,

        .wLength =
            0,
    };

    INFO(
        "[DFU] control request %u",
        (unsigned)request
    );

    int rc =
        bus_control_xfer(
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
 * PAYLOAD METADATA
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
        i < sizeof(offsets) /
            sizeof(offsets[0]);

        ++i
    ) {

        const struct flash_payload_header *hdr =
            (const struct flash_payload_header *)
                offsets[i];

        if (
            hdr->magic ==
            FLASH_PAYLOAD_MAGIC &&

            hdr->chunk_count > 0
        ) {

            if (out_base) {
                *out_base =
                    offsets[i];
            }

            return hdr;
        }
    }

    return NULL;
}


/*
 * ============================================================
 * PAYLOAD BLOCK DESCRIPTION
 * ============================================================
 */

typedef struct {
    const uint8_t *compressed;
    uint32_t compressed_size;
    uint32_t uncompressed_size;
} payload_block_t;


/*
 * Extract one payload block from either:
 *
 *   FLASH SBPT
 *
 * or:
 *
 *   compiled bootfiles
 */
static bool get_payload_block(
    bool have_flash,
    uint32_t flash_base,
    const struct flash_payload_chunk *fchunks,
    const uint8_t **fblob_ptr,
    uint16_t block_index,
    payload_block_t *out
) {
    if (!out) {
        return false;
    }

    out->compressed =
        NULL;

    out->compressed_size =
        0;

    out->uncompressed_size =
        0;

    if (have_flash) {

        if (!fchunks ||
            !fblob_ptr) {
            return false;
        }

        /*
         * Calculate the beginning of the requested chunk by walking
         * from the payload blob start. The caller keeps a sequential
         * pointer, so block_index is expected to be the current block.
         */
        (void)flash_base;

        if (block_index > 0) {
            return false;
        }

        out->compressed =
            *fblob_ptr;

        out->compressed_size =
            fchunks[block_index]
                .compressed_size;

        out->uncompressed_size =
            fchunks[block_index]
                .uncompressed_size;

        return true;
    }

#if BOOTFILE_COUNT > 0

    {
        const struct bootfile_chunk *chunk =
            &bootfiles[0].chunks[block_index];

        out->compressed =
            chunk->start;

        out->compressed_size =
            chunk->compressed_size;

        out->uncompressed_size =
            chunk->uncompressed_size;

        return true;
    }

#else

    (void)flash_base;
    (void)fchunks;
    (void)fblob_ptr;
    (void)block_index;

    return false;

#endif
}


/*
 * ============================================================
 * SEND ONE DECOMPRESSED BUFFER
 * ============================================================
 */

static int send_decompressed_buffer(
    bus_t *b,
    const uint8_t *buffer,
    uint32_t buffer_size,
    size_t *sent,
    uint32_t *ordinal,
    uint64_t t_start,
    uint32_t *last_log_pos,
    uint32_t total_size
) {
    const uint8_t *ptr =
        buffer;

    uint32_t left =
        buffer_size;

    while (left > 0) {

        uint32_t chunk_size =
            left >
            working_transfer_size
            ? working_transfer_size
            : left;

        ++(*ordinal);

        int sent_now =
            dfu_download_adaptive(
                b,
                ptr,
                chunk_size,
                (uint32_t)(*sent),
                *ordinal
            );

        if (sent_now < 0) {

            INFO(
                "[BOOT] DFU transfer failed "
                "offset=0x%08lx rc=%d",
                (unsigned long)(*sent),
                sent_now
            );

            return sent_now;
        }

        ptr +=
            sent_now;

        left -=
            (uint32_t)sent_now;

        *sent +=
            (uint32_t)sent_now;

        /*
         * Correct speed calculation.
         *
         * The old formula reported values roughly 1000x too high
         * because it mixed milliseconds and microseconds.
         */
        if (
            *sent - *last_log_pos >=
            PROGRESS_INTERVAL_BYTES
        ) {

            uint64_t elapsed_us =
                time_us_64() -
                t_start;

            uint32_t kibps =
                elapsed_us > 0
                ? (uint32_t)(
                    (
                        (uint64_t)(*sent) *
                        1000000u
                    ) /
                    elapsed_us /
                    1024u
                )
                : 0u;

            INFO(
                "[BOOT] %lu / %lu bytes "
                "(%u KiB/s sz=0x%lx)",
                (unsigned long)(*sent),
                (unsigned long)total_size,
                (unsigned)kibps,
                (unsigned long)
                    working_transfer_size
            );

            *last_log_pos =
                (uint32_t)(*sent);
        }
    }

    return 0;
}


/*
 * ============================================================
 * BOOT
 * ============================================================
 */

static int boot_internal(
    bus_t *b,
    void *ctx
) {
    (void)ctx;

    uint32_t flash_base =
        0;

    const struct flash_payload_header *fhdr =
        find_flash_payload(
            &flash_base
        );

    bool have_flash =
        (fhdr != NULL);

#if BOOTFILE_COUNT > 0
    bool have_embedded =
        (bootfiles[0].chunk_count > 0);
#else
    bool have_embedded =
        false;
#endif

    if (
        !have_flash &&
        !have_embedded
    ) {

        INFO(
            "[BOOT] no payload found "
            "(no compiled embedded payload "
            "and no valid SBPT header in flash)"
        );

        return -1;
    }

    const char *payload_name =
        NULL;

    uint32_t uncompressed_size =
        0;

    uint16_t chunk_count =
        0;

    const struct flash_payload_chunk *fchunks =
        NULL;

    const uint8_t *fblob_ptr =
        NULL;

    if (have_flash) {

        payload_name =
            (flash_base ==
             0x10020000u)
            ? "flash@0x10020000"
            : "flash@0x10040000";

        uncompressed_size =
            fhdr->uncompressed_size;

        chunk_count =
            fhdr->chunk_count;

        fchunks =
            (const struct flash_payload_chunk *)(
                flash_base +
                sizeof(
                    struct flash_payload_header
                )
            );

        fblob_ptr =
            (const uint8_t *)(
                fchunks +
                chunk_count
            );
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
     * ========================================================
     * Initial state
     * ========================================================
     */

    working_transfer_size =
        INITIAL_TRANSFER_SIZE;

    uint64_t t_start =
        time_us_64();

    uint32_t last_log_pos =
        0;

    size_t sent =
        0;

    uint32_t ordinal =
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
        (unsigned long)
            uncompressed_size
    );

    INFO(
        "[BOOT] compressed blocks: %u",
        (unsigned)chunk_count
    );

    INFO(
        "[BOOT] initial DFU chunk size: 0x%lx",
        (unsigned long)
            working_transfer_size
    );

    INFO(
        "[BOOT] SRAM buffer size: %lu bytes",
        (unsigned long)
            sizeof(boot_buffer_a)
    );

    INFO(
        "[BOOT] double-buffer SRAM: %lu bytes",
        (unsigned long)(
            sizeof(boot_buffer_a) +
            sizeof(boot_buffer_b)
        )
    );

    INFO(
        "[BOOT] target throughput: >=168 KiB/s"
    );

    INFO(
        "[BOOT] target time: <15 seconds"
    );

    INFO(
        "[BOOT] ========================================"
    );


    /*
     * ========================================================
     * Start Core 1
     * ========================================================
     */

    start_decompress_core();


    /*
     * ========================================================
     * Pipeline state
     * ========================================================
     *
     * We maintain two decoded buffers:
     *
     *   A = currently being sent
     *   B = being prepared by Core 1
     */

    uint8_t *buffers[2] = {
        boot_buffer_a,
        boot_buffer_b
    };

    uint16_t current_block =
        0;

    uint8_t current_buffer =
        0;

    uint8_t next_buffer =
        1;


    /*
     * ========================================================
     * Payload source cursor
     * ========================================================
     */

    const uint8_t *flash_cursor =
        fblob_ptr;


    /*
     * Helper used to describe the next block.
     */
    payload_block_t next_block = {
        0
    };


    /*
     * ========================================================
     * PRIME FIRST BLOCK
     * ========================================================
     */

    if (have_flash) {

        if (current_block >=
            chunk_count) {
            return -1;
        }

        next_block.compressed =
            flash_cursor;

        next_block.compressed_size =
            fchunks[current_block]
                .compressed_size;

        next_block.uncompressed_size =
            fchunks[current_block]
                .uncompressed_size;

        if (
            next_block.uncompressed_size >
            MAX_BLOCK
        ) {

            INFO(
                "[BOOT] block %u too large: "
                "%lu > %lu",
                (unsigned)current_block,
                (unsigned long)
                    next_block.uncompressed_size,
                (unsigned long)
                    MAX_BLOCK
            );

            return -1;
        }

        submit_decompression(
            next_block.compressed,
            next_block.compressed_size,
            buffers[current_buffer],
            MAX_BLOCK,
            next_block.uncompressed_size
        );

        /*
         * Advance compressed cursor now.
         */
        flash_cursor +=
            next_block.compressed_size;

    } else {

#if BOOTFILE_COUNT > 0

        const struct bootfile_chunk *chunk =
            &bootfiles[0]
                .chunks[current_block];

        submit_decompression(
            chunk->start,
            chunk->compressed_size,
            buffers[current_buffer],
            MAX_BLOCK,
            chunk->uncompressed_size
        );

#endif
    }


    /*
     * Wait until the first block is decoded.
     */
    if (wait_decompression() != 0) {

        INFO(
            "[LZ4] decode FAILED "
            "block=%u rc=%d",
            (unsigned)current_block,
            core1_result
        );

        return -1;
    }


    /*
     * ========================================================
     * MAIN PIPELINE
     * ========================================================
     */

    while (current_block <
           chunk_count) {

        /*
         * Current block is decoded and ready.
         */

        uint32_t current_size =
            have_flash
            ? fchunks[current_block]
                .uncompressed_size
            : 0;


#if BOOTFILE_COUNT > 0

        if (!have_flash) {

            current_size =
                bootfiles[0]
                    .chunks[current_block]
                    .uncompressed_size;
        }

#endif


        /*
         * ----------------------------------------------------
         * Launch next decompression before USB transmission.
         * ----------------------------------------------------
         */

        bool next_submitted =
            false;

        uint16_t next_block_index =
            current_block + 1u;

        if (
            next_block_index <
            chunk_count
        ) {

            if (have_flash) {

                uint32_t next_size =
                    fchunks[
                        next_block_index
                    ].uncompressed_size;

                if (
                    next_size >
                    MAX_BLOCK
                ) {

                    INFO(
                        "[BOOT] next block %u "
                        "too large: %lu",
                        (unsigned)
                            next_block_index,
                        (unsigned long)
                            next_size
                    );

                    return -1;
                }

                const uint8_t *src =
                    flash_cursor;

                uint32_t compressed_size =
                    fchunks[
                        next_block_index
                    ].compressed_size;

                submit_decompression(
                    src,
                    compressed_size,
                    buffers[next_buffer],
                    MAX_BLOCK,
                    next_size
                );

                flash_cursor +=
                    compressed_size;

                next_submitted =
                    true;

            }

#if BOOTFILE_COUNT > 0

            else {

                const struct bootfile_chunk *chunk =
                    &bootfiles[0]
                        .chunks[next_block_index];

                submit_decompression(
                    chunk->start,
                    chunk->compressed_size,
                    buffers[next_buffer],
                    MAX_BLOCK,
                    chunk->uncompressed_size
                );

                next_submitted =
                    true;
            }

#endif
        }


        /*
         * ----------------------------------------------------
         * Send current block.
         * ----------------------------------------------------
         */

        int send_rc =
            send_decompressed_buffer(
                b,
                buffers[current_buffer],
                current_size,
                &sent,
                &ordinal,
                t_start,
                &last_log_pos,
                uncompressed_size
            );

        if (send_rc != 0) {
            return send_rc;
        }


        /*
         * ----------------------------------------------------
         * If a next block was submitted, wait for it NOW.
         *
         * This wait should mostly be hidden behind the USB
         * transmission time of the current block.
         * ----------------------------------------------------
         */

        if (next_submitted) {

            if (wait_decompression() != 0) {

                INFO(
                    "[LZ4] decode FAILED "
                    "block=%u rc=%d",
                    (unsigned)
                        next_block_index,
                    core1_result
                );

                return -1;
            }
        }


        /*
         * Advance the double-buffer cursor.
         */

        current_block =
            next_block_index;

        current_buffer ^= 1u;
        next_buffer ^= 1u;
    }


    /*
     * ========================================================
     * FINAL VALIDATION
     * ========================================================
     */

    if (
        sent !=
        uncompressed_size
    ) {

        INFO(
            "[BOOT] size mismatch "
            "sent=%lu expected=%lu",
            (unsigned long)sent,
            (unsigned long)
                uncompressed_size
        );

        return -1;
    }


    /*
     * ========================================================
     * Final transfer statistics
     * ========================================================
     */

    uint64_t total_us =
        time_us_64() -
        t_start;

    uint32_t elapsed_ms =
        (uint32_t)(
            total_us /
            1000u
        );

    uint32_t kibps =
        total_us > 0
        ? (uint32_t)(
            (
                (uint64_t)sent *
                1000000u
            ) /
            total_us /
            1024u
        )
        : 0u;

    INFO(
        "[BOOT] ALL PAYLOAD BYTES SENT: "
        "%lu bytes in %lu ms (%u KiB/s)",
        (unsigned long)sent,
        (unsigned long)elapsed_ms,
        (unsigned)kibps
    );

    INFO(
        "[BOOT] final transfer size: 0x%lx",
        (unsigned long)
            working_transfer_size
    );


    /*
     * ========================================================
     * DFU TERMINATION
     * ========================================================
     */

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

    /*
     * Some Apple DFU implementations disconnect immediately
     * after CUSTOM_BOOT. An abort failure at this point is
     * therefore not considered a payload-data failure.
     */
    INFO(
        "[BOOT] DFU_ABORT rc=%d",
        abort_rc
    );


    /*
     * ========================================================
     * SUCCESS
     * ========================================================
     */

    uint32_t elapsed_sec =
        (uint32_t)(
            total_us /
            1000000u
        );

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
        (unsigned long)
            working_transfer_size
    );

    INFO(
        "[SUCCESS] ========================================"
    );

    INFO(
        "[SUCCESS] Took %lu seconds "
        "to transfer",
        (unsigned long)
            elapsed_sec
    );

    INFO(
        "[SUCCESS] Average speed: %u KiB/s",
        (unsigned)kibps
    );

    INFO(
        "[SUCCESS] Device should boot now!"
    );

    return 0;
}


/*
 * ============================================================
 * PUBLIC ENTRY
 * ============================================================
 */

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

    bool have_embedded =
        false;

#endif

    if (
        !have_flash &&
        !have_embedded
    ) {

        INFO(
            "[BOOT] no payload found "
            "in flash or firmware image"
        );

        return -1;
    }

    INFO(
        "[BOOT] entering USB-host "
        "payload transfer stage"
    );

    return usb_bus_execute(
        boot_internal,
        NULL,
        0
    );
}
