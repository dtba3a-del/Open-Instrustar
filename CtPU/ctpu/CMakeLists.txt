# ctpu/CMakeLists.txt

project(CtpuBintapeTimechannel CXX)

if(WIN32)
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    set(CMAKE_EXE_LINKER_FLAGS "-s")
    set(EXECTYPE WIN32)
endif()

find_package(Qt5Widgets REQUIRED)
# Задание 3 очереди: кадр с веб-камеры задним слоем холста. Модуль
# необязателен — без него приложение собирается и работает, камера просто не
# даёт кадров. Отсутствие камеры не должно ронять прибор.
# Ищется он в корневом CMakeLists.txt — один раз на всё дерево, чтобы тесты
# видели тот же ответ, что и приложение.
find_package(Qt5PrintSupport REQUIRED)
find_package(Qt5OpenGL REQUIRED)
find_package(OpenGL)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

if (Qt5Widgets_VERSION VERSION_LESS 5.4.0)
    message(FATAL_ERROR "Minimum supported Qt5 version is 5.4.0!")
else()
    message("-- Qt5 version is ${Qt5Widgets_VERSION}")
endif()

# include directories
set(CMAKE_INCLUDE_CURRENT_DIR ON)
include_directories(src/ src/hantekdso src/widgets src/docks src/configdialog)

# collect sources and other files
file(GLOB_RECURSE SRC "src/*.cpp")
file(GLOB_RECURSE HEADERS "src/*.h")

# Коннектор пути A. Дерево исходников остаётся отдельным (`ictpu/`) — как и
# было велено, чтобы пути A и B не смешивались при разборе. Но собранным он
# должен быть В ПРИЛОЖЕНИИ: иначе окно генератора остаётся картинкой, а
# библиотека вендора, уже установленная на машине оператора, никем не
# грузится. Отдельная сборка `build-ictpu` при этом никуда не девается —
# она гоняет тесты коннектора без Qt и без прибора.
#
# Про `vdso.dll` в архиве — блок ниже, у POST_BUILD. Здесь только исходники
# коннектора; сама библиотека вендора ни во что не линкуется, она грузится в
# работе (`docs/ВЫЗОВ-ЧУЖИХ-МОДУЛЕЙ.md`).
list(APPEND SRC
    ${CMAKE_SOURCE_DIR}/ictpu/src/ivdsounits.cpp
    ${CMAKE_SOURCE_DIR}/ictpu/src/ivdsosession.cpp
    ${CMAKE_SOURCE_DIR}/ictpu/src/ivdsoloader.cpp
)
include_directories(${CMAKE_SOURCE_DIR}/ictpu/src)

file(GLOB_RECURSE UI "src/*.ui")
file(GLOB_RECURSE QRC "res/*.qrc")

if(WIN32)
    file(GLOB_RECURSE RC "res/*.rc")
endif()

add_custom_target(format SOURCES ".clang-format"
    COMMAND "clang-format" "-style=file" "-i" "-sort-includes" ${SRC} ${HEADERS})

# here the translation magic happens
include(translations/Translations.cmake)

option(USE_OPENHANTEK_DRIVER "Use OpenHantek Windows driver" ON)

# Enable MacOSX bundle magic in the next line
option(BUILD_MACOSX_BUNDLE "Build MacOS app bundle" ON)
#
if( APPLE AND BUILD_MACOSX_BUNDLE )
    set( EXECTYPE MACOSX_BUNDLE )
    set( MACOSX_BUNDLE_BUNDLE_NAME "${PROJECT_NAME}" )
    set( MACOSX_BUNDLE_INFO_STRING "Digital oscilloscope software for Hantek DSO6022" )
    set( MACOSX_BUNDLE_GUI_IDENTIFIER "${PROJECT_NAME}" )
    set( MACOSX_BUNDLE_SHORT_VERSION_STRING ${VCS_REVISION} )
    set( MACOSX_BUNDLE_BUNDLE_VERSION ${VERSION} )
    string(TIMESTAMP THISYEAR "%Y")
    set( MACOSX_BUNDLE_COPYRIGHT "(C) 2012-${THISYEAR} OpenHantek community; CtPU/BinTape/TimeChannel additions (C) dtba3a-del" )
    set( MACOSX_BUNDLE_ICON_FILE ctpu.icns )
    set( ICONS ${CMAKE_CURRENT_SOURCE_DIR}/res/images/ctpu.icns )
    set_source_files_properties( ${ICONS} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources" )
endif()

# make executable
add_executable(${PROJECT_NAME} ${EXECTYPE} ${SRC} ${HEADERS} ${UI}
${QRC} ${RC} ${TRANSLATION_BIN_FILES} ${TRANSLATION_QRC} ${ICONS})
target_link_libraries(${PROJECT_NAME} Qt5::Widgets Qt5::PrintSupport Qt5::OpenGL ${OPENGL_LIBRARIES} )

# Вендорская `vdso.dll` кладётся рядом с исполняемым файлом.
#
# **Распоряжение автора 2026-09-06:** «скопировать vdso.dll в сборку
# допускается. Это НЕ публичный релиз.» Решение принадлежит автору проекта:
# сборка идёт в его приватный репозиторий и предназначена ему; публичного
# распространения, к которому относится ограничение GPL-3.0 §1, здесь нет.
# Прежний отказ исполнителя класть файл был перенесён с публичного выпуска на
# частную сборку — область запрета названа неверно.
#
# **Место блока значимо:** `add_custom_command(TARGET …)` требует уже
# СОЗДАННОЙ цели. В первой редакции он стоял выше `add_executable`, и
# конфигурация падала с «No TARGET has been created in this directory» — под
# Linux это не проявлялось, потому что блок под `if(WIN32)`. Поймал новый шаг
# проверки, ради которого он и заведён.
#
# Если файл отсутствует, сборка не падает: она собирается и без него, а окно
# генератора тогда просит указать путь.
#
# **Мест поиска два, и это не двоевластие** (`docs/ДВОЕВЛАСТИЕ.md`): величину
# порождает не список, а первый существующий файл в нём, и правило это одно.
# Дерево приложения живёт в двух репозиториях — приватном, где библиотека
# лежит в `references/vendor/`, и публичном, где рядом с деревом кладётся
# `vendor/vdso.dll`. Один и тот же CMakeLists должен работать в обоих, иначе
# копии разойдутся, а расхождение копий дороже списка из двух строк.
if(WIN32)
    set(VDSO_CANDIDATES
        "${CMAKE_SOURCE_DIR}/references/vendor/instrustar-sdk/bin/vdso.dll"
        "${CMAKE_SOURCE_DIR}/vendor/vdso.dll"
    )
    set(VDSO_DLL "")
    foreach(CAND ${VDSO_CANDIDATES})
        if(EXISTS "${CAND}")
            set(VDSO_DLL "${CAND}")
            break()
        endif()
    endforeach()
    if(NOT VDSO_DLL STREQUAL "")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VDSO_DLL}" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/vdso.dll"
            COMMENT "vdso.dll рядом с исполняемым файлом (путь A)")
    else()
        message(WARNING "vdso.dll не найдена ни в одном из ${VDSO_CANDIDATES} — окно генератора попросит указать путь")
    endif()
