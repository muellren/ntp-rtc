add_executable(ntp_rtc
        ntp_rtc.cpp
        )
pico_enable_stdio_usb(ntp_rtc 1)
pico_enable_stdio_uart(ntp_rtc 0)
target_compile_definitions(ntp_rtc PRIVATE
        WIFI_SSID=\"${WIFI_SSID}\"
        WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
        )
target_include_directories(ntp_rtc PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)
target_link_libraries(ntp_rtc
        #pico_cyw43_arch_lwip_threadsafe_background
        pico_cyw43_arch_lwip_poll
        pico_graphics
        pico_stdlib
        hardware_adc
        hardware_dma
        hardware_pio
        hardware_rtc
        galactic_unicorn
        )

pico_add_extra_outputs(ntp_rtc)


add_executable(ntp_rtc_simple_text
        ntp_rtc_simple_text.cpp
        )
pico_enable_stdio_usb(ntp_rtc_simple_text 1)
pico_enable_stdio_uart(ntp_rtc_simple_text 0)
target_compile_definitions(ntp_rtc_simple_text PRIVATE
        WIFI_SSID=\"${WIFI_SSID}\"
        WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
        )
target_include_directories(ntp_rtc_simple_text PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)
target_link_libraries(ntp_rtc_simple_text
        #pico_cyw43_arch_lwip_threadsafe_background
        pico_cyw43_arch_lwip_poll
        pico_graphics
        pico_stdlib
        hardware_adc
        hardware_dma
        hardware_pio
        hardware_rtc
        galactic_unicorn
        )

pico_add_extra_outputs(ntp_rtc_simple_text)
