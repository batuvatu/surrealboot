#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Check for incoming Web Serial flash commands on USB CDC.
 * Returns true if a payload was flashed (caller should reboot), false otherwise.
 */
bool serial_flasher_check(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
