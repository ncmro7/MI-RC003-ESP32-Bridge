#include "usb_descriptors.h"
#include "xusb_gamepad.h"
#include "app_config.h"
#include "app_log.h"
#include "version.h"

#include "tusb.h"
#include "class/hid/hid_device.h"
#include "class/vendor/vendor_device.h"
#include "esp_mac.h"

// ===========================================================================
// Device descriptor
// ===========================================================================
const tusb_desc_device_t usb_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0210, // >= 2.1 is required for BOS / WebUSB
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,      // Espressif
    .idProduct = 0x8304,     // MI-RC003 bridge
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

// ===========================================================================
// String descriptors
// ===========================================================================
const char *usb_string_descriptors[] = {
    (char[]){0x09, 0x04},        // 0: language id (English US)
    "HD838A",                    // 1: manufacturer
    "MI-RC003 Remote Bridge",     // 2: product
    "MI-RC003-0001",              // 3: serial
    WEBUSB_LANDING_URL,          // 4: WebUSB landing page
    "MI-RC003 HID",               // 5: HID interface
    "MI-RC003 Microphone",        // 6: UAC microphone
    "MI-RC003 Interface",         // 7: WebUSB vendor interface (esp_tinyusb allows max 8)
};
const int usb_string_descriptor_count =
    (int)(sizeof(usb_string_descriptors) / sizeof(usb_string_descriptors[0]));

// The serial number must be unique per chip. Windows keys the device instance
// on VID/PID/serial; a fixed serial makes Windows reuse a stale, incompatible
// registry entry ("device settings were not migrated", Code 10/28).
static char s_serial[16];

void usb_descriptors_init(void)
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    usb_string_descriptors[3] = s_serial;
}

// ===========================================================================
// HID interface 0 (ITF 2): keyboard (report ID 1) + consumer (report ID 2)
// + relative mouse (report ID 3: 5 buttons, X/Y, wheel)
// ===========================================================================
const uint8_t usb_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(1)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(2)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(3)),
};
const uint16_t usb_hid_report_descriptor_len = sizeof(usb_hid_report_descriptor);

// ===========================================================================
// Configuration descriptor
//
//   ITF 0/1 : UAC 1.0 microphone (custom class driver, 108-byte descriptor set)
//   ITF 2   : HID keyboard + consumer + mouse
//   ITF 3   : WebUSB vendor-specific bulk interface
//   ITF 4   : XUSB game controller (MS-XUSBI vendor interface, 0x21 descriptor)
// ===========================================================================
// ITF 4 is a standard interface descriptor (9) + XUSB interface device
// descriptor type 0x21 (17) + two endpoint descriptors (7 + 7).
#define XUSB_ITF_DESC_LEN     40
#define USB_CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + 108 + TUD_HID_DESC_LEN + \
                               TUD_VENDOR_DESC_LEN + XUSB_ITF_DESC_LEN)

