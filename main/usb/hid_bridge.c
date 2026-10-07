#include "hid_bridge.h"
#include "usb/xusb_gamepad.h"
#include "app_config.h"
#include "app_log.h"
#include "audio/audio_pipeline.h"
#include "led/led_indicator.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tusb.h"
#include "class/hid/hid_device.h"

#define HID_REPORT_ID_KEYBOARD 1
#define HID_REPORT_ID_CONSUMER 2
#define HID_REPORT_ID_MOUSE    3

// HID instance 0 = keyboard/consumer/mouse. The gamepad is a separate XUSB
// (XInput) interface, not a HID instance.
#define HID_INST_KEYBOARD 0

// Maximum number of simultaneously held virtual gamepad controls (enough for
// both sticks fully deflected plus a few buttons).
#define GP_MAX_HELD 12

typedef struct {
    uint8_t control;  // gamepad_control_t
    uint8_t value;    // analog magnitude (0 = control default)
} gp_held_t;

static SemaphoreHandle_t s_hid_mutex = NULL;
static uint8_t s_mouse_buttons = 0;
static gp_held_t s_gp_held[GP_MAX_HELD];
static size_t s_gp_held_count = 0;
static volatile bool s_transport_enabled = false;
static volatile bool s_keyboard_release_pending = false;
static volatile bool s_consumer_release_pending = false;
static volatile bool s_mouse_release_pending = false;
static bool s_release_task_started = false;

// Non-blocking gamepad taps: the press is submitted immediately and the
// release is scheduled and handled by the release task, so the caller never
// blocks on a gamepad tap (which would stall BLE/key processing).
#define GP_TAP_HOLD_MS 100
typedef struct {
    uint8_t control;
    TickType_t release_at;
} gp_tap_t;

static gp_tap_t s_gp_tap[GP_MAX_HELD];
static size_t s_gp_tap_count = 0;

static void gamepad_process_taps(void);

// Real HID output state, used to drive the LED (yellow while the host is
// receiving a pressed key/button, cleared on release).
static bool s_keyboard_pressed = false;
static bool s_consumer_pressed = false;

static void update_hid_led(void)
{
    led_indicator_set_hid_active(s_keyboard_pressed || s_consumer_pressed ||
                                 s_mouse_buttons != 0 || s_gp_held_count != 0);
}

static void hid_lock(void)
{
    if (!s_hid_mutex) {
        s_hid_mutex = xSemaphoreCreateMutex();
    }
    if (s_hid_mutex) {
        xSemaphoreTake(s_hid_mutex, portMAX_DELAY);
    }
}

static void hid_unlock(void)
{
    if (s_hid_mutex) {
        xSemaphoreGive(s_hid_mutex);
    }
}

static void hid_release_retry_task(void *arg)
{
    (void)arg;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if (!s_transport_enabled || tud_suspended()) {
            continue;
        }

        // All reports share one HID IN endpoint, so submit at most one report
        // per pass and wait for the host to consume it before the next one.
        if (s_keyboard_release_pending) {
            usb_hid_keyboard_release();
        } else if (s_consumer_release_pending) {
            usb_hid_consumer_release();
        } else if (s_mouse_release_pending) {
            usb_hid_mouse_buttons_release();
        }

        // Release gamepad taps whose hold time elapsed.
        gamepad_process_taps();

        // Keep the XInput report stream alive (~200 Hz) like a real controller.
        xusb_gamepad_keepalive();
    }
}

void hid_bridge_init(void)
{
    if (!s_hid_mutex) {
        s_hid_mutex = xSemaphoreCreateMutex();
    }
    if (!s_release_task_started) {
        s_release_task_started =
            xTaskCreatePinnedToCore(hid_release_retry_task, "hid_release", 2048,
                                    NULL, PRIO_TASK_USB, NULL,
                                    TASK_CORE_USB) == pdPASS;
    }
}

void hid_bridge_set_transport_enabled(bool enabled)
{
    hid_lock();
    s_transport_enabled = enabled;
    if (!enabled) {
        s_keyboard_pressed = false;
        s_consumer_pressed = false;
        s_mouse_buttons = 0;
        s_gp_held_count = 0;
        update_hid_led();
    }
    hid_unlock();
}

