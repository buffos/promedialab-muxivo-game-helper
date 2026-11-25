cmake_minimum_required(VERSION 3.20)
project(MuxivoGameCapture LANGUAGES CXX)

include(CTest)

if(NOT WIN32)
    message(FATAL_ERROR "MuxivoGameCapture supports Windows only.")
endif()

if(NOT DEFINED DETOURS_SOURCE_DIR)
    message(FATAL_ERROR "DETOURS_SOURCE_DIR must point to the pinned Microsoft Detours checkout.")
endif()
file(TO_CMAKE_PATH "${DETOURS_SOURCE_DIR}" DETOURS_CMAKE_DIR)
if(NOT EXISTS "${DETOURS_CMAKE_DIR}/src/detours.cpp")
    message(FATAL_ERROR "Microsoft Detours source not found at ${DETOURS_CMAKE_DIR}/src/detours.cpp")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(MUXIVO_ARCH_SUFFIX "x64")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(MUXIVO_ARCH_SUFFIX "x86")
else()
    message(FATAL_ERROR "Unsupported pointer width: ${CMAKE_SIZEOF_VOID_P}")
endif()

add_library(muxivo-detours STATIC
    "${DETOURS_CMAKE_DIR}/src/detours.cpp"
    "${DETOURS_CMAKE_DIR}/src/disasm.cpp"
    "${DETOURS_CMAKE_DIR}/src/modules.cpp"
    "${DETOURS_CMAKE_DIR}/src/image.cpp")