const uint8_t usb_config_descriptor[] = {
    // Configuration number, interface count, string index, total length, attributes, power (mA)
    TUD_CONFIG_DESCRIPTOR(1, 5, 0, USB_CONFIG_TOTAL_LEN,
                          0, 100),

    // -------------------- UAC 1.0 microphone (108 bytes) --------------------
    // 1. IAD (bFirstInterface=0, bInterfaceCount=2, audio, iFunction=6)
    0x08, 0x0B, USB_ITF_UAC_AC, 0x02, 0x01, 0x00, 0x00, 6,
    // 2. Standard AC interface
    0x09, 0x04, USB_ITF_UAC_AC, 0x00, 0x00, 0x01, 0x01, 0x00, 6,
    // 3. CS AC header
    0x09, 0x24, 0x01, 0x00, 0x01, 0x27, 0x00, 0x01, USB_ITF_UAC_AS,
    // 4. Input terminal (microphone)
    0x0C, 0x24, 0x02, 0x01, 0x01, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00,
    // 5. Feature unit (mute + volume)
    0x09, 0x24, 0x06, 0x02, 0x01, 0x01, 0x01, 0x02, 0x00,
    // 6. Output terminal (USB streaming)
    0x09, 0x24, 0x03, 0x03, 0x01, 0x01, 0x00, 0x02, 0x00,
    // 7. Standard AS interface, alt 0 (zero bandwidth)
    0x09, 0x04, USB_ITF_UAC_AS, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    // 8. Standard AS interface, alt 1 (active, 16 kHz)
    0x09, 0x04, USB_ITF_UAC_AS, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    // 9. CS AS general
    0x07, 0x24, 0x01, 0x03, 0x01, 0x01, 0x00,
    // 10. Format type I (16 kHz, 16-bit, mono)
    0x0B, 0x24, 0x02, 0x01, 0x01, 0x02, 0x10, 0x01, 0x80, 0x3E, 0x00,
    // 11. Isochronous IN endpoint (64 bytes / 32 samples, bInterval = 2 ms).
    //     64 B / 2 ms = 32 kB/s = 16 kHz mono 16-bit, and the 2 ms bInterval
    //     avoids the ESP32-S3 DWC2 even/odd frame-boundary bug.
    0x09, 0x05, USB_EP_UAC_IN, 0x05, 0x40, 0x00, 0x02, 0x00, 0x00,
    // 12. CS endpoint general
    0x07, 0x25, 0x01, 0x00, 0x00, 0x00, 0x00,

    // -------------------- HID keyboard + consumer --------------------
    // Interface number, string index, boot protocol, report descriptor len,
    // EP IN address, EP size, polling interval (ms)
    TUD_HID_DESCRIPTOR(USB_ITF_HID, 5, false, sizeof(usb_hid_report_descriptor),
                       USB_EP_HID_IN, 16, 5),

    // -------------------- WebUSB vendor interface --------------------
    // Interface number, string index, EP OUT address, EP IN address, EP size
    TUD_VENDOR_DESCRIPTOR(USB_ITF_VENDOR, 7, USB_EP_VENDOR_OUT, USB_EP_VENDOR_IN, 64),

    // -------------------- XUSB game controller (ITF 4) --------------------
    // Standard vendor-specific interface: class 0xFF / subclass 0x5D (XUSB)
    // / protocol 0x01 (wired game controller), 2 endpoints.
    0x09, 0x04, USB_ITF_XUSB, 0x00, 0x02, 0xFF, 0x5D, 0x01, 0,
    // XUSB Interface Device Descriptor (MS-XUSBI, bDescriptorType 0x21).
    // wReports encodes endpoint address (high byte) + type/count (low byte):
    //   IN  0x84: data/status mix, 5 reports
    //   OUT 0x01: control/status, 3 reports
    0x11, 0x21,
    0x00, 0x01,             // bcdXUSB 1.00
    0x01,                   // bDeviceSubtype = wired game controller
    0x25, USB_EP_XUSB_IN,   // IN endpoint reports (type=mix, count=5)
    0x14, 0x00, 0x00, 0x00, 0x00,  // report sizes 0x00..0x04
    0x13, USB_EP_XUSB_OUT,  // OUT endpoint reports (type=control/status, count=3)
    0x08, 0x00, 0x00,       // report sizes 0x00..0x02
    // Endpoint IN (interrupt, 32 bytes, 4 ms).
    0x07, 0x05, USB_EP_XUSB_IN, 0x03, 0x20, 0x00, 0x04,
    // Endpoint OUT (interrupt, 32 bytes, 8 ms).
    0x07, 0x05, USB_EP_XUSB_OUT, 0x03, 0x20, 0x00, 0x08,
};

TU_VERIFY_STATIC(sizeof(usb_config_descriptor) == USB_CONFIG_TOTAL_LEN,
                 "configuration descriptor total length mismatch");

// ===========================================================================
// Binary Object Store (WebUSB + Microsoft OS 2.0)
//
// Windows does not auto-bind a driver for a generic vendor interface. The
// Microsoft OS 2.0 descriptor set advertises the "WINUSB" compatible ID so
// Windows loads winusb.sys and Chrome can reach the interface via WebUSB.
// ===========================================================================
#define VENDOR_REQUEST_MICROSOFT  0x02

// The MS OS 2.0 descriptor set carries two function subsets:
//   ITF 3 (WebUSB vendor) -> WINUSB compatible ID + DeviceInterfaceGUIDs
//   ITF 4 (XUSB)          -> "XUSB10" compatible ID so Windows binds xusb22.sys
#define MS_OS_20_LEN_WINUSB_FUNC  0x00A0   // 8 header + 20 compatible + 132 regprop
#define MS_OS_20_LEN_REGPROP      0x0084
#define MS_OS_20_LEN_XUSB_FUNC    0x001C   // 8 header + 20 compatible
#define MS_OS_20_DESC_LEN         (0x000A + 0x0008 + \
                                   MS_OS_20_LEN_WINUSB_FUNC + MS_OS_20_LEN_XUSB_FUNC)

