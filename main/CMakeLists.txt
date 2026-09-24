# Define source files
set(SOURCES "audio/asr_settings.cc"
            "audio/audio_codec.cc"
            "audio/audio_service.cc"
            "audio/gemini_transcribe_client.cc"
            "audio/gemini_asr_turn_controller.cc"
            "audio/demuxer/ogg_demuxer.cc"
            "notify/notification_controller.cc"
            "notify/notify_player.cc"
            "chat/text_chat_controller.cc"
            "audio/codecs/no_audio_codec.cc"
            "led/gpio_led.cc"
            "display/display.cc"
            "display/text_glyph.cc"
            "display/server_status_controller.cc"
            "display/vietnamese_glyph_fallback.cc"
            "display/lcd_display.cc"
            "display/lvgl_display/lvgl_display.cc"
            "display/lvgl_display/lvgl_theme.cc"
            "display/lvgl_display/lvgl_font.cc"
            "display/lvgl_display/dynamic_glyph_cache.cc"
            "display/lvgl_display/lvgl_image.cc"
            "display/lvgl_display/jpg/image_to_jpeg.cpp"
            "display/lvgl_display/jpg/jpeg_to_image.c"
            "protocols/protocol.cc"
            "protocols/text_glyph_payload.cc"
            "protocols/mqtt_protocol.cc"
            "protocols/websocket_protocol.cc"
            "mcp_server.cc"
            "system_info.cc"
            "application.cc"
            "ota.cc"
            "settings.cc"
            "device_state_machine.cc"
            "assets.cc"
            "main.cc"
            )

set(INCLUDE_DIRS "." "display" "display/lvgl_display" "display/lvgl_display/jpg" "audio" "audio/demuxer" "protocols")

# Platform abstractions shared with the application layer.
list(APPEND SOURCES
    "platform/board.cc"
    "platform/wifi_board.cc"
    "platform/backlight.cc"
    "platform/button.cc"
    "platform/esp32_camera.cc"
    "platform/esp32_camera_explain.cc"
)
list(APPEND INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/platform)


idf_build_get_property(build_components BUILD_COMPONENTS)
# Function to find component dynamically by pattern
function(find_component_by_pattern PATTERN COMPONENT_VAR PATH_VAR)
    foreach(COMPONENT ${build_components})
        if(COMPONENT MATCHES "${PATTERN}")
            set(${COMPONENT_VAR} ${COMPONENT} PARENT_SCOPE)
            idf_component_get_property(COMPONENT_PATH ${COMPONENT} COMPONENT_DIR)
            set(${PATH_VAR} "${COMPONENT_PATH}" PARENT_SCOPE)
            break()
        endif()
    endforeach()
endfunction()

# Fixed product identity and display assets for the only supported target.
set(BOARD_TYPE "esp32-s3-n16r8-cam")
set(BOARD_NAME "esp32-s3-n16r8-cam")
set(BOARD_MANUFACTURER "generic")
set(BUILTIN_TEXT_FONT font_noto_sans_basic_20_4)
set(BUILTIN_ICON_FONT font_material_symbols_20_4)

