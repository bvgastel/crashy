cmake_minimum_required(VERSION 3.10) # 3.14 handles PIC differently
project (crashy CXX)
set(CMAKE_CXX_STANDARD 17)

OPTION(BUILD_CRASH_REPORTING "Build and include a crash reporter on supported platforms" ON)

# Set default build type.
if(NOT CMAKE_BUILD_TYPE)
  message(STATUS "Setting build type to 'RelWithDebInfo' as none was specified.")
  set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING
      "Choose the type of build, options are: Debug Release RelWithDebInfo MinSizeRel."
      FORCE)
endif()

IF(BUILD_CRASH_REPORTING 
    AND (NOT CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    AND (CMAKE_SYSTEM_NAME MATCHES "Linux"
      OR CMAKE_SYSTEM_NAME MATCHES "FreeBSD"
      OR CMAKE_SYSTEM_NAME MATCHES "Darwin"))

set (crash
     src/crash.cpp
     src/simple-raw.cpp
     src/reporter.cpp
     src/unwinder.cpp
     src/tosourcecode.cpp
     src/util.cpp
)

add_library (${PROJECT_NAME} STATIC ${crash})

# relative paths in stack traces, so they are easier to read
# need to adjust two paths for in and out of source builds
get_filename_component(PARENT_DIR ${CMAKE_CURRENT_SOURCE_DIR} DIRECTORY)
target_compile_options(${PROJECT_NAME} PUBLIC
  "-fdebug-prefix-map=${PARENT_DIR}/="
  "-fdebug-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/="
  ) # "-no-canonical-prefixes")

target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# on Darwin we use the /usr/bin/atos utility if available (due to the dSYM situation)
IF(NOT CMAKE_SYSTEM_NAME MATCHES "Darwin")
  find_library(DWARF dwarf HINTS /opt/local/lib /usr/local/lib)
  target_link_libraries(${PROJECT_NAME} PRIVATE ${DWARF})
  find_path(DWARF_INCLUDE_DIRS libdwarf.h PATHS /usr/local/include /usr/include /usr/include/libdwarf)
  target_include_directories(${PROJECT_NAME} PRIVATE ${DWARF_INCLUDE_DIRS})
endif()
target_compile_options(${PROJECT_NAME} BEFORE PUBLIC "-fno-omit-frame-pointer" "-fno-optimize-sibling-calls")
#"-fno-inline")
if (CMAKE_BUILD_TYPE MATCHES "Release")
  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "AppleClang")
    # only works if not statically linked (and without -Wl,--export-dynamic for linker)
    target_compile_options(${PROJECT_NAME} BEFORE PUBLIC "-gline-tables-only")
  else()
    target_compile_options(${PROJECT_NAME} BEFORE PUBLIC "-g1" "-gno-column-info")
  endif()
endif()
# if executables are build with PIE, export_dynamic is needed
IF(CMAKE_SYSTEM_NAME MATCHES "Darwin")
	target_link_options(${PROJECT_NAME} PUBLIC "-Wl,-export_dynamic")
else()
  if(CMAKE_VERSION VERSION_LESS 3.13)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--export-dynamic")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--export-dynamic")
  else()
    target_link_options(${PROJECT_NAME} PUBLIC "-Wl,--export-dynamic")
  endif()
endif()
IF(CMAKE_SYSTEM_NAME MATCHES "Linux")
  target_link_libraries(${PROJECT_NAME} PRIVATE dl)
  target_compile_options(${PROJECT_NAME} BEFORE PUBLIC "-funwind-tables")
endif()
add_executable(crashtester src/tester.cpp)
target_link_libraries(crashtester ${PROJECT_NAME})

else()

  add_library (${PROJECT_NAME} STATIC src/nocrash.cpp)
  target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

endif()

