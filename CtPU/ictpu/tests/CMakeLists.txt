# SPDX-License-Identifier: GPL-3.0-or-later
# Last edited: 2026-09-03 UTC
#
# Тесты коннектора пути A. Имена целей с префиксом I — по тому же
# правилу, что и библиотека: ни один бинарник этого дерева не должен
# перепутаться с бинарником пути B.
#
#   Itest_ivdsounits   — пересчёт единиц: мВ настроек против вольт данных,
#                        КБ против отсчётов, ряд скоростей, паспорт.
#   Itest_ivdsosession — порядок вызовов на поддельном приборе: три
#                        запрета пути A, длина по факту, восстановление.

find_package(Qt5 COMPONENTS Core Test REQUIRED)
set(CMAKE_AUTOMOC ON)

function(ictpu_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ICtPU Qt5::Test)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

ictpu_add_test(Itest_ivdsounits)
ictpu_add_test(Itest_ivdsosession)