target_compile_features(muxivo-detours PRIVATE cxx_std_17)
set_property(TARGET muxivo-detours PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_include_directories(muxivo-detours PUBLIC "${DETOURS_CMAKE_DIR}/src")
if(MSVC)
    target_compile_options(muxivo-detours PRIVATE /W0)
endif()

# Never statically link d3d9.lib in the D3D9 hook DLL so it only binds to d3d9.dll
# if already loaded in the target game process.
add_library(muxivo-d3d9-game-capture-${MUXIVO_ARCH_SUFFIX} SHARED
    d3d9/dll_main.cpp
    d3d9/d3d9_hook.cpp
    d3d9/present_capture.cpp
    d3d9/present_readback.cpp
    d3d9/d3d9_hook.def)
target_compile_features(muxivo-d3d9-game-capture-${MUXIVO_ARCH_SUFFIX} PRIVATE cxx_std_17)
set_property(TARGET muxivo-d3d9-game-capture-${MUXIVO_ARCH_SUFFIX}
    PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_definitions(muxivo-d3d9-game-capture-${MUXIVO_ARCH_SUFFIX} PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(muxivo-d3d9-game-capture-${MUXIVO_ARCH_SUFFIX} PRIVATE /W4 /permissive-)
endif()
target_include_directories(muxivo-d3d9-game-capture-${MUXIVO_ARCH_SUFFIX} PRIVATE include common d3d9)
target_link_libraries(muxivo-d3d9-game-capture-${MUXIVO_ARCH_SUFFIX} PRIVATE
    muxivo-detours advapi32 user32)

# Never statically link opengl32.lib in the OpenGL hook DLL; resolve all WGL/GL
# entry points dynamically from the process's loaded opengl32.dll and gdi32.dll.
add_library(muxivo-opengl-game-capture-${MUXIVO_ARCH_SUFFIX} SHARED
    opengl/dll_main.cpp
    opengl/opengl_hook.cpp
    opengl/swap_capture.cpp
    opengl/opengl_hook.def)
target_compile_features(muxivo-opengl-game-capture-${MUXIVO_ARCH_SUFFIX} PRIVATE cxx_std_17)
set_property(TARGET muxivo-opengl-game-capture-${MUXIVO_ARCH_SUFFIX}
    PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_definitions(muxivo-opengl-game-capture-${MUXIVO_ARCH_SUFFIX} PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(muxivo-opengl-game-capture-${MUXIVO_ARCH_SUFFIX} PRIVATE /W4 /permissive-)
endif()
target_include_directories(muxivo-opengl-game-capture-${MUXIVO_ARCH_SUFFIX} PRIVATE include common opengl)
target_link_libraries(muxivo-opengl-game-capture-${MUXIVO_ARCH_SUFFIX} PRIVATE
    muxivo-detours advapi32 user32)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(NOT DEFINED VULKAN_LAYER_INCLUDE_DIR)
        find_path(VULKAN_LAYER_INCLUDE_DIR vulkan/vk_layer.h
            HINTS "$ENV{VULKAN_SDK}/Include" REQUIRED)
    endif()

    add_library(muxivo-vulkan-game-capture SHARED
        vulkan/frame_format.cpp
        vulkan/layer.cpp
        vulkan/readback.cpp
        vulkan/layer.def)
    target_compile_features(muxivo-vulkan-game-capture PRIVATE cxx_std_17)
    set_property(TARGET muxivo-vulkan-game-capture PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(muxivo-vulkan-game-capture PRIVATE
        VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(muxivo-vulkan-game-capture PRIVATE /W4 /permissive-)
    endif()
    target_include_directories(muxivo-vulkan-game-capture PRIVATE "${VULKAN_LAYER_INCLUDE_DIR}" common native)
    target_link_libraries(muxivo-vulkan-game-capture PRIVATE advapi32 user32)

    add_library(muxivo-d3d11-game-capture SHARED
        d3d11/dll_main.cpp
        d3d11/dxgi_hook.cpp
        d3d11/d3d11_readback.cpp
        d3d11/d3d11_hook.def)
    target_compile_features(muxivo-d3d11-game-capture PRIVATE cxx_std_17)
    set_property(TARGET muxivo-d3d11-game-capture PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(muxivo-d3d11-game-capture PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(muxivo-d3d11-game-capture PRIVATE /W4 /permissive-)
    endif()
    target_include_directories(muxivo-d3d11-game-capture PRIVATE common native)
    # Never link d3d11.lib or dxgi.lib so the hook DLL does not force-load graphics DLLs.
    target_link_libraries(muxivo-d3d11-game-capture PRIVATE muxivo-detours advapi32 user32)

    # Never link d3d12.lib, d3d11.lib, or dxgi.lib into the D3D12 hook DLL.
    add_library(muxivo-d3d12-game-capture-x64 SHARED
        d3d12/dll_main.cpp
        d3d12/d3d12_hook.cpp
        d3d12/present_capture.cpp
        d3d12/present_readback.cpp
        d3d12/d3d12_hook.def)
    target_compile_features(muxivo-d3d12-game-capture-x64 PRIVATE cxx_std_17)
    set_property(TARGET muxivo-d3d12-game-capture-x64
        PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(muxivo-d3d12-game-capture-x64 PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(muxivo-d3d12-game-capture-x64 PRIVATE /W4 /permissive-)
    endif()
    target_include_directories(muxivo-d3d12-game-capture-x64 PRIVATE include common d3d12)
    target_link_libraries(muxivo-d3d12-game-capture-x64 PRIVATE
        muxivo-detours advapi32 user32)

    add_library(muxivo-capture-controller STATIC
        controller/adapter_controller.cpp
        d3d9/d3d9_offsets.cpp
        d3d12/d3d12_offsets.cpp)
    target_compile_features(muxivo-capture-controller PUBLIC cxx_std_17)
    set_property(TARGET muxivo-capture-controller
        PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(muxivo-capture-controller PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(muxivo-capture-controller PRIVATE /W4 /permissive-)
    endif()
    target_include_directories(muxivo-capture-controller
        PUBLIC include
        PRIVATE common d3d9 d3d12)
    target_link_libraries(muxivo-capture-controller PUBLIC advapi32 user32)

    add_executable(muxivo-game-capture-host
        host/main.cpp
        host/dxgi_offsets.cpp
        host/injector.cpp)
    target_compile_features(muxivo-game-capture-host PRIVATE cxx_std_17)
    set_property(TARGET muxivo-game-capture-host PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(muxivo-game-capture-host PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(muxivo-game-capture-host PRIVATE /W4 /permissive-)
    endif()
    target_include_directories(muxivo-game-capture-host PRIVATE include common native)
    target_link_libraries(muxivo-game-capture-host PRIVATE muxivo-capture-controller advapi32 user32)

    add_custom_target(muxivo-vulkan-layer-manifest ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/vulkan/VK_LAYER_MUXIVO_game_capture.json"
            "$<TARGET_FILE_DIR:muxivo-vulkan-game-capture>/VK_LAYER_MUXIVO_game_capture.json"
        DEPENDS muxivo-vulkan-game-capture)

    if(BUILD_TESTING)
        add_executable(muxivo-vulkan-frame-format-tests tests/vulkan_frame_format_tests.cpp vulkan/frame_format.cpp)
        target_compile_features(muxivo-vulkan-frame-format-tests PRIVATE cxx_std_17)
        set_property(TARGET muxivo-vulkan-frame-format-tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        if(MSVC)
            target_compile_options(muxivo-vulkan-frame-format-tests PRIVATE /W4 /permissive-)
        endif()
        target_include_directories(muxivo-vulkan-frame-format-tests PRIVATE "${VULKAN_LAYER_INCLUDE_DIR}" vulkan)
        add_test(NAME vulkan_frame_format_tests COMMAND muxivo-vulkan-frame-format-tests)

        add_executable(muxivo-vulkan-swapchain-tests tests/vulkan_swapchain_tests.cpp vulkan/frame_format.cpp vulkan/readback.cpp)
        target_compile_features(muxivo-vulkan-swapchain-tests PRIVATE cxx_std_17)
        set_property(TARGET muxivo-vulkan-swapchain-tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_definitions(muxivo-vulkan-swapchain-tests PRIVATE
            VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        if(MSVC)
            target_compile_options(muxivo-vulkan-swapchain-tests PRIVATE /W4 /permissive-)
        endif()
        target_include_directories(muxivo-vulkan-swapchain-tests PRIVATE "${VULKAN_LAYER_INCLUDE_DIR}" common native)
        target_link_libraries(muxivo-vulkan-swapchain-tests PRIVATE advapi32 user32)
        add_test(NAME vulkan_swapchain_tests COMMAND muxivo-vulkan-swapchain-tests)

        add_executable(muxivo-d3d11-capture-tests
            tests/d3d11_capture_tests.cpp
            host/dxgi_offsets.cpp)
        target_compile_features(muxivo-d3d11-capture-tests PRIVATE cxx_std_17)
        set_property(TARGET muxivo-d3d11-capture-tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_definitions(muxivo-d3d11-capture-tests PRIVATE
            UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        if(MSVC)
            target_compile_options(muxivo-d3d11-capture-tests PRIVATE /W4 /permissive-)
        endif()
        target_include_directories(muxivo-d3d11-capture-tests PRIVATE common native host)
        target_link_libraries(muxivo-d3d11-capture-tests PRIVATE advapi32 user32)
        add_test(NAME d3d11_capture_tests COMMAND muxivo-d3d11-capture-tests)
        set_tests_properties(d3d11_capture_tests PROPERTIES
            ENVIRONMENT "MUXIVO_D3D11_FIXTURE_EXE=$<IF:$<BOOL:${MUXIVO_D3D11_FIXTURE_EXE}>,${MUXIVO_D3D11_FIXTURE_EXE},>;MUXIVO_CAPTURE_HOST_EXE=$<TARGET_FILE:muxivo-game-capture-host>")

        add_executable(muxivo-test-d3d9-x64 tests/test_d3d9_target.cpp)
        target_compile_features(muxivo-test-d3d9-x64 PRIVATE cxx_std_17)
        set_property(TARGET muxivo-test-d3d9-x64 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_definitions(muxivo-test-d3d9-x64 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        if(MSVC)
            target_compile_options(muxivo-test-d3d9-x64 PRIVATE /W4 /permissive-)
        endif()
        target_link_libraries(muxivo-test-d3d9-x64 PRIVATE d3d9 user32)

        add_executable(muxivo-test-d3d12-x64 tests/test_d3d12_target.cpp)
        target_compile_features(muxivo-test-d3d12-x64 PRIVATE cxx_std_17)
        set_property(TARGET muxivo-test-d3d12-x64 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_definitions(muxivo-test-d3d12-x64 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        if(MSVC)
            target_compile_options(muxivo-test-d3d12-x64 PRIVATE /W4 /permissive-)
        endif()
        target_link_libraries(muxivo-test-d3d12-x64 PRIVATE d3d12 dxgi user32)

        add_executable(muxivo-test-opengl-x64 tests/test_opengl_target.cpp)
        target_compile_features(muxivo-test-opengl-x64 PRIVATE cxx_std_17)
        set_property(TARGET muxivo-test-opengl-x64 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_definitions(muxivo-test-opengl-x64 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        if(MSVC)
            target_compile_options(muxivo-test-opengl-x64 PRIVATE /W4 /permissive-)
        endif()
        target_link_libraries(muxivo-test-opengl-x64 PRIVATE opengl32 gdi32 user32)

        add_executable(muxivo-adapter-tests tests/adapter_tests_x64.cpp)
        target_compile_features(muxivo-adapter-tests PRIVATE cxx_std_17)
        set_property(TARGET muxivo-adapter-tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_definitions(muxivo-adapter-tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        if(MSVC)
            target_compile_options(muxivo-adapter-tests PRIVATE /W4 /permissive-)
        endif()
        target_include_directories(muxivo-adapter-tests PRIVATE include common d3d9 d3d12)
        target_link_libraries(muxivo-adapter-tests PRIVATE muxivo-capture-controller advapi32 user32)

        add_test(NAME adapter_tests_x64 COMMAND muxivo-adapter-tests)
        set_tests_properties(adapter_tests_x64 PROPERTIES
            ENVIRONMENT "MUXIVO_TEST_D3D9_EXE=$<TARGET_FILE:muxivo-test-d3d9-x64>;MUXIVO_TEST_D3D12_EXE=$<TARGET_FILE:muxivo-test-d3d12-x64>;MUXIVO_TEST_OPENGL_EXE=$<TARGET_FILE:muxivo-test-opengl-x64>")
    endif()

    install(TARGETS
        muxivo-vulkan-game-capture
        muxivo-d3d11-game-capture
        muxivo-d3d9-game-capture-x64
        muxivo-d3d12-game-capture-x64
        muxivo-opengl-game-capture-x64
        muxivo-game-capture-host
        RUNTIME DESTINATION .)
    install(FILES vulkan/VK_LAYER_MUXIVO_game_capture.json DESTINATION .)
else()
    add_executable(muxivo-capture-attach-x86
        controller/attach_x86_main.cpp
        d3d9/d3d9_offsets.cpp)
    target_compile_features(muxivo-capture-attach-x86 PRIVATE cxx_std_17)
    set_property(TARGET muxivo-capture-attach-x86
        PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_definitions(muxivo-capture-attach-x86 PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(muxivo-capture-attach-x86 PRIVATE /W4 /permissive-)
    endif()
    target_include_directories(muxivo-capture-attach-x86 PRIVATE include common d3d9)
    target_link_libraries(muxivo-capture-attach-x86 PRIVATE advapi32 user32)

    install(TARGETS
        muxivo-d3d9-game-capture-x86
        muxivo-opengl-game-capture-x86
        muxivo-capture-attach-x86
        RUNTIME DESTINATION .)

    if(BUILD_TESTING)
        add_executable(muxivo-test-d3d9-x86 tests/test_d3d9_target.cpp)
        target_compile_features(muxivo-test-d3d9-x86 PRIVATE cxx_std_17)
        set_property(TARGET muxivo-test-d3d9-x86 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_definitions(muxivo-test-d3d9-x86 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        if(MSVC)
            target_compile_options(muxivo-test-d3d9-x86 PRIVATE /W4 /permissive-)
        endif()
        target_link_libraries(muxivo-test-d3d9-x86 PRIVATE d3d9 user32)

        add_executable(muxivo-test-opengl-x86 tests/test_opengl_target.cpp)
        target_compile_features(muxivo-test-opengl-x86 PRIVATE cxx_std_17)
        set_property(TARGET muxivo-test-opengl-x86 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_definitions(muxivo-test-opengl-x86 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        if(MSVC)
            target_compile_options(muxivo-test-opengl-x86 PRIVATE /W4 /permissive-)
        endif()
        target_link_libraries(muxivo-test-opengl-x86 PRIVATE opengl32 gdi32 user32)

        add_executable(muxivo-adapter-tests-x86
            tests/adapter_tests_x86.cpp
            d3d9/d3d9_offsets.cpp)
        target_compile_features(muxivo-adapter-tests-x86 PRIVATE cxx_std_17)
        set_property(TARGET muxivo-adapter-tests-x86 PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        target_compile_definitions(muxivo-adapter-tests-x86 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        if(MSVC)
            target_compile_options(muxivo-adapter-tests-x86 PRIVATE /W4 /permissive-)
        endif()
        target_include_directories(muxivo-adapter-tests-x86 PRIVATE include common d3d9)
        target_link_libraries(muxivo-adapter-tests-x86 PRIVATE advapi32 user32)

        add_test(NAME adapter_tests_x86 COMMAND muxivo-adapter-tests-x86)
        set_tests_properties(adapter_tests_x86 PROPERTIES
            ENVIRONMENT "MUXIVO_TEST_D3D9_X86_EXE=$<TARGET_FILE:muxivo-test-d3d9-x86>;MUXIVO_TEST_OPENGL_X86_EXE=$<TARGET_FILE:muxivo-test-opengl-x86>;MUXIVO_ATTACH_X86_EXE=$<TARGET_FILE:muxivo-capture-attach-x86>")
    endif()
endif()
