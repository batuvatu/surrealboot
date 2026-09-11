
#!/usr/bin/env python3
from pathlib import Path
import sys

root = Path(sys.argv[1])

# 156 MHz during exploit.
main = root / "main.c"
s = main.read_text()

old_clock = r'''    // PIO USB needs sys_clk to be a multiple of 12 MHz
#if PICO_RP2350
    set_sys_clock_khz(156000, true);
#elif PICO_RP2040
    set_sys_clock_khz(120000, true);
#else
#error What is this MCU even?
#endif
'''
new_clock = r'''    // Exploit stage must remain at 156 MHz.
#if PICO_RP2350
#ifndef SURREALBOOT_EXPLOIT_CLOCK_KHZ
#define SURREALBOOT_EXPLOIT_CLOCK_KHZ 156000
#endif
    set_sys_clock_khz(SURREALBOOT_EXPLOIT_CLOCK_KHZ, true);
#elif PICO_RP2040
    set_sys_clock_khz(120000, true);
#else
#error What is this MCU even?
#endif
'''
if old_clock in s:
    s = s.replace(old_clock, new_clock, 1)
elif new_clock not in s:
    raise SystemExit("main.c exploit clock block not found")

marker = r'''        led_set_state(
            LED_STATE_BOOT_PAYLOAD
        );
'''
transition = r'''        /* Switch to 220 MHz only after exploit success. */
#if PICO_RP2350
        if (usb_bus_set_payload_clock(220000) != 0) {
            printf("[BOOT] failed to switch payload clock to 220 MHz\n");
            led_set_state(LED_STATE_ERROR);
            fatal_failure();
        }
#endif

'''
if transition not in s:
    if marker not in s:
        raise SystemExit("main.c payload insertion point not found")
    s = s.replace(marker, transition + marker, 1)
main.write_text(s)

# Runtime clock transition API.
usb_h = root / "usb.h"
ht = usb_h.read_text()
decl = "int usb_bus_set_payload_clock(uint32_t khz);\n"
if decl not in ht:
    usb_h.write_text(ht + "\n" + decl)

usb_c = root / "usb.c"
ct = usb_c.read_text()
if "int usb_bus_set_payload_clock(uint32_t khz)" not in ct:
    include = '#include "usb.h"\n'
    extra = '#include "usb.h"\n#include "hardware/clocks.h"\n#include "pio_usb_host.h"\n'
    if include not in ct:
        raise SystemExit("usb.c: usb.h include not found")
    ct = ct.replace(include, extra, 1)
    ct += r'''
int usb_bus_set_payload_clock(uint32_t khz)
{
#if PICO_RP2350
    if (khz == 0) {
        return -1;
    }

    pio_usb_host_stop();

    if (!set_sys_clock_khz(khz, true)) {
        pio_usb_host_restart();
        return -1;
    }

    if (!pio_usb_host_reclock()) {
        pio_usb_host_restart();
        return -1;
    }

    pio_usb_host_restart();

    INFO("[USB] payload clock switched to %lu MHz",
         (unsigned long)(khz / 1000u));

    return 0;
#else
    (void)khz;
    return 0;
#endif
}
'''
    usb_c.write_text(ct)

# PIO USB reclock API.
pio_h = root / "pio_usb/src/pio_usb_host.h"
pht = pio_h.read_text()
if "bool pio_usb_host_reclock(void);" not in pht:
    pio_h.write_text(pht + "\nbool pio_usb_host_reclock(void);\n")

pio_c = root / "pio_usb/src/pio_usb_host.c"
pct = pio_c.read_text()
if "bool pio_usb_host_reclock(void)" not in pct:
    pct += r'''
bool pio_usb_host_reclock(void)
{
    pio_port_t *pp = PIO_USB_PIO_PORT(0);
    root_port_t *root = PIO_USB_ROOT_PORT(0);

    uint32_t const cpu_khz = clock_get_hz(clk_sys) / 1000u;
    float const cpu_freq = (float)cpu_khz * 1000.0f;

    pio_calculate_clkdiv_from_float(cpu_freq / 48000000.0f,
                                    &pp->clk_div_fs_tx.div_int,
                                    &pp->clk_div_fs_tx.div_frac);
    pio_calculate_clkdiv_from_float(cpu_freq / 6000000.0f,
                                    &pp->clk_div_ls_tx.div_int,
                                    &pp->clk_div_ls_tx.div_frac);
    pio_calculate_clkdiv_from_float(cpu_freq / 96000000.0f,
                                    &pp->clk_div_fs_rx.div_int,
                                    &pp->clk_div_fs_rx.div_frac);
    pio_calculate_clkdiv_from_float(cpu_freq / 12000000.0f,
                                    &pp->clk_div_ls_rx.div_int,
                                    &pp->clk_div_ls_rx.div_frac);

    if (!root->initialized || !root->connected) {
        return false;
    }

    configure_root_port(pp, root);
    return true;
}
'''
    pio_c.write_text(pct)

print("dual-clock patch applied")
