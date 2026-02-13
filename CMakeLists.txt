cmake_minimum_required(VERSION 3.16)

# NOTE: EXCLUDE_COMPONENTS has no effect on pioarduino managed components.
# They are injected by the IDF component manager after this filter runs.
# Use custom_component_remove in platformio.ini instead.
# Kept here for pure ESP-IDF builds.
set(EXCLUDE_COMPONENTS
    # Zigbee stack
    espressif__esp-zigbee-lib
    espressif__esp-zboss-lib
    # Rainmaker ecosystem
    espressif__esp_rainmaker
    espressif__rmaker_common
    espressif__esp_schedule
    espressif__esp_secure_cert_mgr
    espressif__network_provisioning
    espressif__qrcode
    # ESP Insights / diagnostics
    espressif__esp_insights
    espressif__esp_diagnostics
    espressif__esp_diag_data_store
    # Industrial protocols
    espressif__esp-modbus
    espressif__esp_modem
    # Misc unused
    espressif__esp-serial-flasher
    espressif__esp_rcp_update
    espressif__cbor
    espressif__jsmn
    espressif__json_generator
    espressif__json_parser
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Squeek)
