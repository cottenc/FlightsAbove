idf_component_register(
    SRCS
        "bsp/src/boards/bsp_board.c"
        "bsp/src/boards/sensecap_indicator_board.c"
        "bsp/src/boards/lcd_panel_config.c"
        "bsp/src/peripherals/bsp_i2c.c"
        "bsp/src/peripherals/bsp_lcd.c"
        "bsp/src/peripherals/lcd_panel_st7789.c"
        "bsp/src/peripherals/lcd_panel_st7796.c"
        "bsp/src/indev/indev.c"
        "bsp/src/indev/indev_tp.c"
        "bus/src/i2c_bus.c"
        "bus/src/spi_bus.c"
        "i2c_devices/io_expander/tca9535.c"
        "i2c_devices/touch_panel/ft5x06.c"
    INCLUDE_DIRS
        "bsp/include"
        "bsp/src/boards"
        "bsp/src/indev"
        "bsp/src/peripherals"
        "bus/include"
        "i2c_devices/io_expander"
        "i2c_devices/touch_panel/include"
    REQUIRES
        esp_lcd
        driver
        nvs_flash
        spiffs
        fatfs
        freertos
        log
)

# Seeed bus code uses %d with int32_t (long int on xtensa) — suppress format warnings
# in third-party code rather than patching all format strings.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format)
