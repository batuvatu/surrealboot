#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "pico/time.h"
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
 */

#ifndef SURREALBOOT_INITIAL_TRANSFER_SIZE
#define SURREALBOOT_INITIAL_TRANSFER_SIZE 0x200u
#endif

#ifndef SURREALBOOT_MIN_TRANSFER_SIZE
#define SURREALBOOT_MIN_TRANSFER_SIZE 0x40u
#endif

#ifndef SURREALBOOT_TRANSFER_RETRY_SETTLE_MS
#define SURREALBOOT_TRANSFER_RETRY_SETTLE_MS 10u
#endif

#define INITIAL_TRANSFER_SIZE \
    SURREALBOOT_INITIAL_TRANSFER_SIZE

#define MIN_TRANSFER_SIZE \
    SURREALBOOT_MIN_TRANSFER_SIZE

#define PROGRESS_INTERVAL_BYTES \
    (128u * 1024u)

#define CTRL_TIMEOUT_MS \
    250u


/*
 * ============================================================
 * ARM MEMORY BARRIER
 * ============================================================
 */

static inline void surreal_boot_dmb(void)
{
    __asm volatile("dmb sy" ::: "memory");
}


/*
 * ============================================================
 * SRAM PAYLOAD BUFFERS
 * ============================================================
 *
 * Keep enough SRAM free for Pico runtime, USB state, stacks, etc.
 *
 * Two 192 KiB buffers give us:
 *
 *   192 KiB -> Core 0 decode
 *   192 KiB -> Core 1 USB transfer
 *
 * while the payload itself remains in XIP flash.
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
 * PAYLOAD FORMAT
 * ============================================================
 */

#define FLASH_PAYLOAD_MAGIC \
    (0x53425054u) /* SBPT */

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
 * DFU STATE
 * ============================================================
 */

static uint32_t working_transfer_size =
    INITIAL_TRANSFER_SIZE;


/*
 * ============================================================
 * CORE 0 DECOMPRESSION JOB
 * ============================================================
 *
 * Core 1:
 *     USB / DFU
 *
 * Core 0:
 *     LZ4
 *
 * Core 0 is the main Pico core.
 * Core 1 is owned by usb_task().
 */

typedef struct {
    const uint8_t *src;
    uint32_t src_size;

    uint8_t *dst;
    uint32_t dst_capacity;

    uint32_t expected_size;
} decompression_job_t;

static volatile decompression_job_t core0_job;

static volatile bool core0_job_pending =
    false;

static volatile bool core0_job_busy =
    false;

static volatile bool core0_job_done =
    false;

static volatile int core0_job_result =
    0;


/*
 * ============================================================
 * FAST LZ4
 * ============================================================
 *
 * The entire decoder executes from SRAM.
 *
 * Use O3 specifically for the hot decompressor even though the
 * whole firmware is built with -Os.
 */

static int __not_in_flash_func(read_len)(
    const uint8_t *src,
    uint32_t size,
    uint32_t *pos,
    uint32_t *len
) __attribute__((optimize("O3")));

static int __not_in_flash_func(lz4_decompress_block)(
    const uint8_t *src,
    uint32_t src_size,
    uint8_t *dst,
    uint32_t dst_capacity,
    uint32_t expected_size
) __attribute__((optimize("O3")));


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
 * Fast forward copy for non-overlapping data.
 */
static inline void __not_in_flash_func(copy_literals)(
    uint8_t *dst,
    const uint8_t *src,
    uint32_t len
) {
    while (len >= 4u) {

        uint32_t word;

        memcpy(
            &word,
            src,
            sizeof(word)
        );

        memcpy(
            dst,
            &word,
            sizeof(word)
        );

        src += 4;
        dst += 4;
        len -= 4;
    }

    while (len != 0u) {

        *dst++ =
            *src++;

        --len;
    }
}


/*
 * Fast overlapping LZ4 match copy.
 *
 * For offset >= 4, 32-bit copies are safe.
 * For very small offsets we keep the byte-at-a-time path because
 * the source overlaps the destination.
 */
