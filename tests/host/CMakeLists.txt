# 主机侧样本测试：把测试目标和库源码一起编译（hidkit 是 INTERFACE target，
# 源文件会随 INTERFACE_SOURCES 一起进来）。不需要任何硬件或 USB 栈。
add_executable(hidkit_host_tests test_hidkit.c)
target_link_libraries(hidkit_host_tests PRIVATE hidkit)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(hidkit_host_tests PRIVATE -Wall -Wextra -Werror)
    # 未对齐访问与内存错误在主机上就能抓到（Cortex-M0 上会 HardFault 的那类问题）
    target_compile_options(hidkit_host_tests PRIVATE
        -fsanitize=address,undefined,alignment -fno-omit-frame-pointer)
    target_link_options(hidkit_host_tests PRIVATE
        -fsanitize=address,undefined,alignment)
endif()

add_test(NAME hidkit_host_samples COMMAND hidkit_host_tests)
