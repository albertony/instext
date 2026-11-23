cmake_minimum_required(VERSION 3.21)
project(instext VERSION 0.1.0 DESCRIPTION "InstExt - Installer Extractor" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# All of these come from the vcpkg manifest (vcpkg.json) next to this file.
find_package(LibLZMA REQUIRED)
find_package(ZLIB REQUIRED)
find_package(BZip2 REQUIRED)
find_package(boost_hash2 CONFIG REQUIRED)
find_package(boost_nowide CONFIG REQUIRED)
find_package(boost_program_options CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)
find_package(bit7z CONFIG REQUIRED)  # loads the 7-Zip engine (7z.dll / 7z.so) at run time

add_executable(instext
    src/main.cpp src/about.cpp src/common.cpp src/hash.cpp src/report.cpp src/inno.cpp src/sevenzip.cpp
    src/msi.cpp src/burn.cpp)
# What "instext --version" prints: the version given to project() above. With --verbose it also
# names the libraries, and bit7z is the one whose headers do not tell its version.
set_source_files_properties(src/main.cpp PROPERTIES COMPILE_DEFINITIONS "INSTEXT_VERSION=\"${PROJECT_VERSION}\"")
set_source_files_properties(src/about.cpp PROPERTIES COMPILE_DEFINITIONS "INSTEXT_BIT7Z_VERSION=\"${bit7z_VERSION}\"")
target_link_libraries(instext PRIVATE
    LibLZMA::LibLZMA ZLIB::ZLIB BZip2::BZip2 Boost::hash2 Boost::nowide Boost::program_options pugixml::pugixml
    $<IF:$<TARGET_EXISTS:bit7z::bit7z64>,bit7z::bit7z64,bit7z::bit7z>)

if(WIN32)
    target_compile_definitions(instext PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(instext PRIVATE oleaut32)  # the COM string and variant functions bit7z calls

    # The version information that Windows shows in the file's properties: version
    # and description from project() above, the copyright line from LICENSE.txt.
    file(STRINGS LICENSE.txt INSTEXT_COPYRIGHT REGEX "^Copyright" LIMIT_COUNT 1)
    configure_file(src/instext.rc.in instext.rc @ONLY)
    target_sources(instext PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/instext.rc")
endif()
if(MSVC)
    target_compile_options(instext PRIVATE /W4 /utf-8)
else()
    target_compile_options(instext PRIVATE -Wall -Wextra)
endif()

# --- install: instext into <prefix>/bin together with what it needs to run ---
include(GNUInstallDirs)
install(TARGETS instext RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# The license and the notices go next to the program on Windows, where the installed folder stands
# on its own, and into the documentation folder elsewhere: <prefix>/share/doc/instext.
if(WIN32)
    set(docs ${CMAKE_INSTALL_BINDIR})
else()
    set(docs ${CMAKE_INSTALL_DOCDIR})
endif()
install(FILES LICENSE.txt DESTINATION ${docs})  # the license of instext itself

# With a DLL triplet such as x64-windows the libraries are DLLs. vcpkg's helper
# reads which DLLs the installed instext.exe imports and copies those from the
# vcpkg folder of the configured build type; with a static triplet there are
# none. It only looks at that one folder, so install must follow a build of the
# same configuration: use "cmake --build <dir> --target install". A bare
# "cmake --install" does not build, and after a reconfigure it would install
# the old program without the DLLs that one needs.
if(COMMAND x_vcpkg_install_local_dependencies)
    x_vcpkg_install_local_dependencies(TARGETS instext DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# The 7-Zip engine is loaded at run time, first from the program's own folder.
# Installing a copy there makes the installed folder self-contained. Set
# INSTEXT_7ZIP_ENGINE to another file, or to an empty value to install none.
if(WIN32)
    find_file(INSTEXT_7ZIP_ENGINE 7z.dll PATHS "$ENV{ProgramW6432}/7-Zip" "$ENV{ProgramFiles}/7-Zip" NO_DEFAULT_PATH
              DOC "7-Zip engine to install next to instext (empty: none)")
else()
    set(INSTEXT_7ZIP_ENGINE "" CACHE FILEPATH "7-Zip engine (7z.so) to install next to instext (empty: none)")
endif()
if(INSTEXT_7ZIP_ENGINE)
    install(PROGRAMS "${INSTEXT_7ZIP_ENGINE}" DESTINATION ${CMAKE_INSTALL_BINDIR})
    message(STATUS "Install will copy the 7-Zip engine ${INSTEXT_7ZIP_ENGINE}")
else()
    message(STATUS "Install will not copy 7-Zip engine; instext then looks in 7-Zip's own folder at run time")
endif()

# --- notices: the licenses of what the installed folder contains ---
# NOTICE.txt covers what is in this repository. The license texts of the
# libraries are appended to it here, from the files vcpkg installs with each
# library, so that they match the versions that were built. The libraries are
# those the manifest lists. If the 7-Zip engine is installed as well, its
# license information is added too. The result is installed with the license.
if(DEFINED VCPKG_INSTALLED_DIR)
    set(share "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
    set(rule "--------------------------------------------------------------------------------")
    file(READ NOTICE.txt notices)
    string(APPEND notices "\n\nLibraries\n---------\n\n"
           "InstExt is built with the libraries below. vcpkg recorded their versions and license texts\n"
           "when this copy was built.\n")
    file(READ vcpkg.json manifest)
    string(JSON count LENGTH "${manifest}" dependencies)
    math(EXPR last "${count} - 1")
    set(boost_used "")
    foreach(index RANGE ${last})
        string(JSON type TYPE "${manifest}" dependencies ${index})
        if(type STREQUAL "OBJECT")
            string(JSON port GET "${manifest}" dependencies ${index} name)
        else()
            string(JSON port GET "${manifest}" dependencies ${index})
        endif()
        file(READ "${share}/${port}/vcpkg.spdx.json" spdx)
        string(JSON version GET "${spdx}" packages 0 versionInfo)
        string(JSON license GET "${spdx}" packages 0 licenseConcluded)
        string(JSON homepage ERROR_VARIABLE no_homepage GET "${spdx}" packages 0 homepage)
        if(no_homepage)
            set(homepage "vcpkg port ${port}")
        endif()
        file(READ "${share}/${port}/copyright" text)
        if(port MATCHES "^boost-")  # the Boost libraries get one entry together, after the others
            list(APPEND boost_used ${port})
            set(boost_entry "Boost ${version}, https://www.boost.org/\nLicense: ${license}")
            set(boost_text "${text}")
            continue()
        endif()
        string(APPEND notices "\n${rule}\n${port} ${version}, ${homepage}\nLicense: ${license}\n${rule}\n\n${text}\n")
    endforeach()
    if(boost_used)
        # Boost libraries build on one another, so vcpkg has installed more of them than the manifest names.
        file(GLOB others RELATIVE "${share}" "${share}/boost-*/copyright")
        list(TRANSFORM others REPLACE "/copyright$" "")
        list(REMOVE_ITEM others ${boost_used} boost-cmake boost-headers)
        list(JOIN boost_used ", " boost_used)
        set(lines "")
        set(line "Their dependencies, under the same license:")
        foreach(other IN LISTS others)  # a long list, wrapped at the width of the rule
            string(LENGTH "${line} ${other}," length)
            if(length GREATER 80)
                string(APPEND lines "${line}\n")
                set(line " ")
            endif()
            string(APPEND line " ${other},")
        endforeach()
        string(REGEX REPLACE ",$" "" line "${line}")
        string(APPEND notices "\n${rule}\n${boost_entry}\nUsed: ${boost_used}\n${lines}${line}\n${rule}\n\n"
               "${boost_text}\n")
    endif()
    if(INSTEXT_7ZIP_ENGINE)
        file(READ "${share}/7zip/copyright" text)
        string(APPEND notices "\n${rule}\n7-Zip engine\n${rule}\n\n"
               "The 7-Zip library next to this program (7z.dll or 7z.so) is part of 7-Zip,\n"
               "https://www.7-zip.org/, where its source code is available. 7-Zip is licensed as follows.\n\n${text}\n")
    endif()
    string(REPLACE "\r" "" notices "${notices}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/THIRD-PARTY-NOTICES.txt" "${notices}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/THIRD-PARTY-NOTICES.txt" DESTINATION ${docs})
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS NOTICE.txt)
endif()

# --- tests: ctest --test-dir <build folder> --output-on-failure ---
option(BUILD_TESTING "Build the tests and their helper programs" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