static inline void __not_in_flash_func(copy_match)(
    uint8_t *dst,
    uint32_t dp,
    uint32_t offset,
    uint32_t match
) {
    uint32_t from =
        dp - offset;

    if (offset >= 4u) {

        while (match >= 4u) {

            uint32_t word;

            memcpy(
                &word,
                &dst[from],
                sizeof(word)
            );

            memcpy(
                &dst[dp],
                &word,
                sizeof(word)
            );

            from += 4;
            dp += 4;
            match -= 4;
        }
    }

    while (match != 0u) {

        dst[dp++] =
            dst[from++];

        --match;
    }
}


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

        if (
            read_len(
                src,
                src_size,
                &sp,
                &literals
            ) != 0
        ) {
            return -1;
        }

        if (
            literals >
            src_size - sp
        ) {
            return -1;
        }

        if (
            literals >
            dst_capacity - dp
        ) {
            return -1;
        }

        /*
         * Literal bytes.
         */
        copy_literals(
            &dst[dp],
            &src[sp],
            literals
        );

        dp +=
            literals;

        sp +=
            literals;

        /*
         * Last sequence can contain literals only.
         */
        if (sp == src_size) {
            break;
        }

        /*
         * Offset.
         */
        if (
            src_size - sp <
            2u
        ) {
            return -1;
        }

        uint32_t offset =
            (uint32_t)src[sp] |
            ((uint32_t)src[sp + 1] << 8);

        sp +=
            2u;

        if (
            offset == 0u ||
            offset > dp
        ) {
            return -1;
        }

        /*
         * Match length.
         */
        uint32_t match =
            (token & 0x0Fu) + 4u;

        if (
            (token & 0x0Fu) == 15u
        ) {

            uint32_t extra =
                15u;

            if (
                read_len(
                    src,
                    src_size,
                    &sp,
                    &extra
                ) != 0
            ) {
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

        copy_match(
            dst,
            dp,
            offset,
            match
        );

        dp +=
            match;
    }

    return
        dp == expected_size
        ? 0
        : -1;
}


/*
 * ============================================================
 * CORE 0 WORKER
 * ============================================================
 *
 * This is intentionally cooperative because Core 0 is also the
 * main application core. While Core 1 runs the USB worker,
 * Core 0 continuously enters this worker from usb.c.
 */

void surreal_boot_pump(void)
{
    if (!core0_job_pending) {
        return;
    }

    if (core0_job_busy) {
        return;
    }

    core0_job_busy =
        true;

    surreal_boot_dmb();

    INFO(
        "[LZ4] Core 0 decoding %lu -> %lu bytes",
        (unsigned long)core0_job.src_size,
        (unsigned long)core0_job.expected_size
    );

    uint64_t start =
        time_us_64();

    int rc =
        lz4_decompress_block(
            core0_job.src,
            core0_job.src_size,
            core0_job.dst,
            core0_job.dst_capacity,
            core0_job.expected_size
        );

    uint64_t elapsed =
        time_us_64() - start;

    core0_job_result =
        rc;

    surreal_boot_dmb();

    core0_job_busy =
        false;

    core0_job_pending =
        false;

    core0_job_done =
        true;

    if (rc == 0) {

        uint32_t kibps =
            elapsed > 0
            ? (uint32_t)(
                (
                    (uint64_t)
                        core0_job.expected_size *
                    1000000u
                ) /
                elapsed /
                1024u
            )
            : 0u;

        INFO(
            "[LZ4] Core 0 finished in %lu us (%u KiB/s)",
            (unsigned long)elapsed,
            (unsigned)kibps
        );

    } else {

        INFO(
            "[LZ4] Core 0 FAILED rc=%d in %lu us",
            rc,
            (unsigned long)elapsed
        );
    }
}


/*
 * ============================================================
 * SUBMIT DECOMPRESSION
 * ============================================================
 */