static bool hid_prepare_report(void)
{
    return s_transport_enabled && !tud_suspended() && tud_hid_ready();
}

bool usb_hid_keyboard_press(uint8_t modifier, uint8_t keycode)
{
    if (!hid_prepare_report()) {
        return false;
    }
    hid_lock();

    // HID usages 0xE0..0xE7 are modifiers and must go in the modifier byte,
    // not the 6-key rollover array. This keeps legacy keymaps (where a
    // modifier was stored as the key code) working.
    if (keycode >= 0xE0 && keycode <= 0xE7) {
        modifier |= (uint8_t)(1u << (keycode - 0xE0));
        keycode = 0;
    }

    hid_keyboard_report_t report = {0};
    report.modifier = modifier;
    report.keycode[0] = keycode;
    bool ok = tud_hid_n_report(0, HID_REPORT_ID_KEYBOARD, &report, sizeof(report));
    if (!ok) {
        app_log("USB_HID", "Keyboard report rejected");
    }
    if (ok) {
        s_keyboard_release_pending = false;
    }
    s_keyboard_pressed = (modifier != 0 || keycode != 0);
    update_hid_led();

    hid_unlock();
    return ok;
}

bool usb_hid_keyboard_release(void)
{
    hid_lock();

    // Always clear the tracked state, even when the bus is not ready, so the
    // LED cannot get stuck on after a disconnect.
    s_keyboard_pressed = false;
    update_hid_led();

    bool ok = false;
    if (s_transport_enabled && !tud_suspended() && tud_hid_ready()) {
        hid_keyboard_report_t report = {0};
        ok = tud_hid_n_report(0, HID_REPORT_ID_KEYBOARD, &report, sizeof(report));
    }
    s_keyboard_release_pending = !ok;

    hid_unlock();
    return ok;
}

bool usb_hid_keyboard_tap(uint8_t modifier, uint8_t keycode)
{
    if (!usb_hid_keyboard_press(modifier, keycode)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(15));
    usb_hid_keyboard_release();
    return true;
}

bool usb_hid_consumer_press(uint16_t usage_code)
{
    if (!hid_prepare_report()) {
        return false;
    }
    hid_lock();

    uint8_t report[2] = { (uint8_t)(usage_code & 0xFF), (uint8_t)(usage_code >> 8) };
    bool ok = tud_hid_n_report(0, HID_REPORT_ID_CONSUMER, report, sizeof(report));
    if (!ok) {
        app_log("USB_HID", "Consumer report rejected");
    }
    if (ok) {
        s_consumer_release_pending = false;
    }
    s_consumer_pressed = (usage_code != 0);
    update_hid_led();

    hid_unlock();
    return ok;
}

bool usb_hid_consumer_release(void)
{
    hid_lock();

    s_consumer_pressed = false;
    update_hid_led();

    bool ok = false;
    if (s_transport_enabled && !tud_suspended() && tud_hid_ready()) {
        uint8_t report[2] = {0, 0};
        ok = tud_hid_n_report(0, HID_REPORT_ID_CONSUMER, report, sizeof(report));
    }
    s_consumer_release_pending = !ok;

    hid_unlock();
    return ok;
}

bool usb_hid_consumer_tap(uint16_t usage_code)
{
    if (!usb_hid_consumer_press(usage_code)) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(15));
    usb_hid_consumer_release();
    return true;
}

// Caller must hold the HID mutex.
static bool mouse_report_locked(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    if (!hid_prepare_report()) {
        return false;
    }

    hid_mouse_report_t report = {0};
    report.buttons = buttons;
    report.x = dx;
    report.y = dy;
    report.wheel = wheel;
    bool ok = tud_hid_n_report(0, HID_REPORT_ID_MOUSE, &report, sizeof(report));
    if (!ok) {
        app_log("USB_HID", "Mouse report rejected");
    }
    if (ok) {
        s_mouse_release_pending = false;
    }
    return ok;
}

bool usb_hid_mouse_button_press(uint8_t button_mask)
{
    hid_lock();
    s_mouse_buttons |= button_mask;
    bool ok = mouse_report_locked(s_mouse_buttons, 0, 0, 0);
    update_hid_led();
    hid_unlock();
    return ok;
}

