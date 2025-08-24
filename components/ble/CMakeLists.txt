if(IDF_TARGET STREQUAL "esp32c3")
    # Optionally embed a local test provisioning JSON for TEST mode
    set(_EMBED_JSON_FILE "")
    if(EXISTS ${CMAKE_CURRENT_LIST_DIR}/test_prov.json)
        set(_EMBED_JSON_FILE "test_prov.json")
    elseif(EXISTS ${CMAKE_CURRENT_LIST_DIR}/test_prov.json.example)
        set(_EMBED_JSON_FILE "test_prov.json.example")
    endif()

    # Use NimBLE-based implementation on ESP32-C3
    if(_EMBED_JSON_FILE)
        # Sanitize for linker symbol name: replace '.' with '_'
        string(REPLACE "." "_" _EMBED_JSON_SYM "${_EMBED_JSON_FILE}")
        idf_component_register(
            SRCS "ble_nimble.c"
            INCLUDE_DIRS "include"
            REQUIRES log json freertos esp_system bt nvs_flash
            PRIV_REQUIRES main
            EMBED_TXTFILES ${_EMBED_JSON_FILE}
        )
        target_compile_definitions(${COMPONENT_LIB} PRIVATE BLE_PROV_TEST_EMBED_NAME="${_EMBED_JSON_FILE}")
        target_compile_definitions(${COMPONENT_LIB} PRIVATE BLE_PROV_TEST_EMBED_SYM="${_EMBED_JSON_SYM}")
    else()
        idf_component_register(
            SRCS "ble_nimble.c"
            INCLUDE_DIRS "include"
            REQUIRES log json freertos esp_system bt nvs_flash
            PRIV_REQUIRES main
        )
    endif()
else()
    idf_component_register(
        SRCS "ble.c"
        INCLUDE_DIRS "include"
        REQUIRES json log nvs_flash esp_system freertos bt mbedtls storage crypto
        PRIV_REQUIRES main
    )
endif()

# Note: unit tests are not compiled into the firmware by default
