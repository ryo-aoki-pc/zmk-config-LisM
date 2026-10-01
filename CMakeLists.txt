target_sources_ifdef(CONFIG_ZMK_USB_BOOTMAGIC app PRIVATE src/usb_bootmagic.c)

if(CONFIG_DT_HAS_ZMK_USB_BOOTMAGIC_ENABLED AND NOT CONFIG_ZMK_USB_BOOTMAGIC)
    message(WARNING "zmk,usb-bootmagic is in the devicetree, but CONFIG_ZMK_USB_BOOTMAGIC is disabled (CONFIG_USB_DEVICE_STACK is required)")
endif()
