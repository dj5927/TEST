# Runs the Xenos shader recompiler over INPUT_DIR at build time into OUTPUT_CPP.
# Defines reblue_shader_cache_gen, and reblue_shader_hlsl_dump for inspecting
# the intermediate HLSL.
function(reblue_shader_cache)
    cmake_parse_arguments(ARG "" "RECOMP_TARGET;INPUT_DIR;INCLUDE_FILE;OUTPUT_CPP" "" ${ARGN})
    foreach(arg RECOMP_TARGET INPUT_DIR INCLUDE_FILE OUTPUT_CPP)
        if(NOT ARG_${arg})
            message(FATAL_ERROR "reblue_shader_cache: missing ${arg}")
        endif()
    endforeach()

    cmake_path(GET ARG_OUTPUT_CPP PARENT_PATH output_dir)
    file(MAKE_DIRECTORY "${output_dir}")

    if(ANDROID)
        # XenosRecomp is a host-side generator. In an Android cross build its
        # target would be an Android executable and therefore cannot run on the
        # Windows build host. Reuse the architecture-independent cache emitted
        # by the known-good desktop build instead.
        set(prebuilt "${CMAKE_CURRENT_SOURCE_DIR}/android_prebuilt/shader_cache.cpp")
        if(NOT EXISTS "${prebuilt}")
            message(FATAL_ERROR "Android prebuilt shader_cache.cpp is missing")
        endif()
        add_custom_command(
            OUTPUT "${ARG_OUTPUT_CPP}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${prebuilt}" "${ARG_OUTPUT_CPP}"
            DEPENDS "${prebuilt}"
            COMMENT "Using prebuilt Xenos shader cache for Android"
            VERBATIM)
        add_custom_target(reblue_shader_cache_gen DEPENDS "${ARG_OUTPUT_CPP}")
        add_custom_target(reblue_shader_hlsl_dump)
        return()
    endif()

    file(GLOB_RECURSE shader_inputs CONFIGURE_DEPENDS
        "${ARG_INPUT_DIR}/*.vso" "${ARG_INPUT_DIR}/*.pso" "${ARG_INPUT_DIR}/*.xex")

    add_custom_command(
        OUTPUT "${ARG_OUTPUT_CPP}"
        COMMAND $<TARGET_FILE:${ARG_RECOMP_TARGET}>
                "${ARG_INPUT_DIR}" "${ARG_OUTPUT_CPP}" "${ARG_INCLUDE_FILE}"
        DEPENDS ${ARG_RECOMP_TARGET} "${ARG_INCLUDE_FILE}" ${shader_inputs}
        COMMENT "Recompiling Xenos shaders from ${ARG_INPUT_DIR}"
        USES_TERMINAL VERBATIM)
    add_custom_target(reblue_shader_cache_gen DEPENDS "${ARG_OUTPUT_CPP}")

    set(REBLUE_HLSL_DUMP_DIR "${CMAKE_BINARY_DIR}/hlsl_dump" CACHE PATH
        "Directory reblue_shader_hlsl_dump writes recompiled HLSL into")
    add_custom_target(reblue_shader_hlsl_dump
        COMMAND ${CMAKE_COMMAND} -E make_directory "${REBLUE_HLSL_DUMP_DIR}"
        COMMAND $<TARGET_FILE:${ARG_RECOMP_TARGET}>
                "${ARG_INPUT_DIR}" "${output_dir}/shader_cache.hlsldump.cpp"
                "${ARG_INCLUDE_FILE}" "${REBLUE_HLSL_DUMP_DIR}"
        COMMENT "Dumping recompiled HLSL to ${REBLUE_HLSL_DUMP_DIR}"
        USES_TERMINAL VERBATIM)
    add_dependencies(reblue_shader_hlsl_dump ${ARG_RECOMP_TARGET})
endfunction()
