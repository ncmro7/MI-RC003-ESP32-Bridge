#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ==========================================
// 1. Audio & DSP Parameters
// ==========================================
#define AUDIO_SAMPLE_RATE         16000     // 16 kHz sample rate
#define AUDIO_BITS_PER_SAMPLE     16        // 16-bit PCM
#define AUDIO_CHANNELS            1         // Mono
#define AUDIO_DEFAULT_FRAME_BYTES 120       // Default 120 bytes ADPCM per BLE frame
#define AUDIO_DEFAULT_FRAME_SAMPS 240       // 120 * 2 = 240 PCM samples per frame
#define AUDIO_RING_BUFFER_SIZE    8192      // Ring buffer capacity (in samples, ~512 ms)

// AGC & Filter parameters
#define AGC_TARGET_LEVEL          12000.0f  // Target peak (~-8.7 dBFS), avoids driving noise to full scale
#define AGC_DECAY_RATE            0.9995f   // Peak envelope decay per sample
#define AGC_MAX_GAIN              4.0f      // Cap boost at +12 dB so background hiss stays low
#define AGC_NOISE_FLOOR           1000.0f   // Do not chase the envelope below this level
#define AUDIO_LEAD_MUTE_SAMPLES   2400      // 150 ms silence zone to hide button click
#define AUDIO_FADE_IN_SAMPLES     160       // 10 ms micro fade-in
#define DECLIP_THRESHOLD          1000

// ==========================================
// 2. BLE Central & ATVV Parameters
// ==========================================
#define BLE_REMOTE_NAME_PREFIX    "MI RC"
#define BLE_SCAN_INTERVAL_MS      100
#define BLE_SCAN_WINDOW_MS        80
#define BLE_KEEP_ALIVE_INTERVAL   2000      // MIC_EXTEND interval during active voice (ms)
#define BLE_MAX_DISCOVERED        32        // Discovered device cache size

// ATVV GATT UUIDs (128-bit)
#define ATVV_SVC_UUID             "ab5e0001-5a21-4f05-bc7d-af01f617b664"
#define ATVV_CHAR_CMD_UUID        "ab5e0002-5a21-4f05-bc7d-af01f617b664"
#define ATVV_CHAR_AUD_UUID        "ab5e0003-5a21-4f05-bc7d-af01f617b664"
#define ATVV_CHAR_CTL_UUID        "ab5e0004-5a21-4f05-bc7d-af01f617b664"

// Standard HOGP service / characteristics (16-bit)
#define HOGP_SVC_UUID             0x1812
#define HOGP_REPORT_CHAR_UUID     0x2A4D
#define HOGP_PROTOCOL_MODE_UUID   0x2A4E
#define HOGP_CONTROL_POINT_UUID   0x2A4C
#define HOGP_CCCD_UUID            0x2902

// ==========================================
// 3. FreeRTOS Task & Multi-Core Pinning
// ==========================================
#define TASK_CORE_BLE             0         // Core 0: NimBLE host & audio decoding
#define TASK_CORE_USB             1         // Core 1: TinyUSB & HID/audio push

#define PRIO_TASK_USB             6
#define PRIO_TASK_AUDIO_DSP       5
#define PRIO_TASK_BLE             4
#define PRIO_TASK_KEYMAP_CLI      3

// ==========================================
// 4. Default Keymap & Hotkey Codes
// ==========================================
// Default voice input hotkey: Right Alt + Comma (WeChat voice IME).
// HID keyboard modifiers: 0x01=LCTRL, 0x02=LSHIFT, 0x04=LALT, 0x08=LGUI,
//                         0x10=RCTRL, 0x20=RSHIFT, 0x40=RALT, 0x80=RGUI
#define DEFAULT_VOICE_MODIFIER    0x40      // KEY_MOD_RALT
#define DEFAULT_VOICE_KEY         0x36      // HID usage for ',' 

// ==========================================
// 5. USB composite device layout
// ==========================================
// Interface numbers (must match usb_descriptors.c)
#define USB_ITF_UAC_AC            0
#define USB_ITF_UAC_AS            1
#define USB_ITF_HID               2         // HID: keyboard + consumer + mouse
#define USB_ITF_VENDOR            3
#define USB_ITF_XUSB              4         // XUSB game controller (MS-XUSBI, vendor-specific)

// Endpoint addresses
// NOTE: the ESP32-S3 DWC2 exposes five IN endpoints total *including* EP0, so at
// most four non-control IN endpoints can be active. All four are used here
// (UAC / HID / Vendor / XUSB); OUT endpoints are unconstrained by that budget.
#define USB_EP_UAC_IN             0x81      // Isochronous IN  (microphone)
#define USB_EP_HID_IN             0x82      // Interrupt IN    (keyboard/consumer/mouse)
#define USB_EP_VENDOR_IN          0x83      // Bulk IN         (WebUSB responses)
#define USB_EP_XUSB_IN            0x84      // Interrupt IN    (XUSB controller state)
#define USB_EP_XUSB_OUT           0x01      // Interrupt OUT   (XUSB rumble / control)
#define USB_EP_VENDOR_OUT         0x02      // Bulk OUT        (WebUSB requests)

// WebUSB bulk transfer / framing
#define WEBUSB_FRAME_SOF0         0x4D      // 'M'
#define WEBUSB_FRAME_SOF1         0x52      // 'R'
#define WEBUSB_FRAME_HEADER_LEN   6
#define WEBUSB_MAX_PAYLOAD        32768     // full multi-layer keymap JSON is a few KB
#define WEBUSB_TX_CHUNK           64

// ==========================================
// 6. On-board RGB LED
// ==========================================
#ifndef RGB_BUILTIN
#define RGB_BUILTIN               48        // GPIO48 on most ESP32-S3 DevKitC-1 boards
#endif

#ifdef __cplusplus
}
#endif
