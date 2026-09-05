// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file cameradialog.h
/// \brief Окно камеры: выбор прибора, включение слоя, регуляторы драйвера.
///
/// Распоряжение автора 2026-09-02: «в приложении должна быть возможность
/// выбрать и запомнить выбор камеры, и если стандартный драйвер позволяет
/// что-либо настраивать у камеры, отобрази эти регуляторы и опцию вкл/выкл, в
/// окне, которое открывается кнопкой». И отдельно: «нам встроенная камера НЕ
/// нужна. нужен явный выбор конкретной из списка доступных».
///
/// Поэтому в списке первым пунктом стоит **«не выбрана»**, и он же
/// подставляется, когда запомненной камеры в системе нет. Ни при каких
/// обстоятельствах окно не выбирает камеру за оператора.
///
/// Своё окно есть у настройки, но не у изображения: кадр камеры живёт слоем на
/// холсте, под сеткой и кривыми, и отдельного окна с картинкой в программе нет
/// (`docs/CAMERA-LAYER.md` §1).

#include <QDialog>
#include <QList>
#include <QMap>

class CameraLayer;
struct DsoSettingsView;

class QCheckBox;
class QComboBox;
class QGroupBox;
class QLabel;
class QSlider;
class QVBoxLayout;

/// \brief Окно настройки камеры, открывается кнопкой панели.
class CameraDialog : public QDialog {
    Q_OBJECT
  public:
    CameraDialog( DsoSettingsView *view, CameraLayer *layer, QWidget *parent = nullptr );

  public:
    /// \brief Сообщение о том, что слой ХОЛСТА не построен.
    ///
    /// Камера может исправно отдавать кадры, а на холсте их не быть: слой
    /// холста — отдельная ступень, и её отказ не отказ камеры. Ровно это и
    /// случилось (`docs/CAMERA-LAYER.md` §10), причём молча. Строку задаёт
    /// окно-владелец, потому что о холсте знает оно, а не камера.
    void setCanvasDiagnostic( const QString &text );

  public slots:
    /// Применить выбор: открыть камеру и записать выбор в настройки.
    ///
    /// Отдельной кнопкой по требованию автора 2026-09-02: «чтобы предотвратить
    /// применение параметров только закрытием окна». Закрытие окна ничего не
    /// применяет.
    void applySelection();

  signals:
    /// Слой камеры включён, выключен, сменил прибор или прозрачность.
    void cameraChanged();
    /// Выбор применён — настройки пора записать на диск.
    void cameraChoiceApplied();

  private:
    QString canvasDiagnostic;
    /// Записать ориентацию из окна в настройки и в слой.
    void applyOrientation();
    void reloadDeviceList();
    void markPending();
    void rebuildControls();
    void updateStatus();

    /// Есть непринятые изменения: кнопка «Применить» ждёт нажатия.
    bool pending = false;

    DsoSettingsView *view;
    CameraLayer *layer;

    QComboBox *deviceBox = nullptr;
    QCheckBox *enabledBox = nullptr;
    QLabel *statusLabel = nullptr;
    QSlider *opacitySlider = nullptr;
    /// \name Ориентация кадра: поворот и зеркала
    /// Действуют СРАЗУ, как и прозрачность: подбирать ориентацию вслепую,
    /// через «Применить», значило бы гадать. «Применить» отвечает за выбор
    /// прибора, а не за то, что видно немедленно.
    ///@{
    QComboBox *rotationBox = nullptr;
    QCheckBox *mirrorHBox = nullptr;
    QCheckBox *mirrorVBox = nullptr;
    ///@}
    QLabel *opacityLabel = nullptr;
    QGroupBox *controlsGroup = nullptr;
    QVBoxLayout *controlsLayout = nullptr;
    QCheckBox *autoWhiteBalanceBox = nullptr;
    class QPushButton *applyButton = nullptr;
    /// Отложенная запись прозрачности.
    ///
    /// Ползунок шлёт сигнал на каждый шаг; писать настройки на каждый шаг
    /// значило бы дёргать диск сотнями записей за один жест. Таймер сводит жест
    /// к одной записи и при этом не теряет её: ждать закрытия окна нельзя -
    /// именно от «применилось само при закрытии» автор и отказался.
    class QTimer *opacitySaveTimer = nullptr;
};
