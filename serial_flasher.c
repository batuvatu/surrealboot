/*
 * serial_flasher.c — Sector-by-sector Web Serial payload flasher
 *
 * Flashes iBSS.boot payload directly over USB CDC serial (Chrome/Edge).
 *
 * Flashes in 4KB sectors on-demand:
 * 1. Reads 4KB from USB CDC while interrupts are fully enabled.
 * 2. Erases & programs only that 4KB sector (~35ms disable_interrupts).
 * 3. Restores interrupts immediately and sends 'K\n' ACK.
 *
 * This prevents USB pipe stalls / timeout disconnects.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdio.h"
#include "pico/time.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "led.h"
#include "log.h"
#include "serial_flasher.h"

#define FLASH_PAYLOAD_OFFSET_RAW  (0x20000u)      /* 128KB into flash */
#define MAX_PAYLOAD_SIZE          (3800u * 1024u) /* 3.8MB max (supports up to 4MB flash) */
#define SECTOR_BUFFER_SIZE        (4096u)

static uint8_t sector_buf[SECTOR_BUFFER_SIZE] __attribute__((aligned(4)));

static bool read_exact(uint8_t *buf, size_t len, uint32_t timeout_ms_total) {
    size_t read_bytes = 0;
    uint64_t start_us = time_us_64();
    uint64_t timeout_us = (uint64_t)timeout_ms_total * 1000u;

    while (read_bytes < len) {
        int c = getchar_timeout_us(5000); /* 5ms poll */
        if (c >= 0) {
            buf[read_bytes++] = (uint8_t)c;
            start_us = time_us_64(); /* Reset timer on each received byte */
        } else {
            if (time_us_64() - start_us > timeout_us) {
                return false;
            }
        }
    }
    return true;
}

bool serial_flasher_check(uint32_t timeout_ms) {
    int c = getchar_timeout_us(timeout_ms * 1000u);
    if (c < 0) {
        return false;
    }

    /* Check for magic handshake trigger byte 'S' or 'F' */
    if (c != 'S' && c != 'F') {
        return false;
    }

    char cmd_buf[16] = { (char)c, 0 };
    size_t idx = 1;

    while (idx < sizeof(cmd_buf) - 1) {
        int next = getchar_timeout_us(50000); // 50ms timeout
        if (next < 0 || next == '\n' || next == '\r') {
            break;
        }
        cmd_buf[idx++] = (char)next;
    }
    cmd_buf[idx] = '\0';

    if (strcmp(cmd_buf, "SBPT_FLASH") != 0 && strcmp(cmd_buf, "S") != 0 && strcmp(cmd_buf, "FLASH") != 0) {
        return false;
    }

    /* Stop Core 1 so it doesn't execute from flash while we erase/program */
    multicore_reset_core1();

    INFO("[SERIAL-FLASHER] Web Serial handshake received!");
    printf("SBPT_FLASH_READY\n");

    /* Read 4-byte payload length */
    uint32_t payload_len = 0;
    if (!read_exact((uint8_t *)&payload_len, 4, 3000)) {
        INFO("[SERIAL-FLASHER] timeout reading payload length");
        printf("SBPT_ERR_TIMEOUT\n");
        return false;
    }

    if (payload_len == 0 || payload_len > MAX_PAYLOAD_SIZE) {
        INFO("[SERIAL-FLASHER] invalid payload length: %lu (max: %lu)",
             (unsigned long)payload_len, (unsigned long)MAX_PAYLOAD_SIZE);
        printf("SBPT_ERR_SIZE\n");
        return false;
    }

    INFO("[SERIAL-FLASHER] receiving %lu bytes in 4KB sectors...", (unsigned long)payload_len);
    printf("SBPT_ACK_LEN\n");

    /* Stream and flash 4KB sector-by-sector */
    uint32_t written = 0;

    while (written < payload_len) {
        uint32_t block_len = payload_len - written;
        if (block_len > SECTOR_BUFFER_SIZE) {
            block_len = SECTOR_BUFFER_SIZE;
        }

        memset(sector_buf, 0xFF, sizeof(sector_buf));

        /* Read next 4KB from serial while USB interrupts are fully active */
        if (!read_exact(sector_buf, block_len, 6000)) {
            INFO("[SERIAL-FLASHER] timeout reading sector at offset %lu", (unsigned long)written);
            printf("SBPT_ERR_DATA\n");
            return false;
        }

        /* Erase & program only this 4KB sector (~35ms max) */
        uint32_t irq = save_and_disable_interrupts();
        flash_range_erase(FLASH_PAYLOAD_OFFSET_RAW + written, FLASH_SECTOR_SIZE);
        flash_range_program(FLASH_PAYLOAD_OFFSET_RAW + written, sector_buf, FLASH_SECTOR_SIZE);
        restore_interrupts(irq);

        written += block_len;

        /* Send ACK to browser to request next sector */
        printf("K\n");
    }

    /* Verify magic at 0x10020000 */
    const uint32_t *magic_check = (const uint32_t *)(0x10000000u + FLASH_PAYLOAD_OFFSET_RAW);
    if (*magic_check != 0x53425054u) { /* "SBPT" */
        INFO("[SERIAL-FLASHER] verification failed: magic is 0x%08lx (expected 0x53425054)",
             (unsigned long)*magic_check);
        printf("SBPT_ERR_VERIFY\n");
        return false;
    }

    INFO("[SERIAL-FLASHER] payload flashed and verified successfully!");
    printf("SBPT_FLASH_OK\n");

    /* Show solid green LED to signal successful payload flash */
    led_set_state(LED_STATE_SUCCESS);

    sleep_ms(2000);
    watchdog_reboot(0, SRAM_END, 1);
    while (1) {}

    return true;
}
