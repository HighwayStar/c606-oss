cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "poc-0.1")

# Target device: -DBOARD=c606 (default), c606pro, cc700pro or c706. Picks the extra
# sdkconfig defaults (flash size, PSRAM mode, partition table, board choice)
# and the image name; use a separate build dir per board (-B build-c706).
if(NOT DEFINED BOARD)
    set(BOARD "c606")
endif()
set(SUPPORTED_BOARDS c606 c606pro cc700pro c706)
if(NOT BOARD IN_LIST SUPPORTED_BOARDS)
    message(FATAL_ERROR "Unknown BOARD '${BOARD}'. Supported: ${SUPPORTED_BOARDS}")
endif()
if(NOT BOARD STREQUAL "c606")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.${BOARD}")
    if(NOT DEFINED SDKCONFIG)
        set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${BOARD}")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${BOARD}_oss)

# Existing sdkconfig values override defaults. Catch a reused configuration
# before producing an image named for one device with another device's pins.
idf_build_get_property(board_sdkconfig SDKCONFIG_CMAKE)
include(${board_sdkconfig})
string(TOUPPER "${BOARD}" board_upper)
if(NOT CONFIG_C606OSS_BOARD_${board_upper})
    message(FATAL_ERROR
        "BOARD=${BOARD} does not match the board selected in sdkconfig. "
        "Use a separate build directory and a matching SDKCONFIG file.")
endif()
