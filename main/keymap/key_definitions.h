#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ==========================================
// 1. Xiaomi Remote Physical Key Codes
//    (canonical internal codes; HID usages and their alternates are normalized
//     to these by key_state_machine.c)
// ==========================================
#define MI_KEY_VOL_UP       0x80    // Volume Up
#define MI_KEY_VOL_DOWN     0x81    // Volume Down
#define MI_KEY_BACK         0xF1    // Back key
#define MI_KEY_POWER        0x66    // Power button
#define MI_KEY_POWER_ALT    0xFF
#define MI_KEY_HOME         0x24    // Home button
#define MI_KEY_HOME_ALT     0x4A    // HID usage 0x4A (RC003 HOGP report)
#define MI_KEY_MENU         0x5D    // Menu button
#define MI_KEY_MENU_ALT     0x65    // HID usage 0x65
#define MI_KEY_TV           0xC0    // TV button
#define MI_KEY_TV_ALT       0x35    // HID usage 0x35
#define MI_KEY_UP           0x52    // D-Pad Up
#define MI_KEY_DOWN         0x51    // D-Pad Down
#define MI_KEY_LEFT         0x50    // D-Pad Left
#define MI_KEY_RIGHT        0x4F    // D-Pad Right
#define MI_KEY_OK           0x28    // OK / Enter
#define MI_KEY_VOICE        0x04    // Voice button (ATVV)
#define MI_KEY_VOICE_ALT    0x3E    // Voice button (HID usage F5)

// ==========================================
// 2. USB HID Keyboard Modifier Bitmasks
// ==========================================
#define USB_MOD_NONE        0x00
#define USB_MOD_LCTRL       0x01
#define USB_MOD_LSHIFT      0x02
#define USB_MOD_LALT        0x04
#define USB_MOD_LGUI        0x08
#define USB_MOD_RCTRL       0x10
#define USB_MOD_RSHIFT      0x20
#define USB_MOD_RALT        0x40
#define USB_MOD_RGUI        0x80

// ==========================================
// 3. Standard USB HID Keyboard Key Codes
// ==========================================
#define USB_KEY_NONE        0x00
#define USB_KEY_A           0x04
#define USB_KEY_D           0x07    // 'D' (Win+D show desktop)
#define USB_KEY_H           0x0B    // 'H' (Win+H voice typing)
#define USB_KEY_RETURN      0x28    // Enter
#define USB_KEY_ESCAPE      0x29    // Esc
#define USB_KEY_BACKSPACE   0x2A
#define USB_KEY_TAB         0x2B    // Tab (Alt+Tab task switch)
#define USB_KEY_SPACE       0x2C    // Spacebar
#define USB_KEY_F5          0x3E    // F5 refresh
#define USB_KEY_F8          0x41    // F8 voice IME hotkey
#define USB_KEY_RIGHT       0x4F    // Arrow right
#define USB_KEY_LEFT        0x50    // Arrow left
#define USB_KEY_DOWN        0x51    // Arrow down
#define USB_KEY_UP          0x52    // Arrow up
#define USB_KEY_COMMA       0x36    // ',' (RAlt+, WeChat IME hotkey)

// ==========================================
// 4. USB HID Consumer Control Usages
// ==========================================
#define USB_CONSUMER_NONE           0x0000
#define USB_CONSUMER_POWER          0x0030
#define USB_CONSUMER_SLEEP          0x0032
#define USB_CONSUMER_PLAY_PAUSE     0x00CD
#define USB_CONSUMER_MUTE           0x00E2
#define USB_CONSUMER_VOLUME_UP      0x00E9
#define USB_CONSUMER_VOLUME_DOWN    0x00EA
#define USB_CONSUMER_NEXT_TRACK     0x00B5
#define USB_CONSUMER_PREV_TRACK     0x00B6
#define USB_CONSUMER_AC_HOME        0x0223
#define USB_CONSUMER_AC_BACK        0x0224

// ==========================================
// 5. USB HID Mouse Button Bitmasks
// ==========================================
#define USB_MOUSE_BTN_LEFT      0x01    // Left button
#define USB_MOUSE_BTN_RIGHT     0x02    // Right button
#define USB_MOUSE_BTN_MIDDLE    0x04    // Middle button
#define USB_MOUSE_BTN_BACK      0x08    // Back (side button)
#define USB_MOUSE_BTN_FORWARD   0x10    // Forward (side button)

// ==========================================
// 6. Virtual Xbox-layout Gamepad Controls
//
// These are model-independent "logical controls" decoded by hid_bridge into
// the USB HID gamepad report. They are stored in a key action's `key_code`
// field; the action's `consumer_code` carries the analog magnitude
// (0 = use the control's default).
// ==========================================
typedef enum {
    GP_NONE = 0,
    // Face buttons
    GP_A, GP_B, GP_X, GP_Y,
    // Bumpers (shoulder) and analog triggers
    GP_LB, GP_RB, GP_LT, GP_RT,
    // Menu buttons
    GP_SELECT, GP_START, GP_GUIDE, GP_SHARE,
    // Analog stick presses (L3 / R3)
    GP_L3, GP_R3,
    // Directional pad
    GP_DPAD_UP, GP_DPAD_DOWN, GP_DPAD_LEFT, GP_DPAD_RIGHT,
    // Left analog stick directions
    GP_LS_UP, GP_LS_DOWN, GP_LS_LEFT, GP_LS_RIGHT,
    // Right analog stick directions
    GP_RS_UP, GP_RS_DOWN, GP_RS_LEFT, GP_RS_RIGHT,
    GP_CONTROL_COUNT
} gamepad_control_t;

// Default magnitudes used when an action's value is 0.
#define GP_STICK_DEFAULT    127     // analog stick full deflection (-127..127)
#define GP_TRIGGER_DEFAULT  255     // trigger full pull (0..255)

#ifdef __cplusplus
}
#endif
