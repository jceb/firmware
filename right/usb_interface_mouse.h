#ifndef __USB_DEVICE_HID_MOUSE_H__
#define __USB_DEVICE_HID_MOUSE_H__

// Macros:

    #define USB_MOUSE_REPORT_LENGTH (0x07U)

// Typedefs:

    typedef struct usb_device_hid_mouse_struct {
        uint8_t buffer[USB_MOUSE_REPORT_LENGTH];
    } usb_device_hid_mouse_struct_t;

    typedef struct usb_device_wheeled_mouse_struct {
        uint8_t button;
        int16_t x;
        int16_t y;
        int8_t verticalWheelMovement;
        int8_t horizontalWheelMovement;
    } usb_device_wheeled_mouse_struct_t;

// Functions:

    extern usb_status_t UsbMouseCallback(class_handle_t handle, uint32_t event, void *param);
    extern usb_status_t UsbMouseSetConfiguration(class_handle_t handle, uint8_t configuration);
    extern usb_status_t UsbMouseSetInterface(class_handle_t handle, uint8_t interface, uint8_t alternateSetting);

#endif