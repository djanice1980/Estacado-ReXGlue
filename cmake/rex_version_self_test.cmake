cmake_minimum_required(VERSION 3.25)

include("${CMAKE_CURRENT_LIST_DIR}/rex_version.cmake")

function(expect_version label expected)
  rex_compute_version(actual ${ARGN})
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR
      "${label}: expected '${expected}', received '${actual}'")
  endif()
  message(STATUS "${label}: ${actual}")
endfunction()

expect_version(tagged "0.9.2"
  FLOOR_MAJOR 0 FLOOR_MINOR 9
  GIT_DESCRIBE_EXACT "v0.9.2"
  GIT_DESCRIBE_LONG "v0.9.2-0-g12345678"
  BRANCH_NAME "main"
  GIT_REV_PARSE "12345678")

expect_version(untagged-without-reachable-tags "0.9.0.0-dev.gcb58065c"
  FLOOR_MAJOR 0 FLOOR_MINOR 9
  GIT_DESCRIBE_EXACT ""
  GIT_DESCRIBE_LONG ""
  BRANCH_NAME "main"
  GIT_REV_PARSE "cb58065c")

expect_version(no-git-metadata "0.9.0.0-dev.unknown"
  FLOOR_MAJOR 0 FLOOR_MINOR 9
  GIT_DESCRIBE_EXACT ""
  GIT_DESCRIBE_LONG ""
  BRANCH_NAME ""
  GIT_REV_PARSE "")
