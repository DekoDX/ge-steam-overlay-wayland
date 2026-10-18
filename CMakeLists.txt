cmake_minimum_required(VERSION 3.16)

project(steam_overlay_wayland
    VERSION 1.0.0
    LANGUAGES C CXX
)

include(GNUInstallDirs)

find_package(Threads REQUIRED)
find_package(X11 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(Wayland REQUIRED IMPORTED_TARGET wayland-client wayland-cursor)

find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner)
if(NOT WAYLAND_SCANNER_EXECUTABLE)
    message(FATAL_ERROR "wayland-scanner is required")
endif()

set(PROTOCOL_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/protocols")
file(MAKE_DIRECTORY "${PROTOCOL_GENERATED_DIR}")

set(PROTOCOL_SOURCES "")
foreach(protocol
        relative-pointer-unstable-v1
        viewporter
        xdg-output-unstable-v1)
    set(protocol_xml "${CMAKE_CURRENT_SOURCE_DIR}/protocols/${protocol}.xml")
    set(protocol_header "${PROTOCOL_GENERATED_DIR}/${protocol}-client-protocol.h")
    set(protocol_code "${PROTOCOL_GENERATED_DIR}/${protocol}-protocol.c")
    add_custom_command(
        OUTPUT "${protocol_header}"
        COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header
                "${protocol_xml}" "${protocol_header}"
        DEPENDS "${protocol_xml}"
        VERBATIM
    )
    add_custom_command(
        OUTPUT "${protocol_code}"
        COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code
                "${protocol_xml}" "${protocol_code}"
        DEPENDS "${protocol_xml}"
        VERBATIM
    )
    list(APPEND PROTOCOL_SOURCES "${protocol_header}" "${protocol_code}")
endforeach()

set(VULKAN_HEADERS_INCLUDE_DIR "" CACHE PATH "Directory containing Vulkan headers")
if(NOT EXISTS "${VULKAN_HEADERS_INCLUDE_DIR}/vulkan/vulkan.h")
    message(FATAL_ERROR "VULKAN_HEADERS_INCLUDE_DIR must contain vulkan/vulkan.h")
endif()

set(VKROOTS_INCLUDE_DIR "" CACHE PATH "Directory containing vkroots.h")
if(NOT EXISTS "${VKROOTS_INCLUDE_DIR}/vkroots.h")
    message(FATAL_ERROR "VKROOTS_INCLUDE_DIR must contain vkroots.h")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(LAYER_ARCH "64")
else()
    set(LAYER_ARCH "32")
endif()

set(FRONT_LAYER_NAME "VK_LAYER_GE_wayland_steam_overlay_front_${LAYER_ARCH}")
set(BACK_LAYER_NAME "VK_LAYER_GE_wayland_steam_overlay_back_${LAYER_ARCH}")
set(META_LAYER_NAME "VK_LAYER_GE_wayland_steam_overlay_${LAYER_ARCH}")
set(STEAM_COMPONENT_LAYER_NAME "VK_LAYER_GE_steam_overlay_component_${LAYER_ARCH}")

set(STEAM_OVERLAY_LIBRARY "" CACHE FILEPATH
    "Path to Steam's steamoverlayvulkanlayer.so for this architecture")
if(NOT STEAM_OVERLAY_LIBRARY)
    find_file(STEAM_OVERLAY_LIBRARY_DETECTED
        NAMES steamoverlayvulkanlayer.so
        PATHS
            "$ENV{HOME}/.local/share/Steam/ubuntu12_${LAYER_ARCH}"
            "$ENV{HOME}/.steam/root/ubuntu12_${LAYER_ARCH}"
            "$ENV{HOME}/.steam/steam/ubuntu12_${LAYER_ARCH}"
            "$ENV{HOME}/.var/app/com.valvesoftware.Steam/data/Steam/ubuntu12_${LAYER_ARCH}"
        NO_DEFAULT_PATH
    )
    if(STEAM_OVERLAY_LIBRARY_DETECTED)
        set(STEAM_OVERLAY_LIBRARY "${STEAM_OVERLAY_LIBRARY_DETECTED}"
            CACHE FILEPATH
            "Path to Steam's steamoverlayvulkanlayer.so for this architecture"
            FORCE)
    endif()
endif()
if(NOT EXISTS "${STEAM_OVERLAY_LIBRARY}")
    message(FATAL_ERROR
        "STEAM_OVERLAY_LIBRARY must point to Steam's "
        "steamoverlayvulkanlayer.so for this architecture")
endif()
get_filename_component(STEAM_OVERLAY_LIBRARY "${STEAM_OVERLAY_LIBRARY}" REALPATH)

add_library(VkLayer_GE_Wayland_SteamOverlay_Front SHARED
    ${PROTOCOL_SOURCES}
    src/focus_proxy.c
    src/game_input_gate.c
    src/steam_overlay_bridge.c
    src/vulkan_layer.cpp
    src/wayland_input.c
)

add_library(VkLayer_GE_Wayland_SteamOverlay_Back SHARED
    src/vulkan_translate_layer.c
)

target_compile_features(VkLayer_GE_Wayland_SteamOverlay_Front PRIVATE
    c_std_11
    cxx_std_20
)
target_compile_features(VkLayer_GE_Wayland_SteamOverlay_Back PRIVATE c_std_11)

target_compile_definitions(VkLayer_GE_Wayland_SteamOverlay_Front PRIVATE
    _GNU_SOURCE
    GE_STEAM_OVERLAY_BACK_LIBRARY="libVkLayer_GE_Wayland_SteamOverlay_Back.so"
)
target_compile_definitions(VkLayer_GE_Wayland_SteamOverlay_Back PRIVATE
    _GNU_SOURCE
)

target_compile_options(VkLayer_GE_Wayland_SteamOverlay_Front PRIVATE
    -Wall
    -Wextra
    -Wformat=2
    -Wshadow
    $<$<COMPILE_LANGUAGE:C>:-Wstrict-prototypes>
)
target_compile_options(VkLayer_GE_Wayland_SteamOverlay_Back PRIVATE
    -Wall
    -Wextra
    -Wformat=2
    -Wshadow
    -Wstrict-prototypes
)

target_include_directories(VkLayer_GE_Wayland_SteamOverlay_Front PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${PROTOCOL_GENERATED_DIR}
    ${VULKAN_HEADERS_INCLUDE_DIR}
    ${VKROOTS_INCLUDE_DIR}
    ${X11_INCLUDE_DIR}
)
target_include_directories(VkLayer_GE_Wayland_SteamOverlay_Back PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${VULKAN_HEADERS_INCLUDE_DIR}
    ${VKROOTS_INCLUDE_DIR}
)

target_link_libraries(VkLayer_GE_Wayland_SteamOverlay_Front PRIVATE
    PkgConfig::Wayland
    Threads::Threads
    ${X11_LIBRARIES}
    ${CMAKE_DL_LIBS}
    m
)
target_link_libraries(VkLayer_GE_Wayland_SteamOverlay_Back PRIVATE
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

set_target_properties(
    VkLayer_GE_Wayland_SteamOverlay_Front
    VkLayer_GE_Wayland_SteamOverlay_Back
    PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    LINK_OPTIONS "-Wl,-Bsymbolic"
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_GE_wayland_steam_overlay.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_GE_wayland_steam_overlay_${LAYER_ARCH}.json
    @ONLY
)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_GE_wayland_steam_overlay_front.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_GE_wayland_steam_overlay_front_${LAYER_ARCH}.json
    @ONLY
)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_GE_wayland_steam_overlay_back.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_GE_wayland_steam_overlay_back_${LAYER_ARCH}.json
    @ONLY
)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_GE_steam_overlay_component.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_GE_steam_overlay_component_${LAYER_ARCH}.json
    @ONLY
)

install(TARGETS
    VkLayer_GE_Wayland_SteamOverlay_Front
    VkLayer_GE_Wayland_SteamOverlay_Back
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/steam-overlay-wayland
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_GE_wayland_steam_overlay_${LAYER_ARCH}.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/vulkan/implicit_layer.d
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_GE_wayland_steam_overlay_front_${LAYER_ARCH}.json
    ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_GE_wayland_steam_overlay_back_${LAYER_ARCH}.json
    ${CMAKE_CURRENT_BINARY_DIR}/VkLayer_GE_steam_overlay_component_${LAYER_ARCH}.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/vulkan/explicit_layer.d
)
