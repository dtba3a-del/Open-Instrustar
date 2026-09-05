// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <QColor>
#include <QObject>
#include <QPoint>
#include <QString>
#include <QVector>

#include "hantekdso/enums.h"

// These values allow a quite narrow but readable display
const QString defaultFont = "Arial";
const int defaultFontSize = 10;
const int defaultCondensed = 87; // SemiCondensed = 87%

////////////////////////////////////////////////////////////////////////////////
/// \struct DsoSettingsColorValues
/// \brief Holds the color values for the oscilloscope screen.
struct DsoSettingsColorValues {
    QColor axes;                    ///< X- and Y-axis and subdiv lines on them
    QColor background;              ///< The scope background
    QColor border;                  ///< The border of the scope screen
    QColor grid;                    ///< The color of the grid
    QColor markers;                 ///< The color of the markers
    QColor text;                    ///< The default text color
    std::vector< QColor > spectrum; ///< The colors of the spectrum graphs
    std::vector< QColor > voltage;  ///< The colors of the voltage graphs
};

////////////////////////////////////////////////////////////////////////////////
/// \struct DsoSettingsView
/// \brief Holds all view settings.
struct DsoSettingsView {
    DsoSettingsColorValues screen = { QColor( 0x7f, 0x7f, 0x7f, 0xff ), QColor( 0x00, 0x00, 0x00, 0xff ), // axes, background
                                      QColor( 0xff, 0xff, 0xff, 0xff ), QColor( 0xc0, 0xc0, 0xc0, 0xff ), // border, grid
                                      QColor( 0xc0, 0xc0, 0xc0, 0xff ), QColor( 0xff, 0xff, 0xff, 0xff ), // markers, text
                                      std::vector< QColor >(),          std::vector< QColor >() };        // spectrum, voltage
    DsoSettingsColorValues print = { QColor( 0x40, 0x40, 0x40, 0xff ), QColor( 0xff, 0xff, 0xff, 0xff ),  // axes, background
                                     QColor( 0x00, 0x00, 0x00, 0xff ), QColor( 0x40, 0x40, 0x40, 0xff ),  // border, grid
                                     QColor( 0x40, 0x40, 0x40, 0xff ), QColor( 0x00, 0x00, 0x00, 0xff ),  // markers, text
                                     std::vector< QColor >(),          std::vector< QColor >() };         // spectrum, voltage
    bool antialiasing = true;                                         ///< Antialiasing for the graphs
    /// \brief Смешивать цвета кривых в местах перекрытия (задание 6 очереди).
    ///
    /// Прежде верхняя кривая закрывала нижнюю целиком, и в точке перекрытия
    /// нижней просто не существовало — оператор не мог знать, есть она там или
    /// нет. Аддитивное смешивание даёт перекрытию цвет суммы по RGB: красная и
    /// зелёная дают жёлтую, и обе видны.
    ///
    /// Оговорка, обязательная по `docs/СЛЫШИМОСТЬ.md`: сложение осмысленно на
    /// ТЁМНОМ фоне, где сумма светлее слагаемых. На светлом фоне сумма уходит
    /// в белый и перекрытие пропадает — поэтому режим включается только при
    /// тёмном фоне, а иначе кривые рисуются полупрозрачными.
    bool blendCurveOverlap = true;

    /// \name Задание 3 очереди: кадр камеры задним слоем холста
    ///@{
    bool cameraLayerEnabled = false; ///< выводить кадр камеры под всем остальным
    /// Непрозрачность кадра, 0…1; по умолчанию кадр показывается как есть.
    ///
    /// Прежнее значение 0.45 стояло на доводе «иначе слой заглушит фон и
    /// сетку». Довод неверен: слой лежит НИЖЕ всего, сетка и кривые рисуются
    /// поверх него и им ничего не грозит, а «заглушить фон» — ровно то, ради
    /// чего слой и заведён (положение автора 2026-09-02: «фон это тот слой,
    /// перед которым должно быть изображение камеры»). Приглушение осталось
    /// регулятором — это осознанный выбор оператора, а не состояние по
    /// умолчанию. При 0.0 слоя не видно вовсе.
    double cameraLayerOpacity = 1.0;
    /// \name Ориентация кадра камеры (требование автора 2026-09-02)
    ///
    /// «Правильной» ориентации у камеры над платой нет по построению: как её
    /// поставили, так она и снимает, и знает об этом только оператор. Поэтому
    /// поворот и зеркала — регуляторы, а не константы в коде.
    ///
    /// Применяются В ОДНОМ месте — `CameraLayer`, — и потому одинаковы для
    /// холста и для снимка «фото камеры»: два снимка одного момента не могут
    /// разойтись поворотом.
    ///@{
    int cameraRotation = 0;      ///< 0, 90, 180 или 270 градусов
    bool cameraMirrorH = false;  ///< отражение слева направо
    bool cameraMirrorV = false;  ///< отражение сверху вниз
    ///@}

    /// Идентификатор выбранной камеры (`QCameraInfo::deviceName()`).
    /// **Пусто — камера не выбрана**, а не «первая доступная»: выбор только
    /// явный (распоряжение автора 2026-09-02). Запоминается идентификатор, а не
    /// описание: у двух одинаковых камер описание совпадает, а железо разное.
    QString cameraDeviceId;
    ///@}
    bool digitalPhosphor = false;                                     ///< true slowly fades out the previous graphs
    unsigned digitalPhosphorDepth = 8;                                ///< Number of channels shown at one time
    Dso::InterpolationMode interpolation = Dso::INTERPOLATION_LINEAR; ///< Interpolation mode for the graph
    bool printerColorImages = true;                                   ///< Exports images with screen colors
    int zoomHeightIndex = 2;                                          ///< Zoom scope window height
    bool zoomImage = true;                                            ///< Export zoomed images with double height
    bool zoom = false;                                                ///< true if the magnified scope is enabled
    int exportScaleValue = 1;
    Qt::ToolBarArea cursorGridPosition = Qt::RightToolBarArea;
    bool cursorsVisible = false;
    DsoSettingsColorValues *colors = &screen;
    int fontSize = defaultFontSize;
    bool styleFusion = false;
    int theme = 0;
    unsigned screenHeight = 0;
    unsigned screenWidth = 0;

    unsigned digitalPhosphorDraws() const { return digitalPhosphor ? digitalPhosphorDepth : 1; }
};
