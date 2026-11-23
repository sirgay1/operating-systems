cmake_minimum_required(VERSION 3.16)
project(lab2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)
find_package(Threads REQUIRED)

file(GLOB sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
file(GLOB transports CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/conn_*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/conn_*.c")
list(REMOVE_ITEM sources ${transports}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/host.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/client.cpp")

if(sources)
    add_library(lab_common OBJECT ${sources})
    target_compile_options(lab_common PRIVATE -Wall -Werror)
    target_link_libraries(lab_common PRIVATE Threads::Threads)
endif()

foreach(transport IN LISTS transports)
    get_filename_component(name "${transport}" NAME_WE)
    string(REGEX REPLACE "^conn_" "" type "${name}")

    add_library("${name}" OBJECT "${transport}")
    target_compile_options("${name}" PRIVATE -Wall -Werror)
    target_compile_definitions("${name}" PRIVATE LAB_TRANSPORT="${type}")
    target_link_libraries("${name}" PRIVATE Threads::Threads)

    foreach(role IN ITEMS host client)
        set(target "${role}_${type}")
        add_executable("${target}" "src/${role}.cpp")
        target_compile_options("${target}" PRIVATE -Wall -Werror)
        target_link_libraries("${target}" PRIVATE "${name}" Threads::Threads)
        if(TARGET lab_common)
            target_link_libraries("${target}" PRIVATE lab_common)
        endif()
        if(role STREQUAL "host")
            target_link_libraries("${target}" PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)
        endif()
    endforeach()
endforeach()
