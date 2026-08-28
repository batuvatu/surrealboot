/*
 * dfu_helper.c — Recovery Mode → DFU Mode guided entry
 *
 * When the Pico detects an Apple device in Recovery Mode (PID 0x1280–0x1283),
 * it guides the user through DFU entry using LED colours:
 *
 *   MAGENTA (purple) = "Hold Volume Down + Power"
 *   CYAN             = "Release Power, keep holding Volume Down"
 *   WHITE blink      = waiting for DFU
 *
 * The timing matches the proven dfu.sh script from surrealra1n:
 *
 *   3-2-1 countdown (prep)
 *   "Hold both buttons"
 *   4-3-[send reboot cmd]-2-1
 *   "Release power"
 *   8-7-6-5-4-3-2-1
 *   Check for DFU
 *
 * CRITICAL: The reboot command is sent SYNCHRONOUSLY (blocking) between
 * countdown ticks 3 and 2. This is NOT a bug — the original dfu.sh does
 * exactly this (./bin/irecovery -n blocks the shell), and the precise
 * timing is required to hit the DFU entry window.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "pico/time.h"

#include "bus.h"
#include "usb.h"
#include "log.h"
#include "led.h"
#include "dfu_helper.h"

/* Apple USB identifiers */
#define APPLE_VID          0x05AC
#define APPLE_PID_DFU      0x1227
#define APPLE_PID_RECOV_LO 0x1280
#define APPLE_PID_RECOV_HI 0x1283

/* How long to wait for DFU after the guided sequence (seconds) */
#define DFU_WAIT_TIMEOUT_SEC  15

/* How long to wait after seeing Recovery before starting (ms) */
#define RECOVERY_SETTLE_MS    3000

/* USB structures */
struct usb_setup_req_header {
    uint8_t  bmRequestType;
    uint8_t  bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} __attribute__((packed));

struct usb_device_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} __attribute__((packed));

/*
 * Detect what kind of Apple device is connected.
 *
 * Returns:
 *   1  — Recovery mode
 *   0  — DFU mode
 *  -1  — not an Apple device or error
 *  -2  — no device / descriptor read failed
 */
static int detect_apple_device_internal(bus_t *b, void *ctx) {
    (void)ctx;

    struct usb_device_descriptor dev_desc = { 0 };

    struct usb_setup_req_header req = {
        .bmRequestType = 0x80,
        .bRequest      = 0x06,
        .wValue        = 0x0100,
        .wIndex        = 0x0000,
        .wLength       = sizeof(dev_desc)
    };

    int rc = bus_control_xfer(
        b,
        (const uint8_t *)&req,
        (uint8_t *)&dev_desc,
        sizeof(dev_desc),
        true,
        200
    );

    if (rc != 0) {
        return -2;
    }

    if (dev_desc.idVendor != APPLE_VID) {
        return -1;
    }

    if (dev_desc.idProduct == APPLE_PID_DFU) {
        INFO("[DFU-HELPER] Apple DFU device detected (PID=0x%04x)",
             dev_desc.idProduct);
        return 0;
    }

    if (dev_desc.idProduct >= APPLE_PID_RECOV_LO &&
        dev_desc.idProduct <= APPLE_PID_RECOV_HI) {
        INFO("[DFU-HELPER] Apple Recovery device detected (PID=0x%04x)",
             dev_desc.idProduct);
        return 1;
    }

    INFO("[DFU-HELPER] Apple device PID=0x%04x not DFU/Recovery",
         dev_desc.idProduct);
    return -1;
}

static int detect_apple_device(void) {
    return usb_bus_execute(detect_apple_device_internal, NULL, 200 * 1000);
}

/*
 * Recovery mode "reboot" command
 * Equivalent to `irecovery -n`:
 *   bmRequestType = 0x40 (vendor, host-to-device)
 *   bRequest      = 0x00
 *   wValue/wIndex = 0
 *   data          = "reboot\n"
 */
