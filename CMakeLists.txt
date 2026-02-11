cmake_minimum_required(VERSION 3.16)

# Exclude managed components we don't use — reduces build time significantly
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
