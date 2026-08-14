# Resolve every form of Corelet's version from one Git identity.
#
# Live builds pass SOURCE_DIR. Packagers can additionally pass INPUT_MANIFEST
# to freeze an identity before copying the source into a Git-less staging tree.
# OUTPUT_MANIFEST, OUTPUT_HEADER, OUTPUT_MANPAGE and OUTPUT_PLIST are optional;
# the corresponding template must accompany either templated output.

cmake_minimum_required(VERSION 3.16)

function(write_if_different path content)
    if(NOT path)
        return()
    endif()
    get_filename_component(directory "${path}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    set(previous "")
    if(EXISTS "${path}")
        file(READ "${path}" previous)
    endif()
    if(NOT previous STREQUAL content)
        file(WRITE "${path}" "${content}")
    endif()
endfunction()

# CMake's regular-expression dialect is old but sufficient for SemVer's
# published grammar. Keeping the validation here means `nightly`, `latest` and
# tag-shaped typos cannot mask an older release tag.
function(is_release_tag tag result)
    set(number "(0|[1-9][0-9]*)")
    set(identifier "(0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)")
    set(prerelease "(${identifier}(\\.${identifier})*)")
    set(metadata_identifier "[0-9A-Za-z-]+")
    set(metadata "(${metadata_identifier}(\\.${metadata_identifier})*)")
    if("${tag}" MATCHES "^v${number}\\.${number}\\.${number}(-${prerelease})?(\\+${metadata})?$")
        set(${result} TRUE PARENT_SCOPE)
    else()
        set(${result} FALSE PARENT_SCOPE)
    endif()
endfunction()

if(INPUT_MANIFEST)
    if(NOT EXISTS "${INPUT_MANIFEST}")
        message(FATAL_ERROR "version manifest does not exist: ${INPUT_MANIFEST}")
    endif()
    include("${INPUT_MANIFEST}")
    foreach(field IN ITEMS CORELET_VERSION CORELET_VERSION_CORE
                           CORELET_VERSION_DEBIAN CORELET_VERSION_BUILD
                           CORELET_VERSION_DATE)
        if(NOT DEFINED ${field})
            message(FATAL_ERROR "version manifest has no ${field}: ${INPUT_MANIFEST}")
        endif()
    endforeach()
else()
    set(CORELET_VERSION "0.0.0")
    set(CORELET_VERSION_CORE "0.0.0")
    set(CORELET_VERSION_DEBIAN "0.0.0")
    set(CORELET_VERSION_BUILD "0")
    set(CORELET_VERSION_DATE "Thu, 01 Jan 1970 00:00:00 +0000")

    find_package(Git QUIET)
    if(GIT_EXECUTABLE AND SOURCE_DIR)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --verify HEAD
            WORKING_DIRECTORY "${SOURCE_DIR}"
            OUTPUT_QUIET
            ERROR_QUIET
            RESULT_VARIABLE have_head)
        if(have_head EQUAL 0)
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
                WORKING_DIRECTORY "${SOURCE_DIR}"
                OUTPUT_VARIABLE short_hash
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_VARIABLE git_error
                RESULT_VARIABLE git_status)
            if(NOT git_status EQUAL 0)
                message(FATAL_ERROR "cannot resolve Git hash: ${git_error}")
            endif()
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" rev-list --count HEAD
                WORKING_DIRECTORY "${SOURCE_DIR}"
                OUTPUT_VARIABLE CORELET_VERSION_BUILD
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_VARIABLE git_error
                RESULT_VARIABLE git_status)
            if(NOT git_status EQUAL 0)
                message(FATAL_ERROR "cannot count Git commits: ${git_error}")
            endif()
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" show -s --format=%aD HEAD
                WORKING_DIRECTORY "${SOURCE_DIR}"
                OUTPUT_VARIABLE CORELET_VERSION_DATE
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_VARIABLE git_error
                RESULT_VARIABLE git_status)
            if(NOT git_status EQUAL 0)
                message(FATAL_ERROR "cannot read Git commit date: ${git_error}")
            endif()

            execute_process(
                COMMAND "${GIT_EXECUTABLE}" tag --merged HEAD
                WORKING_DIRECTORY "${SOURCE_DIR}"
                OUTPUT_VARIABLE merged_tags
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_VARIABLE git_error
                RESULT_VARIABLE git_status)
            if(NOT git_status EQUAL 0)
                message(FATAL_ERROR "cannot list Git tags: ${git_error}")
            endif()
            string(REPLACE "\n" ";" merged_tags "${merged_tags}")
            set(describe_command "${GIT_EXECUTABLE}" describe --tags --long --abbrev=7)
            set(release_tag_count 0)
            foreach(tag IN LISTS merged_tags)
                is_release_tag("${tag}" valid)
                if(valid)
                    list(APPEND describe_command --match "${tag}")
                    math(EXPR release_tag_count "${release_tag_count} + 1")
                endif()
            endforeach()
            list(APPEND describe_command HEAD)

            set(have_release_tag FALSE)
            if(release_tag_count GREATER 0)
                execute_process(
                    COMMAND ${describe_command}
                    WORKING_DIRECTORY "${SOURCE_DIR}"
                    OUTPUT_VARIABLE described
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET
                    RESULT_VARIABLE describe_status)
                if(describe_status EQUAL 0 AND
                   described MATCHES "^(v.*)-([0-9]+)-g([0-9a-f]+)$")
                    set(tag "${CMAKE_MATCH_1}")
                    set(distance "${CMAKE_MATCH_2}")
                    set(described_hash "${CMAKE_MATCH_3}")
                    is_release_tag("${tag}" valid)
                    if(valid)
                        set(have_release_tag TRUE)
                        string(REGEX REPLACE "^v" "" semver "${tag}")
                        string(REGEX REPLACE
                            "^([0-9]+\\.[0-9]+\\.[0-9]+).*$" "\\1"
                            CORELET_VERSION_CORE "${semver}")
                        string(REGEX REPLACE
                            "^([0-9]+\\.[0-9]+\\.[0-9]+)-" "\\1~"
                            debian_base "${semver}")
                        if(distance EQUAL 0)
                            set(CORELET_VERSION "${semver}")
                            set(CORELET_VERSION_DEBIAN "${debian_base}")
                        else()
                            set(CORELET_VERSION "${semver}-${distance}-g${described_hash}")
                            set(CORELET_VERSION_DEBIAN
                                "${debian_base}+${distance}.g${described_hash}")
                        endif()
                    endif()
                endif()
            endif()

            if(NOT have_release_tag)
                set(CORELET_VERSION "0.0.0-${short_hash}")
                set(CORELET_VERSION_DEBIAN "0.0.0+g${short_hash}")
            endif()

            # Match git describe --dirty: staged and unstaged tracked changes
            # count, while untracked build products do not.
            execute_process(
                COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
                WORKING_DIRECTORY "${SOURCE_DIR}"
                OUTPUT_VARIABLE dirty
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_VARIABLE git_error
                RESULT_VARIABLE git_status)
            if(NOT git_status EQUAL 0)
                message(FATAL_ERROR "cannot inspect Git worktree: ${git_error}")
            endif()
            if(dirty)
                string(APPEND CORELET_VERSION "-dirty")
                if(CORELET_VERSION_DEBIAN MATCHES "[+]")
                    string(APPEND CORELET_VERSION_DEBIAN ".dirty")
                else()
                    string(APPEND CORELET_VERSION_DEBIAN "+dirty")
                endif()
            endif()
        endif()
    endif()
