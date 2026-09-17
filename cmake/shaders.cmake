# DIRECTX_DXC_TOOL is a plain path from vcpkg on Windows and a two-element
# "LD_LIBRARY_PATH=...;<dxc>" list from the vendored dxc-bin elsewhere, which
# works because Ninja runs commands through /bin/sh.
if(NOT REBLUE_DXC)
    if(DIRECTX_DXC_TOOL)
        set(REBLUE_DXC "${DIRECTX_DXC_TOOL}" CACHE STRING "dxc compiler command" FORCE)
    else()
        find_program(REBLUE_DXC NAMES dxc dxc.exe REQUIRED)
    endif()
endif()
message(STATUS "reblue: dxc at ${REBLUE_DXC}")

set(REBLUE_HLSL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/hlsl")

# reblue_host_shader(<stem> <profile> [dxc args...]) emits
# <stem>.hlsl.<dxil|spirv>.h exposing g_<stem>_<dxil|spirv>, once per backend
# in the build, and adds each header to the exes that consume that form.
function(reblue_host_shader STEM PROFILE)
    # dxc emits no header deps, so every sibling .hlsli is listed by hand.
    file(GLOB hlsl_includes "${REBLUE_HLSL_DIR}/*.hlsli")

    foreach(target_list IN ITEMS REBLUE_D3D12_TARGETS REBLUE_VULKAN_TARGETS)
        if(NOT ${target_list})
            continue()
        endif()

        if(target_list STREQUAL "REBLUE_D3D12_TARGETS")
            set(ext dxil)
            set(format_args "")
        else()
            set(ext spirv)
            # Keep cbuffer byte offsets identical to D3D, and match D3D clip space.
            set(format_args -spirv -fvk-use-dx-layout)
            if(PROFILE MATCHES "^vs")
                list(APPEND format_args -fvk-invert-y)
            endif()
        endif()

        set(out "${REBLUE_GEN_DIR}/src/gpu/shaders/hlsl/${STEM}.hlsl.${ext}.h")
        add_custom_command(
            OUTPUT "${out}"
            COMMAND ${REBLUE_DXC}
                    -T ${PROFILE} -HV 2021 -all-resources-bound
                    -Wno-ignored-attributes ${format_args}
                    -I "${CMAKE_CURRENT_SOURCE_DIR}"
                    -Fh "${out}" "${REBLUE_HLSL_DIR}/${STEM}.hlsl"
                    -Vn g_${STEM}_${ext} ${ARGN}
            DEPENDS "${REBLUE_HLSL_DIR}/${STEM}.hlsl" ${hlsl_includes} "${REBLUE_SHADER_COMMON_H}"
            COMMENT "Compiling ${STEM}.hlsl (${PROFILE}, ${ext})"
            VERBATIM)

        # Android chooses between descriptor-indexing and compact fixed-array
        # layouts at runtime. Emit a second SPIR-V blob with the compact
        # declarations so the host helpers match whichever pipeline layout was
        # selected for the device.
        set(compat_out "")
        set(ubo_out "")
        if(ANDROID AND target_list STREQUAL "REBLUE_VULKAN_TARGETS")
            set(compat_out "${REBLUE_GEN_DIR}/src/gpu/shaders/hlsl/${STEM}.hlsl.compat.spirv.h")
            add_custom_command(
                OUTPUT "${compat_out}"
                COMMAND ${REBLUE_DXC}
                        -T ${PROFILE} -HV 2021 -all-resources-bound
                        -Wno-ignored-attributes ${format_args}
                        -D REBLUE_DESCRIPTOR_COMPAT=1
                        -I "${CMAKE_CURRENT_SOURCE_DIR}"
                        -Fh "${compat_out}" "${REBLUE_HLSL_DIR}/${STEM}.hlsl"
                        -Vn g_${STEM}_compat_spirv ${ARGN}
                DEPENDS "${REBLUE_HLSL_DIR}/${STEM}.hlsl" ${hlsl_includes} "${REBLUE_SHADER_COMMON_H}"
                COMMENT "Compiling ${STEM}.hlsl (${PROFILE}, compat spirv)"
                VERBATIM)

            if(STEM STREQUAL "bd_2d_blit_vs" OR STEM STREQUAL "bd_2d_blit_ps")
                set(ubo_out "${REBLUE_GEN_DIR}/src/gpu/shaders/hlsl/${STEM}.hlsl.ubo.spirv.h")
                add_custom_command(
                    OUTPUT "${ubo_out}"
                    COMMAND ${REBLUE_DXC}
                            -T ${PROFILE} -HV 2021 -all-resources-bound
                            -Wno-ignored-attributes ${format_args}
                            -D REBLUE_DESCRIPTOR_COMPAT=1
                            -D REBLUE_SPIRV_UBO_COMPAT=1
                            -I "${CMAKE_CURRENT_SOURCE_DIR}"
                            -Fh "${ubo_out}" "${REBLUE_HLSL_DIR}/${STEM}.hlsl"
                            -Vn g_${STEM}_ubo_spirv ${ARGN}
                    DEPENDS "${REBLUE_HLSL_DIR}/${STEM}.hlsl" ${hlsl_includes} "${REBLUE_SHADER_COMMON_H}"
                    COMMENT "Compiling ${STEM}.hlsl (${PROFILE}, ubo compat spirv)"
                    VERBATIM)
            endif()
        endif()
        foreach(target IN LISTS ${target_list})
            target_sources(${target} PRIVATE "${out}")
            if(compat_out)
                target_sources(${target} PRIVATE "${compat_out}")
            endif()
            if(ubo_out)
                target_sources(${target} PRIVATE "${ubo_out}")
            endif()
        endforeach()
    endforeach()
endfunction()

# The runtime shader linker and reblue_prelink both load dxcompiler/dxil.
function(reblue_stage_dxc TARGET)
    get_filename_component(dxc_dir "${DIRECTX_DXC_TOOL}" DIRECTORY)
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${dxc_dir}/dxcompiler.dll" "${dxc_dir}/dxil.dll"
            "$<TARGET_FILE_DIR:${TARGET}>"
        VERBATIM)
endfunction()
