set(incs .)
set(srcs
    app_main.c
    sdcard.c
    )

if (CONFIG_OVERRIDE_FLASH_PARTITION)
    list(APPEND srcs
        vendor_flash.c
    )
endif()

armino_component_register(SRCS "${srcs}" INCLUDE_DIRS "${incs}"
    PRIV_REQUIRES lwip_intf_v2_1 bk_netif bk_wifi bk_common fatfs)
