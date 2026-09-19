cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "poc-0.1")

# Target device: -DBOARD=c606 (default), c606pro or c706. Picks the extra
# sdkconfig defaults (flash size, PSRAM mode, partition table, board choice)
# and the image name; use a separate build dir per board (-B build-c706).
if(NOT DEFINED BOARD)
    set(BOARD "c606")
endif()
if(NOT BOARD STREQUAL "c606")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.${BOARD}")
    if(NOT DEFINED SDKCONFIG)
        set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${BOARD}")
    endif()
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${BOARD}_oss)
