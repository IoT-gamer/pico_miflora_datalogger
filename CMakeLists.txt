# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico_w CACHE STRING "Board type")

# --- Added from sd_card_fatfs example ---
include(FetchContent)
FetchContent_Declare(
    sd_card_pico
    GIT_REPOSITORY https://github.com/carlk3/no-OS-FatFS-SD-SDIO-SPI-RPi-Pico.git
    GIT_TAG main
)
# Find out where FetchContent will put the files
FetchContent_GetProperties(sd_card_pico)
# If the files aren't already downloaded, download them
if(NOT sd_card_pico_POPULATED)
    FetchContent_Populate(sd_card_pico)
endif()
# ----------------------------------------


# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(pico_miflora_datalogger C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# --- Added from sd_card_fatfs example ---
add_subdirectory(${sd_card_pico_SOURCE_DIR}/src ${sd_card_pico_BINARY_DIR}/src)
# ----------------------------------------

# Add executable.
# We now include main.c and the hw_config.c for the SD card
add_executable(pico_miflora_datalogger
    main.c
    hw_config.c
)

target_compile_definitions(pico_miflora_datalogger PRIVATE
    RUNNING_AS_CLIENT=1
)

pico_set_program_name(pico_miflora_datalogger "pico_miflora_datalogger")
pico_set_program_version(pico_miflora_datalogger "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(pico_miflora_datalogger 0)
pico_enable_stdio_usb(pico_miflora_datalogger 1)

# Add the standard library to the build
# This now links libraries from BOTH projects
target_link_libraries(pico_miflora_datalogger
        PRIVATE
        # From miflora scanner 
        pico_stdlib
        pico_cyw43_arch_none
        pico_btstack_ble
        pico_btstack_cyw43
        
        # From sd_card_fatfs 
        hardware_pio
        hardware_dma
        hardware_spi
        no-OS-FatFS-SD-SDIO-SPI-RPi-Pico
)

target_include_directories(pico_miflora_datalogger PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(pico_miflora_datalogger)