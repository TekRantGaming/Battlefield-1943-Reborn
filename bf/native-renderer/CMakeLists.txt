# rexgpu-bfnative: Battlefield 1943's native renderer as a ReXGlue GPU plugin, Vulkan only (DECISIONS.md, 5 Nov
# 2026: no D3D12, no DXIL, no DirectX of any kind). Built from King Kong's native renderer (tag v2.1.0) in stages
# M0 to M5 (PROMPT-VULKAN-RENDERER.md, docs/native-renderer.md).
#
# Built inside our SDK copy (sdk/src-bf) next to rexgpu-xenos: configure the SDK with
# -DBF_NATIVE_RENDERER_DIR=<this folder> (sdk/src-bf/build_bf.bat does). src/graphics/CMakeLists.txt then hands
# us REXGPU_XENOS_VULKAN_ONLY_SOURCES, the Xenos GPU sources without D3D12 and DXBC (absolute paths): until the
# native passes replace it (M2, M3), the Xenos Vulkan renderer draws the picture inside this plugin.

set(BFNATIVE_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/plugin/plugin_main.cpp
    ${CMAKE_CURRENT_LIST_DIR}/plugin/native_api.cpp
    ${CMAKE_CURRENT_LIST_DIR}/plugin/null_graphics.cpp
    ${REXGPU_XENOS_VULKAN_ONLY_SOURCES}
)

add_library(rexgpu-bfnative SHARED ${BFNATIVE_SOURCES})
set_target_properties(rexgpu-bfnative PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    PREFIX ""
)
if(UNIX)
    set_target_properties(rexgpu-bfnative PROPERTIES PREFIX "lib")
endif()
# The runtime is built with D3D12 on Windows and says so in a public definition; this plugin has none.
target_compile_options(rexgpu-bfnative PRIVATE -UREX_HAS_D3D12 -DREX_HAS_D3D12=0 -Wno-unused-variable)
target_include_directories(rexgpu-bfnative PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${REXGLUE_ROOT}/thirdparty/renderdoc
    ${REXGLUE_ROOT}
)
target_link_libraries(rexgpu-bfnative PRIVATE
    rexruntime
    xxHash::xxhash
    Vulkan::Headers
    glslang::SPIRV
    GPUOpen::VulkanMemoryAllocator
    glslang
    MachineIndependent
    GenericCodeGen
    OSDependent
    OGLCompiler
)
# Symbols (CodeView, a PDB beside the DLL) for profiling; the code is the same optimised code.
if(WIN32)
    target_compile_options(rexgpu-bfnative PRIVATE -gcodeview -g)
    target_link_options(rexgpu-bfnative PRIVATE "LINKER:/DEBUG")
    install(FILES $<TARGET_PDB_FILE:rexgpu-bfnative> DESTINATION bin OPTIONAL)
endif()
install(TARGETS rexgpu-bfnative RUNTIME DESTINATION bin LIBRARY DESTINATION lib)