list(APPEND SOURCES
    "robot/desk_robot_board.cc"
    "robot/camera/camera_settings.cc"
    "robot/camera/camera_image_policy.cc"
    "robot/camera/desk_robot_camera.cc"
    "robot/control/robot_controller.cc"
    "robot/control/robot_settings.cc"
    "robot/display/mochan_display.cc"
    "robot/display/mochan_face_renderer.cc"
    "robot/display/mochan_display_overlay.cc"
    "robot/display/auto_brightness_policy.cc"
    "robot/display/secondary_display_controller.cc"
    "robot/display/secondary_oled.cc"
    "robot/display/secondary_oled_renderer.cc"
    "robot/display/secondary_oled_widgets.cc"
    "robot/display/secondary_oled_environment_widgets.cc"
    "robot/mcp/robot_mcp_tools.cc"
    "robot/motion/expressive_motion_planner.cc"
    "robot/motion/gyro_turn_controller.cc"
    "robot/motion/motor_controller.cc"
    "robot/motion/motion_reactions.cc"
    "robot/power/battery_controller.cc"
    "robot/power/battery_soc_estimator.cc"
    "robot/power/ina219_power_monitor.cc"
    "robot/sensors/aht20_sensor.cc"
    "robot/sensors/bh1750_sensor.cc"
    "robot/sensors/bmp280_sensor.cc"
    "robot/sensors/environment_controller.cc"
    "robot/sensors/environment_controller_sampling.cc"
    "robot/sensors/environment_derived.cc"
    "robot/sensors/shared_i2c_bus.cc"
    "robot/sensors/cliff_sensor.cc"
    "robot/sensors/mpu6050_motion_sensor.cc"
    "robot/robot_web_control_server.cc"
    "robot/web/robot_web_adapter.cc"
    "robot/web/robot_web_camera_settings.cc"
    "robot/web/robot_web_status_core.cc"
    "robot/web/robot_web_status_environment.cc"
    "robot/web/robot_web_status_hardware.cc"
    "robot/web/robot_web_status_io.cc"
    "robot/web/robot_web_status_routes.cc"
    "audio/engines/afe_audio_engine.cc"
    "audio/wake_words/custom_wake_word.cc"
    "audio/wake_words/wake_word_audio_cache.cc"
)
list(APPEND INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/robot"
    "${CMAKE_CURRENT_SOURCE_DIR}/robot/config"
)

