idf_component_register(
    SRCS 
        "main.c"
        "can/can_decode.c"
        "can/can_manager.c"
        "can/can_bus_test.c"
        "can/can_id_tracker.c"
        "can/obd2.c"
        "can/obd2_pids.c"
        "can/obd2_dtc_db.c"
        "can/dtc_monitor.c"
        "storage/boot_assets.c"
        "storage/config_store.c"
        "storage/sd_manager.c"
        "storage/data_logger.c"
        "storage/can_raw_logger.c"
        "storage/can_upload.c"
        "storage/signal_replay.c"
        "storage/user_signals.c"
        "io/wire_inputs.c"
        "net/ota_handler.c"
        "net/ota_update_dialog.c"
        "net/web_server.c"
        "net/web_server_captive.c"
        "net/web_server_gear.c"
        "net/web_server_touch.c"
        "net/web_server_capture.c"
        "net/web_server_signals.c"
        "net/web_server_assets.c"
        "net/web_server_wifi.c"
        "net/web_server_ota.c"
        "net/web_server_system.c"
        "net/web_server_logger.c"
        "net/web_server_layout.c"
        "net/web_server_obd2.c"
        "net/wifi_manager.c"
        "net/uart_protocol.c"
        "net/serial_protocol.c"
        "net/serial_commands.c"
        "system/device_id.c"
        "system/screen_config.c"
        "system/display_capture.c"
        "system/night_mode.c"
        "system/remote_touch.c"
        "system/crash_log.c"
        "system/heap_monitor.c"
        "ui/ui.c"
        "ui/dashboard.c"
        "ui/ui_styles.c"
        "ui/ui_helpers.c"
        "ui/signals.c"
        "ui/settings/preset_picker.c"
        "ui/settings/device_settings.c"
        "ui/settings/settings_panel.c"
        "ui/settings/ui_gear_setup.c"
        "ui/components/ui_comp.c"
        "ui/components/ui_comp_hook.c"
        "ui/components/ui_comp_slider2.c"
        "ui/callbacks/ui_callbacks.c"
"ui/menu/config_modal.c"
        "ui/menu/edit_mode.c"
        "ui/menu/inspector.c"
        "ui/menu/menu_screen.c"
        "ui/menu/obd2_picker.c"
        "ui/menu/dtc_reader.c"
        "ui/screens/ui_Screen3.c"
        "widgets/widget_warning.c"
        "widgets/widget_indicator.c"
"widgets/widget_bar.c"
        "widgets/widget_panel.c"
        "widgets/widget_text.c"
        "widgets/widget_meter.c"
        "widgets/widget_rpm_bar.c"
        "widgets/widget_registry.c"
        "widgets/widget_image.c"
        "widgets/widget_types.c"
        "widgets/widget_shape_panel.c"
        "widgets/widget_arc.c"
        "widgets/widget_toggle.c"
        "widgets/widget_button.c"
        "widgets/widget_shift_light.c"
        "widgets/widget_line.c"
        "widgets/widget_rules.c"
        "widgets/widget_fields.c"
        "widgets/widget_fields.gen.c"
        "widgets/font_manager.c"
        "widgets/signal_internal.c"
        "widgets/signal.c"
        "widgets/signal_sim.c"
        "layout/layout_manager.c"
        "layout/default_layout.c"
        "layout/layout_loader.c"
        "layout/ecu_presets.c"
        "ui/screens/splash_screen.c"
        "ui/screens/ui_wifi.c"
        "ui/screens/ui_diagnostics.c"
        "ui/screens/ui_peaks.c"
        "ui/screens/ui_can_list.c"
        "ui/screens/first_run_wizard.c"
        "ui/screens/ui_ecu_picker.c"
        "ui/fonts/ui_font_fugaz_14.c"
        "ui/fonts/ui_font_fugaz_17.c"
        "ui/fonts/ui_font_fugaz_28.c"
        "ui/fonts/ui_font_fugaz_56.c"
        "ui/fonts/ui_font_Manrope_35_BOLD.c"
        "ui/fonts/ui_font_Manrope_54_BOLD.c"
        "ui/images/ui_img_rev_0_sticker_png.c"
        "ui/images/ui_img_rev_0_sticker2_png.c"
        "ui/images/ui_img_rev_0_sticker3_png.c"
        "ui/images/ui_temporary_image.c"
        "ui/images/ui_img_indicator_left_png.c"
        "ui/images/ui_img_indicator_right_png.c"
        "ui/images/ui_img_363163260.c"
        "ui/images/ui_img_648933004.c"
        "ui/images/ui_img_669363031.c"
        "ui/images/ui_img_rdm_logo_dash_png.c"
        "ui/images/ui_img_RDM_Light.c"
        "ui/images/ui_img_RDM_Dark.c"
        "ui/images/ui_img_RDM_Dark_2.c"
        "ui/images/ui_img_Smart_Car_Key.c"
    INCLUDE_DIRS 
        "."
        "./include"
        "can"
        "net"
        "storage"
        "io"
        "system"
        "ui"
        "ui/components"
        "ui/screens"
        "ui/fonts"
        "ui/images"
        "ui/settings"
        "ui/menu"
        "widgets"
        "layout"
        "design_mode"
        "web"
    REQUIRES 
        "driver"
        "esp_adc"
        "esp_lcd"
        "esp_lcd_touch"
        "esp_lcd_touch_gt911"
        "esp_timer"
        "lvgl"
        "nvs_flash"
        "fatfs"
        "esp_wifi"
        "esp_http_client"
        "json"
        "esp_https_ota"
        "app_update"
        "mbedtls"
        "esp-tls"
        "esp_http_server"
        "joltwallet__littlefs"
    EMBED_FILES "embed/RDM.rdmimg" "web/favicon.ico" "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz"
)