static int send_reboot_internal(bus_t *b, void *ctx) {
    (void)ctx;

    static const char cmd[] = "reboot\n";

    struct usb_setup_req_header req = {
        .bmRequestType = 0x40,
        .bRequest      = 0x00,
        .wValue        = 0x0000,
        .wIndex        = 0x0000,
        .wLength       = sizeof(cmd) - 1,
    };

    int rc = bus_control_xfer(
        b,
        (const uint8_t *)&req,
        (uint8_t *)cmd,
        sizeof(cmd) - 1,
        false,
        1000
    );

    INFO("[DFU-HELPER] reboot command sent, rc=%d", rc);
    return 0;  /* don't fail even if rc != 0; device may disconnect immediately */
}

static void send_recovery_reboot(void) {
    usb_bus_execute(send_reboot_internal, NULL, 2000 * 1000);
}

/*
 * DFU entry guided sequence
 */
int dfu_helper_run(void)
{
    INFO("[DFU-HELPER] Recovery mode detected, starting DFU entry guide");

    /* Settle time */
    INFO("[DFU-HELPER] waiting %d ms for device to settle...", RECOVERY_SETTLE_MS);
    sleep_ms(RECOVERY_SETTLE_MS);

    /* Step 3: Preparation countdown (3-2-1) */
    INFO("[DFU-HELPER] get ready...");
    INFO("[DFU-HELPER] 3"); sleep_ms(1000);
    INFO("[DFU-HELPER] 2"); sleep_ms(1000);
    INFO("[DFU-HELPER] 1"); sleep_ms(1000);

    /* Step 4: MAGENTA LED = "Hold Volume Down + Power" */
    led_set_state(LED_STATE_DFU_HOLD_BUTTONS);
    INFO("[DFU-HELPER] >>> HOLD Volume Down + Power buttons <<<");
    INFO("[DFU-HELPER] 4"); sleep_ms(1000);
    INFO("[DFU-HELPER] 3"); sleep_ms(1000);

    /* Send reboot command — THIS BLOCKS (matches irecovery -n behavior) */
    INFO("[DFU-HELPER] sending reboot command to Recovery...");
    usb_bus_reset_open_ep0();
    send_recovery_reboot();

    INFO("[DFU-HELPER] 2"); sleep_ms(1000);
    INFO("[DFU-HELPER] 1"); sleep_ms(1000);

    /* Step 5: CYAN LED = "Release Power, keep holding Volume Down" */
    led_set_state(LED_STATE_DFU_RELEASE_POWER);
    INFO("[DFU-HELPER] >>> Release Power, KEEP holding Volume Down <<<");
    INFO("[DFU-HELPER] 8"); sleep_ms(1000);
    INFO("[DFU-HELPER] 7"); sleep_ms(1000);
    INFO("[DFU-HELPER] 6"); sleep_ms(1000);
    INFO("[DFU-HELPER] 5"); sleep_ms(1000);
    INFO("[DFU-HELPER] 4"); sleep_ms(1000);
    INFO("[DFU-HELPER] 3"); sleep_ms(1000);
    INFO("[DFU-HELPER] 2"); sleep_ms(1000);
    INFO("[DFU-HELPER] 1"); sleep_ms(1000);

    /* Step 6: WHITE blinking = waiting for DFU */
    led_set_state(LED_STATE_DFU_WAITING);
    INFO("[DFU-HELPER] waiting for device to enter DFU mode...");

    int mode = -2;
    for (int i = 0; i < DFU_WAIT_TIMEOUT_SEC; i++) {
        usb_bus_reset_open_ep0();
        mode = detect_apple_device();
        if (mode == 0) {
            INFO("[DFU-HELPER] device entered DFU successfully!");
            led_set_state(LED_STATE_IDLE);
            return 0;
        }
        sleep_ms(1000);
    }

    INFO("[DFU-HELPER] timed out waiting for DFU");
    led_set_state(LED_STATE_IDLE);
    return -1;
}