endif()
# Пробник Oscill: замер НА ПРИБОРЕ нашим же кодом.
#
# Распоряжение автора 2026-09-06: «важно что бы ты написал его корректно коду.
# и получил искомое, против угаданного?» Поэтому пробник не повторяет протокол
# своими средствами — он линкует те же SerialTransport и OscillBackend, что и
# приложение, и его вывод говорит о НАШЕЙ ПРОГРАММЕ, а не только о приборе.
#
# Цель отдельная от приложения: у неё свой main и она не тянет ни виджеты, ни
# OpenGL. В архив поставки попадает рядом с приложением.
add_executable(Ioscill_probe
    tools/Ioscill_probe.cpp
    src/instrument/oscill/obexcodec.cpp
    src/instrument/oscill/oscillprotocol.cpp
    src/instrument/oscill/oscillbackend.cpp
    src/instrument/oscill/serialtransport.cpp
    src/instrument/instrumentregistry.cpp
)
target_include_directories(Ioscill_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(Ioscill_probe Qt5::Core)
if (WIN32)
    target_link_libraries(Ioscill_probe setupapi)
endif()

if (Qt5Multimedia_FOUND)
    target_link_libraries(${PROJECT_NAME} Qt5::Multimedia)
    target_compile_definitions(${PROJECT_NAME} PRIVATE CTPU_HAVE_CAMERA)
    message("-- Camera layer: enabled (Qt5Multimedia found)")
else()
    message("-- Camera layer: disabled (Qt5Multimedia not found) - application builds without it")
endif()
target_compile_features(${PROJECT_NAME} PRIVATE cxx_range_for)


if(MSVC)
    include(../cmake/fftw_on_windows.cmake)
    include(../cmake/libusb_on_windows.cmake)
    include(../cmake/copy_qt5_dlls_to_bin_dir.cmake)
    target_compile_options(${PROJECT_NAME} PRIVATE "/W4" "/wd4251" "/wd4127" "/wd4275" "/wd4200" "/nologo" "/J" "/Zi")
    target_compile_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:DEBUG>:/MDd>")
else()
    if( APPLE AND BUILD_MACOSX_BUNDLE )
        # Use own template that defines NSPrincipalClass=NSApplication & NSHighResolutionCapable=True
        set_target_properties( ${PROJECT_NAME} PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_LIST_DIR}/../cmake/CtpuBundleInfo.plist.in
        )
    endif()
    find_package(libusb REQUIRED)
    target_include_directories(${PROJECT_NAME} PRIVATE ${LIBUSB_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} ${LIBUSB_LIBRARIES})

# SetupAPI: определение класса драйвера, в котором находится дескриптор
# устройства (`src/usb/driverclass.cpp`). Нужен только под Windows — на
# других системах вендорского стека нет вовсе и различать нечего.
if(WIN32)
    target_link_libraries(${PROJECT_NAME} setupapi)
endif()

    find_package(Threads REQUIRED)
    target_link_libraries(${PROJECT_NAME} ${CMAKE_THREAD_LIBS_INIT})

    find_package(FFTW REQUIRED)
    target_include_directories(${PROJECT_NAME} PRIVATE ${FFTW_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} ${FFTW_LIBRARIES})

    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wno-long-long -pedantic)
    target_compile_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:DEBUG>:-DDEBUG>")
    target_compile_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:DEBUG>:-O0>")
    target_compile_options(${PROJECT_NAME} PRIVATE "$<$<CONFIG:RELEASE>:-fno-rtti>")
endif()

if(WIN32)
    include(../cmake/docs_on_windows.cmake)
    include(../cmake/windows_driver.cmake)
endif()

# install commands
if ( NOT (APPLE AND BUILD_MACOSX_BUNDLE) )
    install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION "bin")
endif()
