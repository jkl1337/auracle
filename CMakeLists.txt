cmake_minimum_required(VERSION 3.17)
cmake_policy(SET CMP0048 NEW)
project(auracle VERSION 0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

file(DOWNLOAD https://github.com/fmtlib/fmt/archive/6.0.0.tar.gz
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/fmt-6.0.0.tar.gz
        EXPECTED_HASH SHA256=f1907a58d5e86e6c382e51441d92ad9e23aea63827ba47fd647eacc0d3a16c78)

if (NOT IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libs/fmt-6.0.0)
    execute_process(COMMAND tar -zxf fmt-6.0.0.tar.gz
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libs)
endif()

file(DOWNLOAD https://github.com/nlohmann/json/releases/download/v3.7.1/include.zip
        ${CMAKE_CURRENT_SOURCE_DIR}/libs/nlohmann_json-3.7.1.zip
        EXPECTED_HASH SHA256=77b9f54b34e7989e6f402afb516f7ff2830df551c3a36973085e2c7a6b1045fe)
if (NOT IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libs/nlohmann_json-3.7.1)
    execute_process(COMMAND unzip -d nlohmann_json-3.7.1 nlohmann_json-3.7.1.zip
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libs)
endif()

set(json_include_directory libs/nlohmann_json-3.7.1/single_include)
add_subdirectory(libs/fmt-6.0.0)

find_package(CURL REQUIRED)
find_library(ALPM_LIBRARY NAMES alpm REQUIRED)
add_library(libalpm UNKNOWN IMPORTED)
set_property(TARGET libalpm PROPERTY IMPORTED_LOCATION "${ALPM_LIBRARY}")

find_library(SYSTEMD_LIBRARY NAMES systemd REQUIRED)
add_library(libsystemd UNKNOWN IMPORTED)
set_property(TARGET libsystemd PROPERTY IMPORTED_LOCATION "${SYSTEMD_LIBRARY}")

configure_file(config.h.in config.h @ONLY)
add_compile_options(-include config.h)

add_library(aur-lib STATIC
        src/aur/aur.cc src/aur/aur.hh
        src/aur/package.cc src/aur/package.hh
        src/aur/request.cc src/aur/request.hh
        src/aur/response.cc src/aur/response.hh
        src/aur/json_internal.hh)

target_include_directories(aur-lib PRIVATE ${json_include_directory})
target_link_libraries(aur-lib CURL::libcurl libsystemd stdc++fs)

add_library(auracle-lib STATIC
        src/auracle/auracle.cc src/auracle/auracle.hh
        src/auracle/format.cc src/auracle/format.hh
        src/auracle/package_cache.cc src/auracle/package_cache.hh
        src/auracle/pacman.cc src/auracle/pacman.hh
        src/auracle/sort.cc src/auracle/sort.hh
        src/auracle/terminal.cc src/auracle/terminal.hh)
target_include_directories(auracle-lib PRIVATE src)
target_link_libraries(auracle-lib aur-lib libalpm fmt stdc++fs)

add_executable(auracle src/auracle_main.cc)
target_include_directories(auracle PRIVATE src)
target_link_libraries(auracle auracle-lib)

add_custom_command(OUTPUT auracle.1 COMMAND
        pod2man --section=1 --center="Auracle Manual"
        --name=AURACLE --release="${PROJECT_VERSION}"
        ${CMAKE_CURRENT_SOURCE_DIR}/man/auracle.1.pod
        auracle.1
        MAIN_DEPENDENCY man/auracle.1.pod)

add_custom_target(man ALL DEPENDS auracle.1)