static void submit_decompression(
    const uint8_t *src,
    uint32_t src_size,
    uint8_t *dst,
    uint32_t dst_capacity,
    uint32_t expected_size
) {
    while (
        core0_job_pending ||
        core0_job_busy
    ) {
        tight_loop_contents();
    }

    core0_job.src =
        src;

    core0_job.src_size =
        src_size;

    core0_job.dst =
        dst;

    core0_job.dst_capacity =
        dst_capacity;

    core0_job.expected_size =
        expected_size;

    core0_job_result =
        0;

    core0_job_done =
        false;

    surreal_boot_dmb();

    core0_job_pending =
        true;

    surreal_boot_dmb();
}


/*
 * ============================================================
 * WAIT FOR DECOMPRESSION
 * ============================================================
 */

static int wait_decompression(void)
{
    while (!core0_job_done) {

        /*
         * Core 1 normally calls this function while Core 0 is
         * running the worker. This path is retained for safety
         * if the call ever happens on Core 0.
         */
        surreal_boot_pump();

        tight_loop_contents();
    }

    surreal_boot_dmb();

    return core0_job_result;
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
            "[DFU] FAILED rc=%d offset=0x%08lx len=0x%04x",
            rc,
            (unsigned long)offset,
            (unsigned)len
        );

    } else {

        INFO(
            "[DFU] sent offset=0x%08lx len=0x%04x",
            (unsigned long)offset,
            (unsigned)len
        );
    }

    return rc;
}


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

        INFO(
            "[DFU] transfer failed at size 0x%lx rc=%d",
            (unsigned long)
                working_transfer_size,
            rc
        );

        if (
            working_transfer_size <=
            MIN_TRANSFER_SIZE
        ) {

            INFO(
                "[DFU] minimum transfer size "
                "0x%lx also failed",
                (unsigned long)
                    working_transfer_size
            );

            return rc;
        }

        /*
         * Re-open EP0 before retrying.
         */
        INFO(
            "[DFU] resetting USB bus before retry"
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

    return bus_control_xfer(
        b,
        (const uint8_t *)&req,
        NULL,
        0,
        false,
        CTRL_TIMEOUT_MS
    );
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

    return bus_control_xfer(
        b,
        (const uint8_t *)&req,
        NULL,
        0,
        false,
        CTRL_TIMEOUT_MS
    );
}


/*
 * ============================================================
 * PAYLOAD DISCOVERY
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
        i <
        sizeof(offsets) /
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
 * SEND BUFFER
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
 * BOOT PIPELINE
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
            "[BOOT] no payload found"
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
        "[BOOT] decompressor: Core 0"
    );

    INFO(
        "[BOOT] USB host: Core 1"
    );

    INFO(
        "[BOOT] CPU/USB pipeline ENABLED"
    );

    INFO(
        "[BOOT] ========================================"
    );


    /*
     * ========================================================
     * DOUBLE BUFFER PIPELINE
     * ========================================================
     */

    uint8_t *buffers[2] = {
        boot_buffer_a,
        boot_buffer_b
    };

    uint8_t current_buffer =
        0;

    uint8_t next_buffer =
        1;

    uint16_t current_block =
        0;

    const uint8_t *flash_cursor =
        fblob_ptr;


    /*
     * ========================================================
     * FIRST BLOCK
     * ========================================================
     */

    if (
        current_block >=
        chunk_count
    ) {
        return -1;
    }

    if (have_flash) {

        uint32_t size =
            fchunks[current_block]
                .uncompressed_size;

        if (
            size >
            MAX_BLOCK
        ) {

            INFO(
                "[BOOT] block %u too large: "
                "%lu > %lu",
                (unsigned)current_block,
                (unsigned long)size,
                (unsigned long)MAX_BLOCK
            );

            return -1;
        }

        INFO(
            "[BOOT] preparing first block %u",
            (unsigned)current_block
        );

        submit_decompression(
            flash_cursor,
            fchunks[current_block]
                .compressed_size,
            buffers[current_buffer],
            MAX_BLOCK,
            size
        );

        flash_cursor +=
            fchunks[current_block]
                .compressed_size;

    }

