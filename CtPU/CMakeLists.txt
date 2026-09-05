cmake_minimum_required(VERSION 3.5 FATAL_ERROR)
project(CtpuBintapeTimechannel)

find_package(Git)

if (GIT_EXECUTABLE AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    # show modified files
    execute_process(
        COMMAND ${GIT_EXECUTABLE} status --short
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    # get the git version
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --exclude latest --exclude build-* --long --dirty
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_DESCRIBE_STATUS
    )
    if( NOT GIT_DESCRIBE_STATUS EQUAL 0 OR VERSION STREQUAL "" )
        # Репозиторий без тегов. После чистого старта тегов нет вообще, и
        # `git describe --tags` завершается с "No names found": VERSION
        # остаётся пустой, а string(FIND) ниже получает на один аргумент
        # меньше и валит ВСЮ конфигурацию — собрать проект нельзя, пока
        # кто-нибудь не расставит теги. Сборка не должна от этого зависеть:
        # берём короткий SHA и идём дальше.
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE GIT_SHORT_SHA
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        # Разряды R.T.B живут в ctpu/src/CTPU_VERSION.h и не зависят от того,
        # дошли ли теги до сборочной машины. На origin версионных тегов нет
        # вовсе: их ставит локальный хук post-commit, а отправка тега из
        # сессии запрещена (HTTP 403). Без этой ветки CI собирал бинарник с
        # версией `0.0.0-0-g<sha>` - то есть номер сборки, который исполнитель
        # обязан поднимать на каждый бинарник, до самого бинарника не доходил.
        set( HEADER_VERSION "" )
        if( EXISTS "${CMAKE_SOURCE_DIR}/ctpu/src/CTPU_VERSION.h" )
            file( STRINGS "${CMAKE_SOURCE_DIR}/ctpu/src/CTPU_VERSION.h" VERSION_LINES
                  REGEX "^#define[ \t]+(LAST_)?CTPU_VERSION[ \t]+\"[^\"]+\"" )
            foreach( LINE ${VERSION_LINES} )
                string( REGEX MATCH "\"([^\"]+)\"" _MATCHED "${LINE}" )
                if( CMAKE_MATCH_1 )
                    set( HEADER_VERSION "${CMAKE_MATCH_1}" )
                    break()
                endif()
            endforeach()
        endif()
        if( HEADER_VERSION STREQUAL "" )
            set( HEADER_VERSION "0.0.0" )
        endif()
        if( GIT_SHORT_SHA STREQUAL "" )
            set( VERSION "${HEADER_VERSION}" )
        else()
            set( VERSION "${HEADER_VERSION}-0-g${GIT_SHORT_SHA}" )
        endif()
        message(STATUS "No git tag found, version taken from CTPU_VERSION.h: ${VERSION}")
    else()
        # devdrop or release?
        string(FIND "${VERSION}" "devdrop-0-g" DEVDROP_POS)
        string(FIND "${VERSION}" "-0-g" ZERO_POS)
        if( ${DEVDROP_POS} EQUAL 0 ) # "x.y.z-p-gxxxxxxx(-dirty)-devdrop"
            execute_process(
                COMMAND ${GIT_EXECUTABLE} describe --exclude latest --exclude build-* --long --dirty
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE VERSION
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            set( VERSION "${VERSION}-devdrop" )
        elseif( ${ZERO_POS} GREATER 0 ) # x.y.z(-dirty)
            # message(STATUS "TAG")
            execute_process(
                COMMAND ${GIT_EXECUTABLE} describe --exclude latest --exclude build-* --dirty
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE VERSION
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
        else() # "x.y.z-p-gxxxxxxx(-dirty)"
            execute_process(
                COMMAND ${GIT_EXECUTABLE} describe --exclude latest --exclude build-* --long --dirty
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE VERSION
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
        endif()
    endif()
    set( PACKAGE_VERSION ${VERSION} )
endif()

# Дерева без `.git` весь блок выше не касается вовсе, и PACKAGE_VERSION
# оставался ПУСТЫМ: имя пакета вырождалось в `ctpubintapetimechannel__x86_64`,
# то есть номер сборки до архива не доходил. Так собирается и распакованный
# архив соответствующего исходника (GPL-3.0 §6), и дерево приложения,
# лежащее ПОДКАТАЛОГОМ чужого репозитория, — там `.git` у родителя, а не
# у `CMAKE_SOURCE_DIR`.
#
# Разряды R.T.B живут в `ctpu/src/CTPU_VERSION.h` и от наличия git не
# зависят: это и есть источник версии, а `git describe` — лишь уточнение.
# Кавычки вокруг ${PACKAGE_VERSION} обязательны: у НЕОПРЕДЕЛЁННОЙ переменной
# `if( VAR STREQUAL "" )` сравнивает не значение, а само имя "PACKAGE_VERSION"
# со строкой "" и даёт ЛОЖЬ. Ровно на этом первая редакция ветки и молчала.
if( "${PACKAGE_VERSION}" STREQUAL "" )
    set( HEADER_VERSION "" )
    if( EXISTS "${CMAKE_SOURCE_DIR}/ctpu/src/CTPU_VERSION.h" )
        file( STRINGS "${CMAKE_SOURCE_DIR}/ctpu/src/CTPU_VERSION.h" VERSION_LINES
              REGEX "^#define[ \t]+(LAST_)?CTPU_VERSION[ \t]+\"[^\"]+\"" )
        foreach( LINE ${VERSION_LINES} )
            string( REGEX MATCH "\"([^\"]+)\"" _MATCHED "${LINE}" )
            if( CMAKE_MATCH_1 )
                set( HEADER_VERSION "${CMAKE_MATCH_1}" )
                break()
            endif()
        endforeach()
    endif()
    if( "${HEADER_VERSION}" STREQUAL "" )
        set( HEADER_VERSION "0.0.0" )
    endif()
    set( PACKAGE_VERSION "${HEADER_VERSION}" )
    message(STATUS "Дерево без .git: версия взята из CTPU_VERSION.h: ${PACKAGE_VERSION}")
endif()

message(STATUS "PACKAGE_VERSION: ${PACKAGE_VERSION}")

set(OpenGL_GL_PREFERENCE GLVND)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default build type
IF(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
   SET(CMAKE_BUILD_TYPE RelWithDebInfo)
ENDIF(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)

# Find external libraries
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake/")

INCLUDE_DIRECTORIES(".")

if(MSVC)
  # NOMINMAX to avoid clash with std::min or std::max due to silly macros min and max
  add_compile_options(/W4 /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX)
else()
  add_compile_options(-Wall -Wextra -pedantic)
endif()


# Silence nasty warnings "parameter passing for argument of type ... changed in GCC 7.1"
if(CMAKE_COMPILER_IS_GNUCXX)
  add_compile_options(-Wno-psabi)
endif()

# enable extra feature(s)
if( ${CMAKE_VERSION} VERSION_LESS "3.12.0" ) # deprecated, but still used in older Ubuntu releases
    # Enable C++ standard library hardening -> cheap range checks for C++ arrays, vectors, and strings.
    add_definitions( -D_GLIBCXX_ASSERTIONS )
    add_definitions( -D_USE_MATH_DEFINES )
    if ( DEFINED VERSION )
        add_definitions( -DVERSION="${VERSION}" )
    endif()
else() # 'add_compile_definitions()' was introduced with CMake 3.12
    add_compile_definitions( _GLIBCXX_ASSERTIONS )
    add_compile_definitions( _USE_MATH_DEFINES )
    if ( DEFINED VERSION )
        add_compile_definitions( VERSION="${VERSION}" )
    endif()
endif()

# show all compile options and definitions
get_directory_property( CompOpts COMPILE_OPTIONS )
message( "-- COMPILE_OPTIONS: ${CompOpts}" )
get_directory_property( CompDefs COMPILE_DEFINITIONS )
message( "-- COMPILE_DEFINITIONS: ${CompDefs}" )

# Есть ли Qt Multimedia - решается ЗДЕСЬ, один раз, и читается и приложением, и
# тестами. Прежде find_package стоял только в ctpu/: результат в область tests/
# не попадал (соседние подкаталоги друг друга не видят), и тест поверхности
# камеры молча уходил в SKIP - то есть проверка была, а проверял её никто.
# Одна величина - одно место, где она определяется.
find_package(Qt5Multimedia QUIET)
find_package(Qt5MultimediaWidgets QUIET)

# Qt Widgets based Gui with OpenGL canvas
add_subdirectory(ctpu)

# [MOD] TZ §14.1 — unit + integration tests via QtTest + CTest.
# Build only when BUILD_TESTING is ON (default ON for the developer build).
option(BUILD_TESTING "Build the CCtPU_MultiXY unit tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

if (WIN32)
    install(
        FILES CHANGELOG LICENSE README.md
        DESTINATION "."
    )
endif()

# Add auxiliary files to the project, so that these files appear in VisualStudio/QtCreator
file(GLOB_RECURSE MDFILES "docs/*.md" "ctpu/*.md")
add_custom_target(
    readme
    SOURCES CHANGELOG LICENSE README.md ${MDFILES}
)

# Use CPack to make tgz/deb/rpm/zip installer packages
include(cmake/CPackInfos.cmake)

