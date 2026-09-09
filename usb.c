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
 *
 * Core 1 is permanently dedicated to USB.
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

                INFO(
                    "[USB] Core 1: bus init"
                );

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

                INFO(
                    "[USB] Core 1: executing USB operation"
                );

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
 */

void usb_start(void)
{
    multicore_reset_core1();

    multicore_launch_core1(
        usb_task
    );

    INFO(
        "[USB] Core 1 USB worker started"
    );
}


/*
 * ============================================================
 * COMMAND EXECUTION
 * ============================================================
 *
 * Core 1 executes USB.
 *
 * Core 0 stays alive here and services LZ4 through
 * surreal_boot_pump().
 */

static int _usb_task_execute_cmd(
    int cmd,
    uint64_t timeout
) {
    /*
     * Flush any stale FIFO result before issuing a new command.
     */
    while (
        multicore_fifo_rvalid()
    ) {
        (void)
            multicore_fifo_pop_blocking();
    }

    multicore_fifo_push_blocking(
        (uint32_t)cmd
    );

    uint64_t start =
        time_us_64();

    while (
        !multicore_fifo_rvalid()
    ) {

        /*
         * IMPORTANT:
         *
         * Core 0 is the decompression CPU.
         * Never sleep here while a payload job is pending.
         */
        surreal_boot_pump();

        if (
            timeout != 0
        ) {

            uint64_t elapsed =
                time_us_64() -
                start;

            if (
                elapsed >=
                timeout
            ) {

                INFO(
                    "[USB] command timeout"
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


/*
 * Long-running operations deliberately have no timeout because
 * Apple DFU enumeration and payload transfers can exceed the
 * short command timeout used by initialization operations.
 */

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

    if (
        ret == 0
    ) {

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

    if (
        ret == 0
    ) {

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
 */

bool usb_bus_is_connected(void)
{
    return (
        gBus.root != NULL &&
        gBus.root->connected
    );
}
