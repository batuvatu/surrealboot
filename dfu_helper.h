#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DFU Helper — guides the user from Recovery Mode into DFU Mode
 * using LED colour cues and timed USB commands.
 *
 * Call from main.c before the exploit loop.
 *
 * Returns:
 *   0  — device is now in DFU mode (either was already or guided in)
 *  <0  — error / timeout
 */
int dfu_helper_run(void);

#ifdef __cplusplus
}
#endif