# ─── Gzip the web editor at configure time ─────────────────────────────────
#
# web/index.html is 14.5k lines / ~825 KB. Embedding it raw eats most of the
# OTA partition's slack. Gzipping at build time gets it to ~150 KB on the
# wire AND inside the firmware image — every modern browser handles
# Content-Encoding: gzip transparently.
#
# Why configure-time and not build-time:
#   - EMBED_FILES verifies file existence during CMake configure, so the
#     .gz must already be on disk when idf_component_register runs.
#   - execute_process runs synchronously during configure, before
#     EMBED_FILES processing — gzip output is guaranteed available.
#   - CMAKE_CONFIGURE_DEPENDS re-triggers configure whenever index.html
#     changes, so the .gz stays in sync with the source automatically.
#   - File is small (~825 KB → ~150 KB) so re-running gzip per configure
#     is a fraction of a second; no point in a build-time custom command
#     with cache invalidation logic.
#
# The Python one-liner uses gzip.compress(..., 9) for max compression and
# writes a standard .gz file. ${Python3_EXECUTABLE} is set up by the IDF
# build infrastructure (its bundled Python venv).
# ────────────────────────────────────────────────────────────────────────────
set(_RDM_INDEX_HTML_SRC "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html")
set(_RDM_INDEX_HTML_GZ  "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")

# Re-run CMake configure when index.html changes so the .gz gets regenerated.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${_RDM_INDEX_HTML_SRC}")

# The build-tree component dir is lazily created by ESP-IDF and may not exist
# yet when we're configuring — make sure it does before Python tries to write
# its output into it.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# Prefer Python3_EXECUTABLE if FindPython3 has been run by upstream code by
# this point; fall back to the bare "python" name (ESP-IDF's export script
# puts its venv first on PATH). Using a variable so the diagnostic below
# shows what we actually invoked.
if(Python3_EXECUTABLE)
    set(_RDM_PYTHON "${Python3_EXECUTABLE}")
else()
    set(_RDM_PYTHON "python")
endif()

execute_process(
    COMMAND "${_RDM_PYTHON}" -c
        "import gzip,sys; open(sys.argv[2],'wb').write(gzip.compress(open(sys.argv[1],'rb').read(), 9))"
        "${_RDM_INDEX_HTML_SRC}" "${_RDM_INDEX_HTML_GZ}"
    RESULT_VARIABLE _rdm_gzip_result
    OUTPUT_VARIABLE _rdm_gzip_stdout
    ERROR_VARIABLE  _rdm_gzip_stderr
)
if(NOT _rdm_gzip_result EQUAL 0)
    message(FATAL_ERROR
            "Failed to gzip ${_RDM_INDEX_HTML_SRC} -> ${_RDM_INDEX_HTML_GZ}\n"
            "  python: ${_RDM_PYTHON}\n"
            "  result: ${_rdm_gzip_result}\n"
            "  stderr: ${_rdm_gzip_stderr}")
endif()

target_compile_definitions(${COMPONENT_LIB} PUBLIC "-DLV_CONF_INCLUDE_SIMPLE")
target_compile_definitions(${COMPONENT_LIB} PUBLIC "-DLV_CONF_PATH=\"lv_conf.h\"")