#if BOOTFILE_COUNT > 0

    else {

        const struct bootfile_chunk *chunk =
            &bootfiles[0]
                .chunks[current_block];

        if (
            chunk->uncompressed_size >
            MAX_BLOCK
        ) {
            return -1;
        }

        submit_decompression(
            chunk->start,
            chunk->compressed_size,
            buffers[current_buffer],
            MAX_BLOCK,
            chunk->uncompressed_size
        );
    }

#endif


    /*
     * Core 0 decompresses the first block.
     */
    if (
        wait_decompression() !=
        0
    ) {

        INFO(
            "[BOOT] first block decompression failed"
        );

        return -1;
    }

    INFO(
        "[BOOT] first block ready, "
        "starting USB transfer"
    );


    /*
     * ========================================================
     * MAIN PIPELINE
     * ========================================================
     */

    while (
        current_block <
        chunk_count
    ) {

        uint32_t current_size =
            0;

        if (have_flash) {

            current_size =
                fchunks[current_block]
                    .uncompressed_size;
        }

#if BOOTFILE_COUNT > 0

        if (!have_flash) {

            current_size =
                bootfiles[0]
                    .chunks[current_block]
                    .uncompressed_size;
        }

#endif

        bool next_submitted =
            false;

        uint16_t next_block =
            current_block + 1u;


        /*
         * Submit the next decode job BEFORE USB transfer.
         */
        if (
            next_block <
            chunk_count
        ) {

            if (have_flash) {

                uint32_t next_size =
                    fchunks[next_block]
                        .uncompressed_size;

                if (
                    next_size >
                    MAX_BLOCK
                ) {

                    INFO(
                        "[BOOT] block %u too large",
                        (unsigned)next_block
                    );

                    return -1;
                }

                submit_decompression(
                    flash_cursor,
                    fchunks[next_block]
                        .compressed_size,
                    buffers[next_buffer],
                    MAX_BLOCK,
                    next_size
                );

                flash_cursor +=
                    fchunks[next_block]
                        .compressed_size;

                next_submitted =
                    true;
            }

#if BOOTFILE_COUNT > 0

            else {

                const struct bootfile_chunk *chunk =
                    &bootfiles[0]
                        .chunks[next_block];

                if (
                    chunk->uncompressed_size >
                    MAX_BLOCK
                ) {
                    return -1;
                }

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
         * Core 1 sends current buffer while Core 0 decodes
         * next_buffer.
         */
        int rc =
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

        if (rc != 0) {
            return rc;
        }


        /*
         * Wait for the next decode only after USB had a chance
         * to run in parallel.
         */
        if (next_submitted) {

            if (
                wait_decompression() !=
                0
            ) {

                INFO(
                    "[LZ4] decode FAILED "
                    "block=%u rc=%d",
                    (unsigned)next_block,
                    core0_job_result
                );

                return -1;
            }
        }


        current_block =
            next_block;

        uint8_t tmp =
            current_buffer;

        current_buffer =
            next_buffer;

        next_buffer =
            tmp;
    }


    /*
     * ========================================================
     * VALIDATION
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
     * BENCHMARK
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

    INFO("");

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
     * DFU FINISH
     * ========================================================
     */

    int finish_rc =
        dfu_download_finish(b);

    INFO(
        "[BOOT] DFU termination rc=%d",
        finish_rc
    );

    if (
        finish_rc !=
        0
    ) {
        return finish_rc;
    }


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


    /*
     * ========================================================
     * SUCCESS
     * ========================================================
     */

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
        "[SUCCESS] Took %lu ms to transfer",
        (unsigned long)
            elapsed_ms
    );

    INFO(
        "[SUCCESS] Average speed: %u KiB/s",
        (unsigned)kibps
    );

    INFO(
        "[SUCCESS] Device should boot now!"
    );

    INFO(
        "[SUCCESS] ========================================"
    );

    return 0;
}


/*
 * ============================================================
 * PUBLIC API
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
