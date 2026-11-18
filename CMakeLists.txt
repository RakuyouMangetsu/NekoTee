cmake_minimum_required(VERSION 3.16)
project(NekoTee VERSION 0.1.0 LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "NekoTee requires Linux")
endif()

set(vterm_dir third_party/libvterm)
add_library(vterm STATIC
    ${vterm_dir}/src/encoding.c
    ${vterm_dir}/src/keyboard.c
    ${vterm_dir}/src/mouse.c
    ${vterm_dir}/src/parser.c
    ${vterm_dir}/src/pen.c
    ${vterm_dir}/src/screen.c
    ${vterm_dir}/src/state.c
    ${vterm_dir}/src/unicode.c
    ${vterm_dir}/src/vterm.c
)
target_include_directories(vterm SYSTEM PUBLIC ${vterm_dir}/include)
set_target_properties(vterm PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)

add_executable(nekotee
    src/main.cpp
    src/mirror.cpp
    src/relay.cpp
    src/session.cpp
    src/terminal.cpp
    src/transcript.cpp
)
set_target_properties(nekotee PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_compile_options(nekotee PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(nekotee PRIVATE vterm util)

include(GNUInstallDirs)
install(TARGETS nekotee RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/nekotee)
install(FILES ${vterm_dir}/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/nekotee
        RENAME libvterm-LICENSE)

include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME integration COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.py $<TARGET_FILE:nekotee>)
        set_tests_properties(integration PROPERTIES TIMEOUT 45)
    endif()
endif()