bool usb_hid_mouse_button_release(uint8_t button_mask)
{
    hid_lock();
    s_mouse_buttons &= (uint8_t)~button_mask;
    bool ok = mouse_report_locked(s_mouse_buttons, 0, 0, 0);
    if (!ok) {
        s_mouse_release_pending = true;
    }
    update_hid_led();
    hid_unlock();
    return ok;
}

bool usb_hid_mouse_buttons_release(void)
{
    hid_lock();
    s_mouse_buttons = 0;
    bool ok = false;
    if (s_transport_enabled && !tud_suspended() && tud_hid_ready()) {
        hid_mouse_report_t report = {0};
        ok = tud_hid_n_report(0, HID_REPORT_ID_MOUSE, &report, sizeof(report));
    }
    s_mouse_release_pending = !ok;
    update_hid_led();
    hid_unlock();
    return ok;
}

bool usb_hid_mouse_move(int8_t dx, int8_t dy)
{
    hid_lock();
    bool ok = mouse_report_locked(s_mouse_buttons, dx, dy, 0);
    hid_unlock();
    return ok;
}

bool usb_hid_mouse_wheel(int8_t wheel)
{
    hid_lock();
    bool ok = mouse_report_locked(s_mouse_buttons, 0, 0, wheel);
    hid_unlock();
    return ok;
}

// ===========================================================================
// Virtual gamepad (XInput via XUSB)
//
// Logical controls (gamepad_control_t) are accumulated into a held set so
// several can combine (a stick diagonal, bumper + trigger, ...). On every
// change the whole report is recomputed into a 20-byte XUSB game controller
// input report and submitted on the XUSB interrupt IN endpoint.
// ===========================================================================

static int gamepad_held_index(uint8_t control)
{
    for (size_t i = 0; i < s_gp_held_count; i++) {
        if (s_gp_held[i].control == control) return (int)i;
    }
    return -1;
}

static void gamepad_held_set(uint8_t control, uint8_t value)
{
    int idx = gamepad_held_index(control);
    if (idx >= 0) {
        s_gp_held[idx].value = value;
    } else if (s_gp_held_count < GP_MAX_HELD) {
        s_gp_held[s_gp_held_count].control = control;
        s_gp_held[s_gp_held_count].value = value;
        s_gp_held_count++;
    }
}

static void gamepad_held_clear(uint8_t control)
{
    int idx = gamepad_held_index(control);
    if (idx < 0) return;
    s_gp_held[idx] = s_gp_held[s_gp_held_count - 1];
    s_gp_held_count--;
}

