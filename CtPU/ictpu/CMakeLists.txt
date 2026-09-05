# SPDX-License-Identifier: GPL-3.0-or-later
# Last edited: 2026-09-03 UTC
#
# ICtPU — коннектор пути A (вендорский стек InstruStar `vdso.dll`).
#
# Дерево ОТДЕЛЬНОЕ и собирается ОТДЕЛЬНО, по распоряжению автора: у
# исходников свой каталог `ictpu/`, у сборки свой `build-ictpu/`, у
# цели своё имя с префиксом `I`. Смешение путей A и B — самая дорогая
# ошибка проекта (`docs/ACCESS_PATHS.md`), и общий каталог сборки
# сделал бы её незаметной.
#
#     cmake -S ictpu -B build-ictpu
#     cmake --build build-ictpu
#     ctest --test-dir build-ictpu --output-on-failure
#
# Проект самостоятельный: корневой CMakeLists репозитория его НЕ
# подключает и о нём не знает. Библиотека ICtPU от Qt не зависит —
# зависят только тесты (QtTest, как и в остальном репозитории).

cmake_minimum_required(VERSION 3.5 FATAL_ERROR)
project(ICtPU LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

add_library(ICtPU STATIC
    src/ivdsounits.cpp
    src/ivdsosession.cpp
    src/ivdsoloader.cpp
)
target_include_directories(ICtPU PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MSVC)
    target_compile_options(ICtPU PRIVATE /W4)
else()
    target_compile_options(ICtPU PRIVATE -Wall -Wextra)
endif()

# Загрузчик пути A живёт только под Windows; под Linux он собирается
# как честный отказ, чтобы всё, что выше него, проверялось тестами.
if(WIN32)
    target_compile_definitions(ICtPU PRIVATE UNICODE _UNICODE)
endif()

# Дымовая проверка на НАСТОЯЩЕЙ библиотеке. Собирается только под Windows:
# на других системах путь A не существует, и проверять нечего.
# Прибор для неё не нужен — проверяются загрузка, связывание всех 73
# экспортов и определённость ответа. Разбор — tools/Ismoke_vdso.cpp.
if(WIN32)
    add_executable(Ismoke_vdso tools/Ismoke_vdso.cpp)
    target_link_libraries(Ismoke_vdso PRIVATE ICtPU)
endif()

enable_testing()
add_subdirectory(tests)
