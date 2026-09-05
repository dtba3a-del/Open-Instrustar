# Last edited: 2026-08-11 11:31:17 UTC
# Test suite for CCtPU_MultiXY (TZ §14).
#
# Builds four unit-test executables and registers them with CTest:
#   test_ctpu           — CtPU::apply/applyToVector/calculateFromCalibration
#   test_mathstack       — MathStack ADD/SUB/MUL/DIV + cascade + invalid operand
#   test_pipeline        — integration: synthetic DSOsamples → CtPU → Math → asserts
#   test_movingaverage   — MovingAverage core (Step 1 of chat-confirmed plan):
#                          apply()/margin()/cutoffHz()/zoom<->N law
#   test_bintape         — MedianFilter + BinTape (ring-buffer replacement):
#                          exact merging, no erasure, exact point count, trust ratio
#
# Each test is a QtTest executable. CTest runs them via `ctest --output-on-failure`.

find_package(Qt5 COMPONENTS Core Gui Test REQUIRED)

# Add the ctpu src tree to the include path so tests can include
# ctpu.h, mathstack.h, etc. without copying them.
include_directories(${CMAKE_SOURCE_DIR}/ctpu/src)

# QtTest requires moc to process the Q_OBJECT test classes; turn on AUTOMOC
# for the test targets so the `test_*.moc` includes resolve.
set(CMAKE_AUTOMOC ON)

# Helper function: declare a QtTest executable + register it with CTest.
function(ctpu_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE Qt5::Test)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

ctpu_add_test(test_ctpu)
ctpu_add_test(test_movingaverage)
ctpu_add_test(test_autotrigger)
ctpu_add_test(test_bintape)
ctpu_add_test(test_timechannel)

# test_mathstack needs MathStack + enums.cpp (Dso::MathOpEnum definition)
add_executable(test_mathstack test_mathstack.cpp
    ${CMAKE_SOURCE_DIR}/ctpu/src/hantekdso/mathstack.cpp
    ${CMAKE_SOURCE_DIR}/ctpu/src/hantekdso/enums.cpp
)
set_target_properties(test_mathstack PROPERTIES AUTOMOC ON)
target_link_libraries(test_mathstack PRIVATE Qt5::Test)
add_test(NAME test_mathstack COMMAND test_mathstack)

# test_pipeline needs MathStack + enums.cpp + scopesettings (header-only)
add_executable(test_pipeline test_pipeline.cpp
    ${CMAKE_SOURCE_DIR}/ctpu/src/hantekdso/mathstack.cpp
    ${CMAKE_SOURCE_DIR}/ctpu/src/hantekdso/enums.cpp
)
set_target_properties(test_pipeline PROPERTIES AUTOMOC ON)
target_link_libraries(test_pipeline PRIVATE Qt5::Test)
add_test(NAME test_pipeline COMMAND test_pipeline)

# test_instrument — дерево приборов (instrument/): контракт, транспорты,
# реестр типов. Уровень, которого не было: второй прибор было некуда цеплять.
add_executable(test_instrument test_instrument.cpp
    ${CMAKE_SOURCE_DIR}/ctpu/src/instrument/instrumentregistry.cpp
)
set_target_properties(test_instrument PROPERTIES AUTOMOC ON)
target_link_libraries(test_instrument PRIVATE Qt5::Test)
add_test(NAME test_instrument COMMAND test_instrument)

# test_params — реестр параметров (params.h). Нужен сам params.cpp и
# enums.cpp: scopesettings.h ссылается на Dso::*Enum.
add_executable(test_params test_params.cpp
    ${CMAKE_SOURCE_DIR}/ctpu/src/params.cpp
    ${CMAKE_SOURCE_DIR}/ctpu/src/hantekdso/enums.cpp
)
set_target_properties(test_params PROPERTIES AUTOMOC ON)
target_link_libraries(test_params PRIVATE Qt5::Test Qt5::Gui)
add_test(NAME test_params COMMAND test_params)

# test_displaygain — регресс на расхождение масштабов при HiRes
# (`docs/PROTOTYPE-QUEUE.md` №7). Нужен enums.cpp: scopesettings.h ссылается
# на Dso::*Enum.
add_executable(test_displaygain test_displaygain.cpp
    ${CMAKE_SOURCE_DIR}/ctpu/src/hantekdso/enums.cpp
)
set_target_properties(test_displaygain PROPERTIES AUTOMOC ON)
target_link_libraries(test_displaygain PRIVATE Qt5::Test Qt5::Gui)
add_test(NAME test_displaygain COMMAND test_displaygain)

# test_cameralayer — регресс на молчаливую подмену камеры (распоряжение автора
# 2026-09-02: выбор только явный). Собирается и без Qt5Multimedia: проверяются
# отказы, а они обязаны наступать при любой сборке. Флаг CTPU_HAVE_CAMERA
# передаётся, только когда модуль найден, — как и самому приложению.
add_executable(test_cameralayer test_cameralayer.cpp
    ${CMAKE_SOURCE_DIR}/ctpu/src/cameralayer.cpp
    ${CMAKE_SOURCE_DIR}/ctpu/src/camerashaders.cpp
)
set_target_properties(test_cameralayer PROPERTIES AUTOMOC ON)
target_include_directories(test_cameralayer PRIVATE ${CMAKE_SOURCE_DIR}/ctpu/src)
target_link_libraries(test_cameralayer PRIVATE Qt5::Test Qt5::Gui)
if (Qt5Multimedia_FOUND)
    target_link_libraries(test_cameralayer PRIVATE Qt5::Multimedia)
    target_compile_definitions(test_cameralayer PRIVATE CTPU_HAVE_CAMERA)
endif()
add_test(NAME test_cameralayer COMMAND test_cameralayer)

# test_xyrecorder needs XYRecorder + PPresult + enums.cpp (Dso::*Enum definitions
# referenced from scopesettings.h / xyrecorder.cpp). Qt5::Gui supplies QVector3D,
# which PPresult stores in its graph arrays.
add_executable(test_xyrecorder test_xyrecorder.cpp
    ${CMAKE_SOURCE_DIR}/ctpu/src/xyrecorder.cpp
    ${CMAKE_SOURCE_DIR}/ctpu/src/post/ppresult.cpp
    ${CMAKE_SOURCE_DIR}/ctpu/src/hantekdso/enums.cpp
)
set_target_properties(test_xyrecorder PROPERTIES AUTOMOC ON)
target_link_libraries(test_xyrecorder PRIVATE Qt5::Test Qt5::Gui)
add_test(NAME test_xyrecorder COMMAND test_xyrecorder)
