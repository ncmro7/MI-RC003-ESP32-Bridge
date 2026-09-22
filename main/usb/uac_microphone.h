#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Register the custom UAC 1.0 microphone class driver. */
bool uac_microphone_init(void);

/** @brief Feed audio is driven by the TinyUSB transfer callback; no polling needed. */
void uac_microphone_task(void);

/** @brief True while the host has selected the streaming alternate setting. */
bool uac_microphone_is_streaming(void);

/** @brief Stop submissions before USB suspend, detach, or re-enumeration. */
void uac_microphone_stop_usb_stream(void);

#ifdef __cplusplus
}
#endif
