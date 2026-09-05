###################################
#
# Add more languages here
#
set(TS_FILES translations/ctpu_de.ts translations/ctpu_es.ts translations/ctpu_fr.ts translations/ctpu_it.ts translations/ctpu_pl.ts translations/ctpu_pt.ts translations/ctpu_ru.ts translations/ctpu_sv.ts translations/ctpu_zh.ts)
#
###################################

# Find the Qt linguist tool
find_package(Qt5 REQUIRED COMPONENTS LinguistTools)

# defines files with translatable strings.
set(INPUT ${SRC} ${HEADERS} ${UI})

# prepares 'lupdate' to update ts files and also 'lcreate' to build qm files.
qt5_create_translation(QM_FILES ${INPUT} ${TS_FILES})

# prepare the translations.qrc file and insert all available compiled translation files now.
set(QRC_ITEMS "")
foreach(QM_FILE ${QM_FILES})
        get_filename_component(FILENAME "${QM_FILE}" NAME)
        set(QRC_ITEMS "${QRC_ITEMS}\n<file alias=\"${FILENAME}\">${QM_FILE}</file>")
endforeach()
configure_file("${CMAKE_CURRENT_LIST_DIR}/translations.qrc.template" "${CMAKE_BINARY_DIR}/translations.qrc" @ONLY)

QT5_ADD_RESOURCES(TRANSLATION_QRC "${CMAKE_BINARY_DIR}/translations.qrc")
