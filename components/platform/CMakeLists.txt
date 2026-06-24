set(platform_srcs)
set(platform_reqs)

# ESP-IDF evaluates component CMake during an early requirements pass before
# Kconfig symbols are fully populated. Keep SenseCAP as the safe default there.
if(CONFIG_FLIGHTSABOVE_PLATFORM_LILYGO_T_DISPLAY_S3)
    list(APPEND platform_srcs "platform_lilygo_t_display_s3.cpp")
elseif(CONFIG_FLIGHTSABOVE_PLATFORM_ROUND_360)
    list(APPEND platform_srcs "platform_round_360.cpp")
elseif(CONFIG_FLIGHTSABOVE_PLATFORM_SENSECAP OR NOT DEFINED CONFIG_FLIGHTSABOVE_PLATFORM_SENSECAP)
    list(APPEND platform_srcs "platform_sensecap.cpp")
    list(APPEND platform_reqs seeed_indicator)
else()
    message(FATAL_ERROR "No supported FlightsAbove platform selected")
endif()

idf_component_register(
    SRCS ${platform_srcs}
    INCLUDE_DIRS "include"
    REQUIRES ${platform_reqs}
)
