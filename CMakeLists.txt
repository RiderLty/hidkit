cmake_minimum_required(VERSION 3.13)
project(hidkit C)

#
# hidkit 的构建只做两件事：
#   1) 把库暴露成 target `hidkit`（默认 INTERFACE：用消费方的工具链编译，零平台依赖）
#   2) 可选地构建主机侧样本测试（不需要任何硬件）
#
# 作为子目录被别的工程引用（Pico / ESP-IDF / STM32 / 纯 CMake 主机工程）：
#   add_subdirectory(lib/hidkit)
#   target_link_libraries(your_app PRIVATE hidkit)
#
# 容量与开关全部是宏，想改就在自己工程里：
#   target_compile_definitions(your_app PRIVATE HIDKIT_MAX_GAMEPADS=2)
# 或用 HIDKIT_USER_CONFIG 指向自己的配置头。
#

set(HIDKIT_SOURCES
    src/hidkit_dev.c
    src/hidkit_hooks.c
    src/hid_dispatch.c
    src/hid_parser.c
    src/gamepad.c
)

add_library(hidkit INTERFACE)
target_sources(hidkit INTERFACE ${HIDKIT_SOURCES})
target_include_directories(hidkit INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# 主机/非裸机集成想编译成静态库时打开（示例：cmake -DHIDKIT_BUILD_STATIC=ON）
option(HIDKIT_BUILD_STATIC "同时构建 hidkit_static 静态库" OFF)
if(HIDKIT_BUILD_STATIC)
    add_library(hidkit_static STATIC ${HIDKIT_SOURCES})
    target_include_directories(hidkit_static PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
endif()

# 主机侧样本测试（默认开；作为子模块被嵌入式工程引用时可 -DHIDKIT_BUILD_TESTS=OFF 关掉）
option(HIDKIT_BUILD_TESTS "构建主机侧样本测试" ON)
if(HIDKIT_BUILD_TESTS AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(tests/host)
endif()
