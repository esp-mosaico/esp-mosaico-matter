# SPDX-License-Identifier: Apache-2.0

function(bridge_apply_dependency_patch component_dir patch_file)
    if(NOT IS_DIRECTORY "${component_dir}")
        message(FATAL_ERROR "Missing dependency directory for ${patch_file}: ${component_dir}")
    endif()
    find_program(bridge_git_program git)
    if(NOT bridge_git_program)
        message(FATAL_ERROR "Git is required to build matter_bridge")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${patch_file}")

    # Apply paths relative to the component, independent of the enclosing checkout.
    # Git must not discover the project's repository and skip component-local paths.
    get_filename_component(component_dir "${component_dir}" REALPATH)
    get_filename_component(component_parent "${component_dir}" DIRECTORY)
    set(git_apply "${CMAKE_COMMAND}" -E env --unset=GIT_DIR --unset=GIT_WORK_TREE
        "GIT_CEILING_DIRECTORIES=${component_parent}"
        "${bridge_git_program}" apply --no-index)

    execute_process(
        COMMAND ${git_apply} --check "${patch_file}"
        WORKING_DIRECTORY "${component_dir}"
        RESULT_VARIABLE forward_result
        OUTPUT_VARIABLE forward_output
        ERROR_VARIABLE forward_error)
    if(forward_result EQUAL 0)
        execute_process(
            COMMAND ${git_apply} "${patch_file}"
            WORKING_DIRECTORY "${component_dir}"
            RESULT_VARIABLE apply_result
            OUTPUT_VARIABLE apply_output
            ERROR_VARIABLE apply_error)
        if(NOT apply_result EQUAL 0)
            message(FATAL_ERROR "Unable to apply ${patch_file}:\n${apply_output}${apply_error}")
        endif()
        message(STATUS "matter_bridge: applied ${patch_file}")
        return()
    endif()

    # Accept only a complete, already-applied patch. Dependency changes that
    # match neither direction need review rather than a silent skipped fix.
    execute_process(
        COMMAND ${git_apply} --reverse --check "${patch_file}"
        WORKING_DIRECTORY "${component_dir}"
        RESULT_VARIABLE reverse_result
        OUTPUT_QUIET ERROR_QUIET)
    if(NOT reverse_result EQUAL 0)
        message(FATAL_ERROR
            "Dependency does not match ${patch_file}; review the dependency version and patch.\n"
            "${forward_output}${forward_error}")
    endif()
endfunction()
