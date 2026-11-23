cmake_minimum_required(VERSION 4.3.4 FATAL_ERROR)

# cairn: an IRC client library, as C++26 modules -- over a transport of the
# caller's, with the control flow the caller's.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/import-std.cmake)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_EXTENSIONS OFF)

project(cairn VERSION 0.1 LANGUAGES CXX)

option(CAIRN_TESTS "Build the tests" ${PROJECT_IS_TOP_LEVEL})

if(NOT COMMAND cme_find_package)
  set(CME_WITHOUT_PROVIDER ON)
  include(${CMAKE_CURRENT_LIST_DIR}/cmake/get_cme.cmake)
endif()

# splice: the variant cairn's errors, casemappings and messages are held
# in, and the visiting. The family's step count, so that all of them share
# one std.
set(SPLICE_CONSTEXPR_STEPS 100000000 CACHE STRING
  "clang's -fconstexpr-steps for splice, as cairn and its family set it")
cme_declare_port(NAME splice PROVIDES splice
  GITHUB_REPOSITORY j4niwzis/splice
  GIT_TAG 8a7755a335917daf3bbfc54ef8a3a160f8445d15
  LICENSE GPL-3.0-only
  TARGETS splice)
cme_options(splice "SPLICE_TESTS OFF")
cme_find_package(splice REQUIRED)

set(CAIRN_CONSTEXPR_STEPS 100000000 CACHE STRING
  "clang's -fconstexpr-steps for cairn and what imports it, as its family sets it")

# One library, its modules in one file set.
add_library(cairn)
target_sources(cairn PUBLIC FILE_SET CXX_MODULES BASE_DIRS src
  FILES src/line.cc src/grammar.cc src/read.cc src/casemap.cc src/isupport.cc src/messages.cc src/cairn.cc)
target_compile_features(cairn PUBLIC cxx_std_26)
target_link_libraries(cairn PUBLIC splice)
target_compile_options(cairn PUBLIC
  $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-fconstexpr-steps=${CAIRN_CONSTEXPR_STEPS}>)
add_library(cairn::cairn ALIAS cairn)

# The tests: static_asserts, checked while they compile, and a run each.
if(CAIRN_TESTS)
  enable_testing()
  foreach(name IN ITEMS line messages grammars)
    add_executable(cairn-${name}-test test/${name}_test.cc)
    target_link_libraries(cairn-${name}-test PRIVATE cairn)
    add_test(NAME ${name} COMMAND cairn-${name}-test)
  endforeach()
endif()