# Keep the Web Control UI editable as focused HTML, CSS, and JavaScript files while
# preserving the existing single-page response at runtime. CMake assembles a generated
# header in the build tree; generated output is never a source-of-truth file.
#
# ESP-IDF evaluates component CMakeLists.txt once during early dependency expansion.
# File generation must not run in that pass because its current source directory can
# point at the temporary build context instead of this component's source directory.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    set(ROBOT_WEB_UI_DIR "${CMAKE_CURRENT_LIST_DIR}/robot/web/ui")
    set(ROBOT_WEB_JS_FILES
        "${ROBOT_WEB_UI_DIR}/js/core.js"
        "${ROBOT_WEB_UI_DIR}/js/drive.js"
        "${ROBOT_WEB_UI_DIR}/js/oled.js"
        "${ROBOT_WEB_UI_DIR}/js/chat_asr.js"
        "${ROBOT_WEB_UI_DIR}/js/server_config.js"
        "${ROBOT_WEB_UI_DIR}/js/status.js"
        "${ROBOT_WEB_UI_DIR}/js/camera_logs.js"
        "${ROBOT_WEB_UI_DIR}/js/tabs.js"
        "${ROBOT_WEB_UI_DIR}/app.js"
    )
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${ROBOT_WEB_UI_DIR}/index.html"
        "${ROBOT_WEB_UI_DIR}/style.css"
        "${ROBOT_WEB_UI_DIR}/favicon.svg"
        ${ROBOT_WEB_JS_FILES}
    )
    file(READ "${ROBOT_WEB_UI_DIR}/index.html" ROBOT_WEB_CONTROL_PAGE)
    file(READ "${ROBOT_WEB_UI_DIR}/style.css" ROBOT_WEB_CONTROL_STYLE)
    file(READ "${ROBOT_WEB_UI_DIR}/favicon.svg" ROBOT_WEB_CONTROL_FAVICON)
    set(ROBOT_WEB_CONTROL_SCRIPT "")
    foreach(ROBOT_WEB_JS_FILE IN LISTS ROBOT_WEB_JS_FILES)
        file(READ "${ROBOT_WEB_JS_FILE}" ROBOT_WEB_CONTROL_FRAGMENT)
        string(REPLACE "\r\n" "\n" ROBOT_WEB_CONTROL_FRAGMENT "${ROBOT_WEB_CONTROL_FRAGMENT}")
        string(REGEX REPLACE "\n$" "" ROBOT_WEB_CONTROL_FRAGMENT "${ROBOT_WEB_CONTROL_FRAGMENT}")
        string(APPEND ROBOT_WEB_CONTROL_SCRIPT "${ROBOT_WEB_CONTROL_FRAGMENT}\n")
    endforeach()
    string(REPLACE "\r\n" "\n" ROBOT_WEB_CONTROL_PAGE "${ROBOT_WEB_CONTROL_PAGE}")
    string(REPLACE "\r\n" "\n" ROBOT_WEB_CONTROL_STYLE "${ROBOT_WEB_CONTROL_STYLE}")
    string(REGEX REPLACE "\n$" "" ROBOT_WEB_CONTROL_PAGE "${ROBOT_WEB_CONTROL_PAGE}")
    string(REGEX REPLACE "\n$" "" ROBOT_WEB_CONTROL_STYLE "${ROBOT_WEB_CONTROL_STYLE}")
    string(REGEX REPLACE "\n$" "" ROBOT_WEB_CONTROL_SCRIPT "${ROBOT_WEB_CONTROL_SCRIPT}")
    string(REPLACE "\r" "" ROBOT_WEB_CONTROL_FAVICON "${ROBOT_WEB_CONTROL_FAVICON}")
    string(REPLACE "\n" "" ROBOT_WEB_CONTROL_FAVICON "${ROBOT_WEB_CONTROL_FAVICON}")
    string(REPLACE "%" "%25" ROBOT_WEB_CONTROL_FAVICON "${ROBOT_WEB_CONTROL_FAVICON}")
    string(REPLACE "#" "%23" ROBOT_WEB_CONTROL_FAVICON "${ROBOT_WEB_CONTROL_FAVICON}")
    string(REPLACE "<" "%3C" ROBOT_WEB_CONTROL_FAVICON "${ROBOT_WEB_CONTROL_FAVICON}")
    string(REPLACE ">" "%3E" ROBOT_WEB_CONTROL_FAVICON "${ROBOT_WEB_CONTROL_FAVICON}")
    string(REPLACE " " "%20" ROBOT_WEB_CONTROL_FAVICON "${ROBOT_WEB_CONTROL_FAVICON}")
    string(REPLACE "\"" "%22" ROBOT_WEB_CONTROL_FAVICON "${ROBOT_WEB_CONTROL_FAVICON}")
    string(REPLACE
        "    <link rel=\"icon\" type=\"image/svg+xml\" href=\"favicon.svg\" />"
        "    <link rel=\"icon\" type=\"image/svg+xml\" href=\"data:image/svg+xml,${ROBOT_WEB_CONTROL_FAVICON}\" />"
        ROBOT_WEB_CONTROL_PAGE
        "${ROBOT_WEB_CONTROL_PAGE}"
    )
    string(REPLACE
        "    <link rel=\"stylesheet\" href=\"style.css\" />"
        "    <style>\n${ROBOT_WEB_CONTROL_STYLE}\n    </style>"
        ROBOT_WEB_CONTROL_PAGE
        "${ROBOT_WEB_CONTROL_PAGE}"
    )
    foreach(ROBOT_WEB_JS_FILE IN LISTS ROBOT_WEB_JS_FILES)
        file(RELATIVE_PATH ROBOT_WEB_JS_RELATIVE "${ROBOT_WEB_UI_DIR}" "${ROBOT_WEB_JS_FILE}")
        string(REPLACE
            "    <script src=\"${ROBOT_WEB_JS_RELATIVE}\"></script>\n"
            ""
            ROBOT_WEB_CONTROL_PAGE
            "${ROBOT_WEB_CONTROL_PAGE}"
        )
    endforeach()
    string(REPLACE
        "    <!-- ROBOT_WEB_CONTROL_SCRIPT -->"
        "    <script>\n${ROBOT_WEB_CONTROL_SCRIPT}\n    </script>"
        ROBOT_WEB_CONTROL_PAGE
        "${ROBOT_WEB_CONTROL_PAGE}"
    )
    if(ROBOT_WEB_CONTROL_PAGE MATCHES "(style\\.css|favicon\\.svg|<script src=|ROBOT_WEB_CONTROL_SCRIPT)")
        message(FATAL_ERROR "Web Control UI assembly failed")
    endif()

    set(ROBOT_WEB_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${ROBOT_WEB_GENERATED_DIR}")
    configure_file(
        "${CMAKE_CURRENT_LIST_DIR}/robot/web/robot_web_control_page.h.in"
        "${ROBOT_WEB_GENERATED_DIR}/robot_web_control_page.h"
        @ONLY
    )
    list(APPEND INCLUDE_DIRS "${ROBOT_WEB_GENERATED_DIR}")
endif()

# Select language directory according to Kconfig
if(CONFIG_LANGUAGE_EN_US)
    set(LANG_DIR "en-US")
elseif(CONFIG_LANGUAGE_VI_VN)
    set(LANG_DIR "vi-VN")
endif()

# Define generation path
set(LANG_JSON "${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/${LANG_DIR}/language.json")
set(LANG_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/assets/lang_config.h")

# Collect current language audio files
file(GLOB LANG_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/${LANG_DIR}/*.ogg)

# If not en-US, collect en-US audio files as fallback for missing files
if(NOT LANG_DIR STREQUAL "en-US")
    file(GLOB EN_US_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/locales/en-US/*.ogg)

    # Extract filenames (without path) from current language
    set(EXISTING_NAMES "")
    foreach(SOUND_FILE ${LANG_SOUNDS})
        get_filename_component(FILENAME ${SOUND_FILE} NAME)
        list(APPEND EXISTING_NAMES ${FILENAME})
    endforeach()

    # Only add en-US audio files that are missing in current language
    foreach(EN_SOUND ${EN_US_SOUNDS})
        get_filename_component(FILENAME ${EN_SOUND} NAME)
        if(NOT ${FILENAME} IN_LIST EXISTING_NAMES)
            list(APPEND LANG_SOUNDS ${EN_SOUND})
            message(STATUS "Using en-US fallback for missing audio: ${FILENAME}")
        endif()
    endforeach()
endif()

file(GLOB COMMON_SOUNDS ${CMAKE_CURRENT_SOURCE_DIR}/assets/common/*.ogg)

idf_component_register(SRCS ${SOURCES}
                    EMBED_FILES ${LANG_SOUNDS} ${COMMON_SOUNDS}
                    INCLUDE_DIRS ${INCLUDE_DIRS}
                    WHOLE_ARCHIVE
                    PRIV_REQUIRES
                        esp_pm
                        esp_psram
                        esp_driver_gpio
                        esp_driver_spi
                        esp_driver_i2c
                        esp_driver_i2s
                        esp_driver_ledc
                        esp_app_format
                        app_update
                        spi_flash
                        mbedtls
                        efuse
                        xiaozhi-fonts
                        esp_http_server
                    )

# Keep project-owned C++ on GNU C++23. ESP-IDF 6 may default to a newer
# dialect; pin the baseline so main/ does not depend on it.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++23>)
endif()

# Define the compatibility-sensitive reported product identity.
string(REGEX MATCH "^font_noto_sans_basic_([0-9]+)_([0-9]+)$" _font_match "${BUILTIN_TEXT_FONT}")
if(NOT _font_match)
    message(FATAL_ERROR "Unsupported built-in text font name: ${BUILTIN_TEXT_FONT}")
endif()
set(TEXT_FONT_SIZE "${CMAKE_MATCH_1}")
set(TEXT_FONT_BPP "${CMAKE_MATCH_2}")
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    target_compile_definitions(${COMPONENT_LIB}
                        PRIVATE BOARD_TYPE=\"${BOARD_TYPE}\" BOARD_NAME=\"${BOARD_NAME}\"
                        PRIVATE BOARD_MANUFACTURER=\"${BOARD_MANUFACTURER}\"
                        PRIVATE XIAOZHI_DISABLE_OFFICIAL_FIRMWARE_UPGRADE=1
                        PRIVATE BUILTIN_TEXT_FONT=${BUILTIN_TEXT_FONT} BUILTIN_ICON_FONT=${BUILTIN_ICON_FONT}
                        PRIVATE TEXT_FONT_SIZE=${TEXT_FONT_SIZE} TEXT_FONT_BPP=${TEXT_FONT_BPP}
                        )
    # ESP32-S3 LVGL builtin pool: PSRAM boards allocate it from PSRAM.
    # A configuration with CONFIG_SPIRAM=n must not keep the 1MB pool as a
    # .dram0.bss array; use CLIB malloc for such a configuration.
    if(CONFIG_IDF_TARGET_ESP32S3)
        idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
        if(lvgl_lib)
            target_include_directories(${lvgl_lib} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/display")
            if(CONFIG_SPIRAM)
                target_compile_definitions(${lvgl_lib} PRIVATE
                    "LV_MEM_POOL_INCLUDE=\"lvgl_psram_pool.h\"")
            elseif(CONFIG_LV_USE_BUILTIN_MALLOC)
                target_compile_definitions(${lvgl_lib} PRIVATE
                    "LV_MEM_POOL_INCLUDE=\"lvgl_no_psram_pool.h\"")
            endif()
        endif()
    endif()
endif()

# Add generation rules
add_custom_command(
    OUTPUT ${LANG_HEADER}
    COMMAND python ${PROJECT_DIR}/scripts/gen_lang.py
            --language "${LANG_DIR}"
            --output "${LANG_HEADER}"
    DEPENDS
        ${LANG_JSON}
        ${PROJECT_DIR}/scripts/gen_lang.py
    COMMENT "Generating ${LANG_DIR} language config"
)

# Force build generation dependencies
add_custom_target(lang_header ALL
    DEPENDS ${LANG_HEADER}
)

# Find ESP-SR component dynamically
find_component_by_pattern("espressif__esp-sr" ESP_SR_COMPONENT ESP_SR_COMPONENT_PATH)
if(ESP_SR_COMPONENT_PATH)
    set(ESP_SR_MODEL_PATH "${ESP_SR_COMPONENT_PATH}/model")
endif()

# Find xiaozhi-fonts component dynamically
find_component_by_pattern("xiaozhi-fonts" NOTO_FONTS_COMPONENT NOTO_FONTS_COMPONENT_PATH)
if(NOTO_FONTS_COMPONENT_PATH)
    set(NOTO_FONTS_PATH "${NOTO_FONTS_COMPONENT_PATH}")
endif()

# Function to build default assets based on configuration
function(build_default_assets_bin)
    # Set output path for generated assets.bin
    set(GENERATED_ASSETS_BIN "${CMAKE_BINARY_DIR}/generated_assets.bin")
    set(DEFAULT_ASSETS_DEPENDENCIES
        "${SDKCONFIG}"
        "${PROJECT_DIR}/scripts/build_default_assets.py"
    )

    # Prepare arguments for build script
    set(BUILD_ARGS
        "--sdkconfig" "${SDKCONFIG}"
        "--output" "${GENERATED_ASSETS_BIN}"
    )

    # Add builtin text font if defined
    if(BUILTIN_TEXT_FONT)
        list(APPEND BUILD_ARGS "--builtin_text_font" "${BUILTIN_TEXT_FONT}")
        string(REPLACE "_basic_" "_common_" COMMON_TEXT_FONT "${BUILTIN_TEXT_FONT}")
        set(COMMON_TEXT_FONT_FILE "${NOTO_FONTS_PATH}/cbin/${COMMON_TEXT_FONT}.bin")
        if(EXISTS "${COMMON_TEXT_FONT_FILE}")
            list(APPEND DEFAULT_ASSETS_DEPENDENCIES "${COMMON_TEXT_FONT_FILE}")
        endif()
        if(EXISTS "${NOTO_FONTS_PATH}/manifest.json")
            list(APPEND DEFAULT_ASSETS_DEPENDENCIES "${NOTO_FONTS_PATH}/manifest.json")
        endif()
    endif()

    # Add default assets extra files if defined
    if(DEFAULT_ASSETS_EXTRA_FILES)
        list(APPEND BUILD_ARGS "--extra_files" "${DEFAULT_ASSETS_EXTRA_FILES}")
    endif()

    list(APPEND BUILD_ARGS "--esp_sr_model_path" "${ESP_SR_MODEL_PATH}")
    list(APPEND BUILD_ARGS "--noto_fonts_path" "${NOTO_FONTS_PATH}")

    # Enforce assets.bin fits the flash partition (e.g. ESP-HI 4MB map uses 1MB assets).
    partition_table_get_partition_info(assets_partition_size "--partition-name assets" "size")
    if(assets_partition_size)
        list(APPEND BUILD_ARGS "--max_size" "${assets_partition_size}")
    endif()

    # Create custom command to build assets
    add_custom_command(
        OUTPUT ${GENERATED_ASSETS_BIN}
        COMMAND python ${PROJECT_DIR}/scripts/build_default_assets.py ${BUILD_ARGS}
        DEPENDS ${DEFAULT_ASSETS_DEPENDENCIES}
        COMMENT "Building default assets.bin based on configuration"
        VERBATIM
    )

    # Create target for generated assets
    add_custom_target(generated_default_assets ALL
        DEPENDS ${GENERATED_ASSETS_BIN}
    )

    # Set the generated file path in parent scope
    set(GENERATED_ASSETS_LOCAL_FILE ${GENERATED_ASSETS_BIN} PARENT_SCOPE)

    message(STATUS "Default assets build configured: ${GENERATED_ASSETS_BIN}")
endfunction()


# Function to get local assets file path (handles both URL and local file)
function(get_assets_local_file assets_source assets_local_file_var)
    # Check if it's a URL (starts with http:// or https://)
    if(assets_source MATCHES "^https?://")
        # It's a URL, download it
        get_filename_component(ASSETS_FILENAME "${assets_source}" NAME)
        set(ASSETS_LOCAL_FILE "${CMAKE_BINARY_DIR}/${ASSETS_FILENAME}")
        set(ASSETS_TEMP_FILE "${CMAKE_BINARY_DIR}/${ASSETS_FILENAME}.tmp")

        # Check if local file exists
        if(EXISTS ${ASSETS_LOCAL_FILE})
            message(STATUS "Assets file ${ASSETS_FILENAME} already exists, skipping download")
        else()
            message(STATUS "Downloading ${ASSETS_FILENAME}")

            # Clean up any existing temp file
            if(EXISTS ${ASSETS_TEMP_FILE})
                file(REMOVE ${ASSETS_TEMP_FILE})
            endif()

            # Download to temporary file first
            file(DOWNLOAD ${assets_source} ${ASSETS_TEMP_FILE}
                 STATUS DOWNLOAD_STATUS)
            list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
            if(NOT STATUS_CODE EQUAL 0)
                # Clean up temp file on failure
                if(EXISTS ${ASSETS_TEMP_FILE})
                    file(REMOVE ${ASSETS_TEMP_FILE})
                endif()
                message(FATAL_ERROR "Failed to download ${ASSETS_FILENAME} from ${assets_source}")
            endif()

            # Move temp file to final location (atomic operation)
            file(RENAME ${ASSETS_TEMP_FILE} ${ASSETS_LOCAL_FILE})
            message(STATUS "Successfully downloaded ${ASSETS_FILENAME}")
        endif()
    else()
        # It's a local file path
        if(IS_ABSOLUTE "${assets_source}")
            set(ASSETS_LOCAL_FILE "${assets_source}")
        else()
            set(ASSETS_LOCAL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/${assets_source}")
        endif()

        # Check if local file exists
        if(NOT EXISTS ${ASSETS_LOCAL_FILE})
            message(FATAL_ERROR "Assets file not found: ${ASSETS_LOCAL_FILE}")
        endif()

        message(STATUS "Using assets file: ${ASSETS_LOCAL_FILE}")
    endif()

    set(${assets_local_file_var} ${ASSETS_LOCAL_FILE} PARENT_SCOPE)
endfunction()


partition_table_get_partition_info(size "--partition-name assets" "size")
partition_table_get_partition_info(offset "--partition-name assets" "offset")
if ("${size}" AND "${offset}")
    # Flash assets based on configuration
    if(CONFIG_FLASH_DEFAULT_ASSETS)
        # Build default assets based on configuration
        build_default_assets_bin()
        esptool_py_flash_to_partition(flash "assets" "${GENERATED_ASSETS_LOCAL_FILE}")
        message(STATUS "Generated default assets flash configured: ${GENERATED_ASSETS_LOCAL_FILE} -> assets partition")
    elseif(CONFIG_FLASH_CUSTOM_ASSETS)
        # Flash custom assets
        get_assets_local_file("${CONFIG_CUSTOM_ASSETS_FILE}" ASSETS_LOCAL_FILE)
        esptool_py_flash_to_partition(flash "assets" "${ASSETS_LOCAL_FILE}")
        message(STATUS "Custom assets flash configured: ${ASSETS_LOCAL_FILE} -> assets partition")
    elseif(CONFIG_FLASH_NONE_ASSETS)
        message(STATUS "Assets flashing disabled (FLASH_NONE_ASSETS)")
    endif()
else()
    message(STATUS "Assets partition not found, using v1 partition table")
endif()
