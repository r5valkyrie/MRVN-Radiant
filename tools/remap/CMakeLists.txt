# Optional HIPRT support for accelerated ray tracing
# HIPRT uses Orochi (GPU abstraction over HIP/CUDA) and hiprtew (runtime DLL loading)
# so no HIP SDK installation is required - GPU libraries are loaded dynamically at runtime.
option(USE_HIPRT "Use AMD HIP RT for accelerated ray tracing" ON)

if(USE_HIPRT)
    set(HIPRT_SDK_DIR "${CMAKE_SOURCE_DIR}/windeps/hiprt")
    set(HIPRT_LIB "${HIPRT_SDK_DIR}/hiprt/win/hiprt0300064.lib")
    
    if(EXISTS "${HIPRT_SDK_DIR}/hiprt/hiprt.h" AND EXISTS "${HIPRT_LIB}")
        message(STATUS "HIPRT SDK found: ${HIPRT_SDK_DIR}")
        add_compile_definitions(USE_HIPRT)
        add_compile_definitions(OROCHI_ENABLE_CUEW)
        # Pass SDK path as string for runtime kernel compilation include paths
        string(REPLACE "\\" "/" HIPRT_SDK_DIR_FWD "${HIPRT_SDK_DIR}")
        add_compile_definitions(HIPRT_SDK_INCLUDE_DIR=\"${HIPRT_SDK_DIR_FWD}\")
    else()
        message(STATUS "HIPRT SDK not found in windeps - using fallback ray tracing")
        set(USE_HIPRT OFF)
    endif()
endif()

add_executable(remap
               common/bitflags.h
               common/cmdlib.cpp
               common/cmdlib.h
               common/inout_xml.h
               common/inout.cpp
               common/inout.h
               common/jpeg.cpp
               common/maxworld.h
               common/md4.cpp
               common/md4.h
               common/miniz.cpp
               common/miniz.h
               common/mutex.cpp
               common/mutex.h
               common/polylib.cpp
               common/polylib.h
               common/qimagelib.cpp
               common/qimagelib.h
               common/qmath.h
               common/qpathops.h
               common/qstringops.h
               common/qthreads.h
               common/scriplib.cpp
               common/scriplib.h
               common/stringfixedsize.h
               common/threads.cpp
               common/unzip.cpp
               common/unzip.h
               common/vfs.cpp
               common/vfs.h
               source/hiprt_trace.cpp
               source/hiprt_trace.h
               source/apex_legends/apex_legends_bsp.cpp
               source/apex_legends/apex_legends_collisions.cpp
               source/apex_legends/apex_legends_entities.cpp
               source/apex_legends/apex_legends_lighting.cpp
               source/apex_legends/apex_legends_lightmaps.cpp
               source/apex_legends/apex_legends_meshes.cpp
               source/apex_legends/apex_legends_models.cpp
               source/apex_legends/apex_legends_portals.cpp
               source/apex_legends/apex_legends_textures.cpp
               source/apex_legends/apex_legends_vertices.cpp
               source/apex_legends/apex_legends_visibility.cpp
               source/apex_legends/apex_legends.h
               source/titanfall/titanfall_collisions.cpp
               source/titanfall/titanfall_decompile.cpp
               source/titanfall/titanfall_misc.cpp
               source/titanfall/titanfall_vis.cpp
               source/titanfall/titanfall.cpp
               source/titanfall/titanfall.h
               source/titanfall2/titanfall2_vis.cpp
               source/titanfall2/titanfall2.cpp
               source/titanfall2/titanfall2.h
               source/brush.cpp
               source/bsp.cpp
               source/bspfile_abstract.cpp
               source/bspfile_abstract.h
               source/bspfile_shared.cpp
               source/bspfile_shared.h
               source/convert_bsp.cpp
               source/convert_json.cpp
               source/convert_obj.cpp
               source/convert_obj.h
               source/decals.cpp
               source/exportents.cpp
               source/facebsp.cpp
               source/fog.cpp
               source/games.cpp
               source/games.h
               source/help.cpp
               source/image.cpp
               source/leakfile.cpp
               source/light_bounce.cpp
               source/light_trace.cpp
               source/light_ydnar.cpp
               source/light.cpp
               source/lightmaps_ydnar.cpp
               source/lump_names.h
               source/main.cpp
               source/map.cpp
               source/mesh.cpp
               source/model.cpp
               source/model.h
               source/patch.cpp
               source/path_init.cpp
               source/portals.cpp
               source/prtfile.cpp
               source/remap.h
               source/shaders.cpp
               source/shaders.h
               source/surface_extra.cpp
               source/surface_extra.h
               source/surface_foliage.cpp
               source/surface_fur.cpp
               source/surface_meta.cpp
               source/surface.cpp
               source/tjunction.cpp
               source/tree.cpp
               source/vis.cpp
               source/vis.h
               source/visflow.cpp
               source/visflow.h
               source/writebsp.cpp
               ${CMAKE_SOURCE_DIR}/icons/remap.rc
              )

target_include_directories(remap PRIVATE .
                           common
                           source
                           ${CMAKE_SOURCE_DIR}/libs/assimp
                           ${LIBXML2_INCLUDE_DIRS}
                           ${GLIB_INCLUDE_DIRS}
                           ${JPEG_INCLUDE_DIRS}
                           ${PNG_INCLUDE_DIRS}
                          )

# HIPRT: add Orochi/hipew/cuew sources and include directories
if(USE_HIPRT)
    target_sources(remap PRIVATE
        ${HIPRT_SDK_DIR}/contrib/Orochi/Orochi/Orochi.cpp
        ${HIPRT_SDK_DIR}/contrib/Orochi/contrib/hipew/src/hipew.cpp
        ${HIPRT_SDK_DIR}/contrib/Orochi/contrib/cuew/src/cuew.cpp
    )
    target_include_directories(remap PRIVATE
        ${HIPRT_SDK_DIR}
        ${HIPRT_SDK_DIR}/contrib/Orochi
        ${HIPRT_SDK_DIR}/contrib/Orochi/contrib/hipew/include
        ${HIPRT_SDK_DIR}/contrib/Orochi/contrib/cuew/include
        ${CMAKE_SOURCE_DIR}/windeps/cuda/include
    )
endif()

target_link_libraries(remap
                      xml
                      ddslib
                      etclib
                      filematch
                      l_net
                      commandlib
                      assimp
                      Threads::Threads
                      ${GLIB_LIBRARIES}
                      ${JPEG_LIBRARIES}
                      ${PNG_LIBRARIES}
                      ${ZLIB_LIBRARIES}
                      ${LIBXML2_LIBRARIES}
                     )

# Link HIPRT if available
if(USE_HIPRT)
    target_link_libraries(remap ${HIPRT_LIB} version)
    
    # Copy HIPRT DLLs to output directory (Windows only)
    if(WIN32)
        set(HIPRT_DLL_DIR "${HIPRT_SDK_DIR}/hiprt/win")
        add_custom_command(TARGET remap POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${HIPRT_DLL_DIR}/hiprt0300064.dll"
                "${HIPRT_DLL_DIR}/amd_comgr0604.dll"
                "${HIPRT_DLL_DIR}/hiprtc0604.dll"
                "${HIPRT_DLL_DIR}/hiprtc-builtins0604.dll"
                $<TARGET_FILE_DIR:remap>
            COMMENT "Copying HIPRT DLLs..."
        )
        # Copy kernel bitcode files needed at runtime
        add_custom_command(TARGET remap POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${HIPRT_DLL_DIR}/hiprt03000_6.4_amd.hipfb"
                "${HIPRT_DLL_DIR}/hiprt03000_6.4_amd_lib_win.bc"
                "${HIPRT_DLL_DIR}/hiprt03000_nv.fatbin"
                "${HIPRT_DLL_DIR}/hiprt03000_nv_lib.fatbin"
                "${HIPRT_DLL_DIR}/oro_compiled_kernels.fatbin"
                "${HIPRT_DLL_DIR}/oro_compiled_kernels.hipfb"
                $<TARGET_FILE_DIR:remap>
            COMMENT "Copying HIPRT kernel files..."
        )
    endif()
endif()

set_target_properties(remap PROPERTIES LINK_FLAGS "-Wl,-rpath,./")