endif()

string(CONCAT manifest
    "# Generated by cmake/version.cmake. Do not edit.\n"
    "set(CORELET_VERSION \"${CORELET_VERSION}\")\n"
    "set(CORELET_VERSION_CORE \"${CORELET_VERSION_CORE}\")\n"
    "set(CORELET_VERSION_DEBIAN \"${CORELET_VERSION_DEBIAN}\")\n"
    "set(CORELET_VERSION_BUILD \"${CORELET_VERSION_BUILD}\")\n"
    "set(CORELET_VERSION_DATE \"${CORELET_VERSION_DATE}\")\n")
write_if_different("${OUTPUT_MANIFEST}" "${manifest}")

string(CONCAT header
    "// Generated by cmake/version.cmake. Do not edit.\n"
    "#pragma once\n"
    "#define CORELET_VERSION \"${CORELET_VERSION}\"\n")
write_if_different("${OUTPUT_HEADER}" "${header}")

if(OUTPUT_MANPAGE)
    if(NOT MANPAGE_TEMPLATE)
        message(FATAL_ERROR "OUTPUT_MANPAGE needs MANPAGE_TEMPLATE")
    endif()
    configure_file("${MANPAGE_TEMPLATE}" "${OUTPUT_MANPAGE}" @ONLY)
endif()

if(OUTPUT_PLIST)
    if(NOT PLIST_TEMPLATE)
        message(FATAL_ERROR "OUTPUT_PLIST needs PLIST_TEMPLATE")
    endif()
    configure_file("${PLIST_TEMPLATE}" "${OUTPUT_PLIST}" @ONLY)
endif()

if(PRINT_VERSION)
    message("${CORELET_VERSION}")
endif()