#define USB_BOS_TOTAL_LEN  (TUD_BOS_DESC_LEN + TUD_BOS_WEBUSB_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

static const uint8_t usb_bos_descriptor[] = {
    TUD_BOS_DESCRIPTOR(USB_BOS_TOTAL_LEN, 2),
    TUD_BOS_WEBUSB_DESCRIPTOR(WEBUSB_VENDOR_CODE, 4),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT),
};

uint8_t const *tud_descriptor_bos_cb(void)
{
    return usb_bos_descriptor;
}

// Microsoft OS 2.0 descriptor set: WINUSB compatible ID + DeviceInterfaceGUIDs.
static const uint8_t desc_ms_os_20[] = {
    // Set header: length, type, windows version, total length
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

    // Configuration subset header: length, type, configuration index, reserved, total length
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),

    // Function subset header (WebUSB): first interface, subset length
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    USB_ITF_VENDOR, 0, U16_TO_U8S_LE(MS_OS_20_LEN_WINUSB_FUNC),

    // Compatible ID descriptor: length, type, "WINUSB", sub-compatible
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // Registry property descriptor: DeviceInterfaceGUIDs
    U16_TO_U8S_LE(MS_OS_20_LEN_REGPROP),
    U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002A), // wPropertyDataType, wPropertyNameLength
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00, 'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00,
    't', 0x00, 'e', 0x00, 'r', 0x00, 'f', 0x00, 'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00,
    'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00, 0x00, 0x00,
    U16_TO_U8S_LE(0x0050), // wPropertyDataLength
    '{', 0x00, '9', 0x00, '7', 0x00, '5', 0x00, 'F', 0x00, '4', 0x00, '4', 0x00, 'D', 0x00,
    '9', 0x00, '-', 0x00, '0', 0x00, 'D', 0x00, '0', 0x00, '8', 0x00, '-', 0x00, '4', 0x00,
    '3', 0x00, 'F', 0x00, 'D', 0x00, '-', 0x00, '8', 0x00, 'B', 0x00, '3', 0x00, 'E', 0x00,
    '-', 0x00, '1', 0x00, '2', 0x00, '7', 0x00, 'C', 0x00, 'A', 0x00, '8', 0x00, 'A', 0x00,
    'F', 0x00, 'F', 0x00, 'F', 0x00, '9', 0x00, 'D', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00,

    // --- Function subset: XUSB game controller (ITF 4) ---
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    USB_ITF_XUSB, 0, U16_TO_U8S_LE(MS_OS_20_LEN_XUSB_FUNC),
    // Compatible ID: "XUSB10"
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'X', 'U', 'S', 'B', '1', '0', 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "MS OS 2.0 descriptor size mismatch");

// ===========================================================================
// WebUSB landing page URL descriptor
// ===========================================================================
static const tusb_desc_webusb_url_t usb_url_descriptor = {
    .bLength = (uint8_t)(3 + sizeof(WEBUSB_LANDING_URL) - 1),
    .bDescriptorType = 3, // WEBUSB URL descriptor type
    .bScheme = WEBUSB_LANDING_SCHEME,
    .url = WEBUSB_LANDING_URL,
};

// Invoked for every vendor-type control request. WebUSB fetches the landing
// page URL, and Windows fetches the Microsoft OS 2.0 descriptor set.
bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    app_log("USB", "vendor req=0x%02X bmRequestType=0x%02X wVal=0x%04X wIdx=0x%04X wLen=%u",
            request->bRequest, request->bmRequestType, request->wValue,
            request->wIndex, request->wLength);

    // XUSB vendor requests (GET_DEVICE_ID / SET_CONTROL) are dispatched here
    // because the ESP-IDF TinyUSB fork routes every vendor request to this
    // callback, then delegated to the XUSB class implementation.
    if (xusb_gamepad_handle_vendor_request(rhport, request)) {
        return true;
    }

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR) {
        // WebUSB GET_URL (device-level).
        if (request->bRequest == WEBUSB_VENDOR_CODE) {
            return tud_control_xfer(rhport, request, (void *)(uintptr_t)&usb_url_descriptor,
                                    usb_url_descriptor.bLength);
        }
        if (request->bRequest == VENDOR_REQUEST_MICROSOFT) {
            uint16_t total_len;
            memcpy(&total_len, desc_ms_os_20 + 8, 2);
            return tud_control_xfer(rhport, request, (void *)(uintptr_t)desc_ms_os_20, total_len);
        }
    }
    return false;
}

// ===========================================================================
// HID callbacks
// ===========================================================================
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return usb_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}