static int clamp_i(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// XUSB bmButtons bit for a digital control (MS-XUSBI Table 53).
static uint16_t gamepad_button_bit(uint8_t control)
{
    switch (control) {
        case GP_DPAD_UP:    return XUSB_BTN_DPAD_UP;
        case GP_DPAD_DOWN:  return XUSB_BTN_DPAD_DOWN;
        case GP_DPAD_LEFT:  return XUSB_BTN_DPAD_LEFT;
        case GP_DPAD_RIGHT: return XUSB_BTN_DPAD_RIGHT;
        case GP_START:      return XUSB_BTN_START;
        case GP_SELECT:     return XUSB_BTN_BACK;
        case GP_L3:         return XUSB_BTN_L3;
        case GP_R3:         return XUSB_BTN_R3;
        case GP_LB:         return XUSB_BTN_LB;
        case GP_RB:         return XUSB_BTN_RB;
        case GP_GUIDE:      return XUSB_BTN_GUIDE;
        case GP_SHARE:      return XUSB_BTN_BINDING;
        case GP_A:          return XUSB_BTN_A;
        case GP_B:          return XUSB_BTN_B;
        case GP_X:          return XUSB_BTN_X;
        case GP_Y:          return XUSB_BTN_Y;
        default:            return 0;
    }
}

// Caller must hold the HID mutex.
static bool gamepad_send_locked(void)
{
    int lsx = 0, lsy = 0, rsx = 0, rsy = 0, lt = 0, rt = 0;
    uint16_t buttons = 0;

    for (size_t i = 0; i < s_gp_held_count; i++) {
        uint8_t c = s_gp_held[i].control;
        int mag = s_gp_held[i].value ? s_gp_held[i].value : GP_STICK_DEFAULT;

        buttons |= gamepad_button_bit(c);

        switch (c) {
            case GP_LT:
                lt = s_gp_held[i].value ? s_gp_held[i].value : GP_TRIGGER_DEFAULT;
                break;
            case GP_RT:
                rt = s_gp_held[i].value ? s_gp_held[i].value : GP_TRIGGER_DEFAULT;
                break;
            // XUSB/XInput Y axis: positive is up.
            case GP_LS_UP:    lsy += mag; break;
            case GP_LS_DOWN:  lsy -= mag; break;
            case GP_LS_LEFT:  lsx -= mag; break;
            case GP_LS_RIGHT: lsx += mag; break;
            case GP_RS_UP:    rsy += mag; break;
            case GP_RS_DOWN:  rsy -= mag; break;
            case GP_RS_LEFT:  rsx -= mag; break;
            case GP_RS_RIGHT: rsx += mag; break;
            default: break;
        }
    }

    lsx = clamp_i(lsx, -127, 127);
    lsy = clamp_i(lsy, -127, 127);
    rsx = clamp_i(rsx, -127, 127);
    rsy = clamp_i(rsy, -127, 127);

    // Expand -127..127 to the full XUSB -32768..32767 range (x258).
    xusb_gamepad_report_t report;
    memset(&report, 0, sizeof(report));
    report.report_id = 0x00;
    report.size = XUSB_REPORT_LEN;
    report.buttons = buttons;
    report.left_trigger = (uint8_t)clamp_i(lt, 0, 255);
    report.right_trigger = (uint8_t)clamp_i(rt, 0, 255);
    report.left_x = (int16_t)(lsx * 258);
    report.left_y = (int16_t)(lsy * 258);
    report.right_x = (int16_t)(rsx * 258);
    report.right_y = (int16_t)(rsy * 258);

    bool ok = false;
    if (s_transport_enabled && !tud_suspended()) {
        ok = xusb_gamepad_send(&report);
    }

    update_hid_led();
    return ok;
}

// Runs from the release task (5 ms cadence). Caller must not hold the mutex.
static void gamepad_process_taps(void)
{
    if (s_gp_tap_count == 0) {
        return;
    }
    TickType_t const now = xTaskGetTickCount();
    hid_lock();
    bool changed = false;
    for (size_t i = 0; i < s_gp_tap_count; ) {
        if ((int32_t)(now - s_gp_tap[i].release_at) >= 0) {
            gamepad_held_clear(s_gp_tap[i].control);
            s_gp_tap[i] = s_gp_tap[s_gp_tap_count - 1];
            s_gp_tap_count--;
            changed = true;
        } else {
            i++;
        }
    }
    if (changed) {
        gamepad_send_locked();
    }
    hid_unlock();
}

bool usb_hid_gamepad_press(uint8_t control, uint8_t value)
{
    if (control == GP_NONE || control >= GP_CONTROL_COUNT) return false;
    hid_lock();
    gamepad_held_set(control, value);
    bool ok = gamepad_send_locked();
    hid_unlock();
    return ok;
}

bool usb_hid_gamepad_release(uint8_t control)
{
    if (control == GP_NONE || control >= GP_CONTROL_COUNT) return false;
    hid_lock();
    gamepad_held_clear(control);
    bool ok = gamepad_send_locked();
    hid_unlock();
    return ok;
}

bool usb_hid_gamepad_tap(uint8_t control, uint8_t value)
{
    if (control == GP_NONE || control >= GP_CONTROL_COUNT) return false;

    // Non-blocking: press now and let the release task clear it after the hold
    // time. Re-tapping the same control extends its release deadline so rapid
    // taps stay responsive without ever blocking the caller.
    hid_lock();
    gamepad_held_set(control, value);

    TickType_t const release_at = xTaskGetTickCount() + pdMS_TO_TICKS(GP_TAP_HOLD_MS);
    size_t i = 0;
    for (; i < s_gp_tap_count; i++) {
        if (s_gp_tap[i].control == control) {
            s_gp_tap[i].release_at = release_at;
            break;
        }
    }
    if (i == s_gp_tap_count && s_gp_tap_count < GP_MAX_HELD) {
        s_gp_tap[s_gp_tap_count].control = control;
        s_gp_tap[s_gp_tap_count].release_at = release_at;
        s_gp_tap_count++;
    }

    bool ok = gamepad_send_locked();
    hid_unlock();
    return ok;
}

bool usb_hid_gamepad_release_all(void)
{
    hid_lock();
    s_gp_held_count = 0;
    s_gp_tap_count = 0;
    bool ok = false;
    if (s_transport_enabled && !tud_suspended()) {
        xusb_gamepad_report_t neutral;
        memset(&neutral, 0, sizeof(neutral));
        neutral.report_id = 0x00;
        neutral.size = XUSB_REPORT_LEN;
        ok = xusb_gamepad_send(&neutral);
    }
    update_hid_led();
    hid_unlock();
    return ok;
}

void usb_hid_dispatch_action(const key_action_t *action)
{
    if (!action) return;

    app_log("USB_HID", "Emit action type=%d mod=0x%02X key=0x%02X cons=0x%04X dx=%d dy=%d wheel=%d",
            action->type, action->modifier, action->key_code, action->consumer_code,
            action->mouse_dx, action->mouse_dy, action->mouse_wheel);

    // The LED is driven by the real HID output state: usb_hid_*_press/release
    // turn it yellow while the host is receiving a held key/button. Voice
    // actions still take over with the blue streaming colour.
    if (action->type == ACTION_VOICE_HOLD) {
        led_indicator_set(LED_STATE_MIC_STREAMING);
    } else if (action->type == ACTION_VOICE_RELEASE) {
        led_indicator_set(LED_STATE_CONNECTED);
    }

    switch (action->type) {
        case ACTION_KEYBOARD_TAP:
            usb_hid_keyboard_tap(action->modifier, action->key_code);
            break;
        case ACTION_KEYBOARD_HOLD:
            usb_hid_keyboard_press(action->modifier, action->key_code);
            break;
        case ACTION_KEYBOARD_RELEASE:
            usb_hid_keyboard_release();
            break;
        case ACTION_CONSUMER_TAP:
            usb_hid_consumer_tap(action->consumer_code);
            break;
        case ACTION_CONSUMER_HOLD:
            usb_hid_consumer_press(action->consumer_code);
            break;
        case ACTION_CONSUMER_RELEASE:
            usb_hid_consumer_release();
            break;
        case ACTION_VOICE_HOLD:
            audio_pipeline_start_session(&g_audio_pipeline, 0);
            if (action->modifier != 0 || action->key_code != 0) {
                usb_hid_keyboard_press(action->modifier, action->key_code);
            }
            break;
        case ACTION_VOICE_RELEASE:
            usb_hid_keyboard_release();
            audio_pipeline_stop_session(&g_audio_pipeline);
            break;
        case ACTION_MOUSE_BUTTON_TAP:
            usb_hid_mouse_button_press(action->key_code);
            vTaskDelay(pdMS_TO_TICKS(15));
            usb_hid_mouse_button_release(action->key_code);
            break;
        case ACTION_MOUSE_BUTTON_HOLD:
            usb_hid_mouse_button_press(action->key_code);
            break;
        case ACTION_MOUSE_BUTTON_RELEASE:
            // Internal action only: emitted when a mouse-button-hold key is
            // released. A configured "mouse button release" action no longer
            // exists, so the button mask is always present here.
            if (action->key_code) {
                usb_hid_mouse_button_release(action->key_code);
            }
            break;
        case ACTION_MOUSE_MOVE:
            usb_hid_mouse_move(action->mouse_dx, action->mouse_dy);
            break;
        case ACTION_MOUSE_WHEEL:
            usb_hid_mouse_wheel(action->mouse_wheel);
            break;
        case ACTION_GAMEPAD_TAP:
            usb_hid_gamepad_tap(action->key_code, (uint8_t)action->consumer_code);
            break;
        case ACTION_GAMEPAD_HOLD:
            usb_hid_gamepad_press(action->key_code, (uint8_t)action->consumer_code);
            break;
        case ACTION_GAMEPAD_RELEASE:
            usb_hid_gamepad_release(action->key_code);
            break;
        default:
            break;
    }
}
