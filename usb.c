#include <stdint.h>

#include "pico/multicore.h"
#include "pico/time.h"

#include "usb.h"
#include "bus.h"
#include "log.h"
#include "surreal_boot.h"


enum {
    USB_CMD_BUS_INIT = 0,
    USB_CMD_WAIT_FOR_DEVICE,
    USB_CMD_RESET_OPEN_EP0,
    USB_CMD_EXECUTE_FUNC
};


static struct {
    usb_executee_t func;
    void *ctx;
} usb_exec_ctx;


static bus_t gBus = {
    0
};


/*
 * ============================================================
 * USB CORE 1 TASK
 * ============================================================
 */

void usb_task(void)
{
    while (1) {

        uint32_t cmd =
            multicore_fifo_pop_blocking();

        uint32_t ret =
            (uint32_t)-1;

        switch (cmd) {

            case USB_CMD_BUS_INIT: {

                bus_init(
                    &gBus,
                    false
                );

                ret = 0;
                break;
            }


            case USB_CMD_WAIT_FOR_DEVICE: {

                bus_wait_for_connect(
                    &gBus
                );

                ret = 0;
                break;
            }


            case USB_CMD_RESET_OPEN_EP0: {

                bus_reset_ep0_reopen(
                    &gBus
                );

                ret = 0;
                break;
            }


            case USB_CMD_EXECUTE_FUNC: {

                ret =
                    (uint32_t)
                    usb_exec_ctx.func(
                        &gBus,
                        usb_exec_ctx.ctx
                    );

                break;
            }


            default:
                break;
        }

        multicore_fifo_push_blocking(
            ret
        );
    }
}


/*
 * ============================================================
 * USB CORE START
 * ============================================================
 *
 * Core 1 remains dedicated to USB.
 *
 * Core 0 stays available for the decompression pump.
 */

void usb_start(void)
{
    multicore_reset_core1();

    multicore_launch_core1(
        usb_task
    );
}


/*
 * ============================================================
 * COMMAND EXECUTION
 * ============================================================
 *
 * While Core 0 waits for a USB command result, it also services
 * the payload decompressor.
 *
 * This is the key piece that allows:
 *
 *   Core 0 -> LZ4
 *   Core 1 -> USB
 *
 * simultaneously.
 */

static int _usb_task_execute_cmd(
    int cmd,
    uint64_t timeout
) {
    multicore_fifo_push_blocking(
        (uint32_t)cmd
    );

    uint64_t start =
        time_us_64();

    while (
        !multicore_fifo_rvalid()
    ) {

        /*
         * Run pending LZ4 work.
         */
        surreal_boot_pump();

        /*
         * Timeout support.
         */
        if (timeout != 0) {

            uint64_t elapsed =
                time_us_64() -
                start;

            if (elapsed >= timeout) {

                INFO(
                    "TIMEOUT"
                );

                return -1;
            }
        }

        tight_loop_contents();
    }

    uint32_t out =
        multicore_fifo_pop_blocking();

    return (int)out;
}


#define DEFAULT_TIMEOUT_US \
    (100u * 1000u)


int usb_bus_init(void)
{
    return _usb_task_execute_cmd(
        USB_CMD_BUS_INIT,
        DEFAULT_TIMEOUT_US
    );
}


int usb_bus_wait_for_device(void)
{
    INFO(
        "waiting for device..."
    );

    int ret =
        _usb_task_execute_cmd(
            USB_CMD_WAIT_FOR_DEVICE,
            0
        );

    if (ret == 0) {

        INFO(
            "connected, speed = %s",
            gBus.root->is_fullspeed
                ? "FS"
                : "LS"
        );
    }

    return ret;
}


int usb_bus_reset_open_ep0(void)
{
    int ret =
        _usb_task_execute_cmd(
            USB_CMD_RESET_OPEN_EP0,
            DEFAULT_TIMEOUT_US
        );

    if (ret == 0) {

        INFO(
            "opened EP0"
        );
    }

    return ret;
}


int usb_bus_execute(
    usb_executee_t func,
    void *ctx,
    uint64_t timeout
) {
    usb_exec_ctx.func =
        func;

    usb_exec_ctx.ctx =
        ctx;

    return _usb_task_execute_cmd(
        USB_CMD_EXECUTE_FUNC,
        timeout
    );
}


/*
 * ============================================================
 * CONNECTION STATE
 * ============================================================
 *
 * Kept for compatibility with code that wants to inspect the
 * root USB port without modifying the existing bus abstraction.
 */

bool usb_bus_is_connected(void)
{
    return (
        gBus.root != NULL &&
        gBus.root->connected
    );
}
