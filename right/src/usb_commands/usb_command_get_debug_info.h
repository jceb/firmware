#ifndef __USB_COMMAND_GET_DEBUG_INFO_H__
#define __USB_COMMAND_GET_DEBUG_INFO_H__

// Includes:

    #include "usb_interfaces/usb_interface_generic_hid.h"

// Macros:

    #define SET_DEBUG_BUFFER_UINT8(offset, value) (*(uint8_t*)(UsbDebugInfo+(offset)) = (value))
    #define SET_DEBUG_BUFFER_UINT16(offset, value) (*(uint16_t*)(UsbDebugInfo+(offset)) = (value))
    #define SET_DEBUG_BUFFER_UINT32(offset, value) (*(uint32_t*)(UsbDebugInfo+(offset)) = (value))

// Variables:

    extern uint8_t UsbDebugInfo[USB_GENERIC_HID_OUT_BUFFER_LENGTH];

// Functions:

    void UsbCommand_GetDebugInfo(void);

#endif