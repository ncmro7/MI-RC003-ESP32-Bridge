#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "keymap/key_state_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Initialize the HID bridge (mutex + initial release). */
void hid_bridge_init(void);

/** @brief Allow or block reports while the USB data endpoints are recovering. */
void hid_bridge_set_transport_enabled(bool enabled);

bool usb_hid_keyboard_press(uint8_t modifier, uint8_t keycode);
bool usb_hid_keyboard_release(void);
bool usb_hid_keyboard_tap(uint8_t modifier, uint8_t keycode);

bool usb_hid_consumer_press(uint16_t usage_code);
bool usb_hid_consumer_release(void);
bool usb_hid_consumer_tap(uint16_t usage_code);

/** @brief Mouse button control. @p button_mask is a USB_MOUSE_BTN_* bitmask. */
bool usb_hid_mouse_button_press(uint8_t button_mask);
bool usb_hid_mouse_button_release(uint8_t button_mask);
bool usb_hid_mouse_buttons_release(void);

/** @brief Relative cursor movement and wheel scroll. */
bool usb_hid_mouse_move(int8_t dx, int8_t dy);
bool usb_hid_mouse_wheel(int8_t wheel);

/**
 * @brief Virtual Xbox-layout gamepad control.
 *
 * @p control is a `gamepad_control_t`; @p value is the analog magnitude
 * (0 = use the control's default). Press/release maintain the accumulated
 * control set so several directions can combine (e.g. a stick diagonal).
 */
bool usb_hid_gamepad_press(uint8_t control, uint8_t value);
bool usb_hid_gamepad_release(uint8_t control);
bool usb_hid_gamepad_tap(uint8_t control, uint8_t value);
/** @brief Neutralize all gamepad controls and send a centered report. */
bool usb_hid_gamepad_release_all(void);

/** @brief Dispatch a high-level key action produced by the key engine. */
void usb_hid_dispatch_action(const key_action_t *action);

#ifdef __cplusplus
}
#endif
