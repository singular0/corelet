cmake_minimum_required(VERSION 3.16)

if(NOT SOURCE_ROOT)
    message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

set(resolver "${SOURCE_ROOT}/cmake/version.cmake")
set(temp_base "$ENV{TMPDIR}")
if(NOT temp_base)
    set(temp_base "/tmp")
endif()
string(RANDOM LENGTH 10 ALPHABET 0123456789abcdef suffix)
set(TEST_ROOT "${temp_base}/corelet-version-test-${suffix}")
set(repo "${TEST_ROOT}/version-test-repo")
set(plain "${TEST_ROOT}/version-test-plain")
file(REMOVE_RECURSE "${repo}" "${plain}")
file(MAKE_DIRECTORY "${repo}" "${plain}")

function(run_git)
    execute_process(
        COMMAND git ${ARGN}
        WORKING_DIRECTORY "${repo}"
        OUTPUT_QUIET ERROR_VARIABLE error RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "git ${ARGN} failed: ${error}")
    endif()
endfunction()

function(resolve source output)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=${source}
                -DOUTPUT_MANIFEST=${output} -P "${resolver}"
        RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "version resolver failed for ${source}")
    endif()
endfunction()

function(assert_equal actual expected label)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "${label}: expected '${expected}', got '${actual}'")
    endif()
endfunction()

run_git(init -q)
run_git(config user.name "Version Test")
run_git(config user.email version-test@example.invalid)
file(WRITE "${repo}/tracked" "one\n")
run_git(add tracked)
run_git(commit -q -m initial)
run_git(tag nightly)
run_git(tag v01.2.3)
execute_process(COMMAND git rev-parse --short=7 HEAD WORKING_DIRECTORY "${repo}"
                OUTPUT_VARIABLE first_hash OUTPUT_STRIP_TRAILING_WHITESPACE)

set(manifest "${TEST_ROOT}/version-test.cmake")
resolve("${repo}" "${manifest}")
include("${manifest}")
assert_equal("${CORELET_VERSION}" "0.0.0-${first_hash}" "non-release tag fallback")
assert_equal("${CORELET_VERSION_DEBIAN}" "0.0.0+g${first_hash}" "fallback Debian version")

run_git(tag v1.2.3)
resolve("${repo}" "${manifest}")
include("${manifest}")
assert_equal("${CORELET_VERSION}" "1.2.3" "exact stable tag")
assert_equal("${CORELET_VERSION_CORE}" "1.2.3" "stable numeric core")
assert_equal("${CORELET_VERSION_DEBIAN}" "1.2.3" "stable Debian version")
run_git(tag -d v1.2.3)

run_git(tag v1.2.3-rc.1+meta)
resolve("${repo}" "${manifest}")
include("${manifest}")
assert_equal("${CORELET_VERSION}" "1.2.3-rc.1+meta" "exact SemVer tag")
assert_equal("${CORELET_VERSION_CORE}" "1.2.3" "numeric core")
assert_equal("${CORELET_VERSION_DEBIAN}" "1.2.3~rc.1+meta" "prerelease Debian version")

file(APPEND "${repo}/tracked" "two\n")
run_git(add tracked)
run_git(commit -q -m second)
run_git(tag v9.9.9-01)
execute_process(COMMAND git rev-parse --short=7 HEAD WORKING_DIRECTORY "${repo}"
                OUTPUT_VARIABLE second_hash OUTPUT_STRIP_TRAILING_WHITESPACE)
resolve("${repo}" "${manifest}")
include("${manifest}")
assert_equal("${CORELET_VERSION}" "1.2.3-rc.1+meta-1-g${second_hash}" "post-tag version")
assert_equal("${CORELET_VERSION_DEBIAN}" "1.2.3~rc.1+meta+1.g${second_hash}"
             "post-tag Debian version")

file(APPEND "${repo}/tracked" "dirty\n")
resolve("${repo}" "${manifest}")
include("${manifest}")
assert_equal("${CORELET_VERSION}" "1.2.3-rc.1+meta-1-g${second_hash}-dirty"
             "dirty version")
assert_equal("${CORELET_VERSION_DEBIAN}" "1.2.3~rc.1+meta+1.g${second_hash}.dirty"
             "dirty Debian version")

run_git(add tracked)
resolve("${repo}" "${manifest}")
include("${manifest}")
assert_equal("${CORELET_VERSION}" "1.2.3-rc.1+meta-1-g${second_hash}-dirty"
             "staged dirty version")

file(WRITE "${plain}/source" "no Git metadata\n")
resolve("${plain}" "${manifest}")
include("${manifest}")
assert_equal("${CORELET_VERSION}" "0.0.0" "Git-less fallback")
assert_equal("${CORELET_VERSION_DEBIAN}" "0.0.0" "Git-less Debian fallback")
assert_equal("${CORELET_VERSION_BUILD}" "0" "Git-less bundle build")

file(REMOVE_RECURSE "${repo}" "${plain}" "${manifest}")
