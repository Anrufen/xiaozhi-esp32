cmake_minimum_required(VERSION 3.16)
project(lvgl_sim C)

set(CMAKE_C_STANDARD 11)

execute_process(COMMAND sdl2-config --cflags OUTPUT_VARIABLE SDL2_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND sdl2-config --libs OUTPUT_VARIABLE SDL2_LDFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)

add_definitions(-DLV_CONF_INCLUDE_SIMPLE=1)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    /opt/homebrew/include
    /opt/homebrew/include/SDL2
    ${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/lvgl__lvgl
    ${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/lvgl__lvgl/src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/78__xiaozhi-fonts/include
)

file(GLOB_RECURSE LVGL_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/lvgl__lvgl/src/*.c"
)

set(FONT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/78__xiaozhi-fonts/src/font_noto_sans_basic_30_4.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/78__xiaozhi-fonts/src/font_noto_sans_basic_20_4.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/78__xiaozhi-fonts/src/font_noto_sans_basic_16_4.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/78__xiaozhi-fonts/src/font_material_symbols_16_4.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/78__xiaozhi-fonts/src/font_material_symbols_30_4.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/espressif2022__esp_emote_expression/src/default_assets/font_puhui_basic_20_4.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../managed_components/espressif2022__esp_emote_expression/src/default_assets/font_maison_neue_book_26.c
)

add_executable(lvgl_sim
    main.c
    ${LVGL_SRC}
    ${FONT_SOURCES}
)

target_link_libraries(lvgl_sim
    "-L/opt/homebrew/lib -lSDL2"
    "-framework OpenGL"
    "-framework Cocoa"
    "-framework IOKit"
    "-framework CoreVideo"
)
