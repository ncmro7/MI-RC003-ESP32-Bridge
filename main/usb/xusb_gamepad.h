#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

// Size of the default XUSB game controller input report (MS-XUSBI Table 52).
#define XUSB_REPORT_LEN 20

// bmButtons bits (MS-XUSBI Table 53).
#define XUSB_BTN_DPAD_UP    (1u << 0)
#define XUSB_BTN_DPAD_DOWN  (1u << 1)
#define XUSB_BTN_DPAD_LEFT  (1u << 2)
#define XUSB_BTN_DPAD_RIGHT (1u << 3)
#define XUSB_BTN_START      (1u << 4)
#define XUSB_BTN_BACK       (1u << 5)
#define XUSB_BTN_L3         (1u << 6)
#define XUSB_BTN_R3         (1u << 7)
#define XUSB_BTN_LB         (1u << 8)
#define XUSB_BTN_RB         (1u << 9)
#define XUSB_BTN_GUIDE      (1u << 10)
#define XUSB_BTN_BINDING    (1u << 11)
#define XUSB_BTN_A          (1u << 12)
#define XUSB_BTN_B          (1u << 13)
#define XUSB_BTN_X          (1u << 14)
#define XUSB_BTN_Y          (1u << 15)

// Default game controller input report (report ID 0x00), 20 bytes on the wire.
typedef struct __attribute__((packed)) {
    uint8_t  report_id;      // 0x00
    uint8_t  size;           // 0x14 (20)
    uint16_t buttons;        // bmButtons, little-endian
    uint8_t  left_trigger;   // 0..255
    uint8_t  right_trigger;  // 0..255
    int16_t  left_x;         // -32768..32767
    int16_t  left_y;
    int16_t  right_x;
    int16_t  right_y;
    uint8_t  reserved[6];    // must be zero
} xusb_gamepad_report_t;

/** @brief Reset internal state (call before installing the USB driver). */
void xusb_gamepad_init(void);

/** @brief True once the XUSB interface has been opened by the host. */
bool xusb_gamepad_mounted(void);

/** @brief Submit a game controller input report on the XUSB interrupt IN endpoint. */
bool xusb_gamepad_send(const xusb_gamepad_report_t *report);

/** @brief Re-submit the last report to keep the XInput stream alive. */
void xusb_gamepad_keepalive(void);

/**
 * @brief Handle vendor-type XUSB requests.
 *
 * The ESP-IDF TinyUSB fork routes every vendor-type control request to
 * tud_vendor_control_xfer_cb(), so GET_DEVICE_ID / SET_CONTROL are delegated
 * here. Returns true when the request was recognized and answered.
 */
bool xusb_gamepad_handle_vendor_request(uint8_t rhport,
                                        tusb_control_request_t const *request);

#ifdef __cplusplus
}
#endif
