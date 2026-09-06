// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "ui_selectsupporteddevice.h"

#include <QDialog>
#include <QPushButton>

#include "usb/scopedevice.h"
#include <memory>

struct libusb_context;

/**
 * Offers the user a device selection dialog. If you call any of the -Modal methods,
 * the method will block and show a dialog for selection or for a usb error
 * message. The method returns as soon as the user closes the dialog.
 *
 * An example to get a user selected device:
 * std::unique_ptr<USBDevice> device = SelectDevice().showSelectDeviceModal(context, ...);
 */
class SelectSupportedDevice : public QDialog {
    Q_OBJECT

  public:
    explicit SelectSupportedDevice( QWidget *parent = nullptr );

    /// \brief Окно выбора устройства.
    ///
    /// **При запуске приложения не вызывается.** Распоряжение автора
    /// 2026-09-04, повторённое 2026-09-06: «убери это окно из автозагрузки
    /// приложения. подключи его на кнопку». Причина не в удобстве: окно
    /// стояло ДО главного окна и конфигурировало железо — на машине с
    /// вендорским драйвером запуск приложения оставлял оператора с
    /// неработоспособным прибором. Теперь окно открывается только по кнопке
    /// «Выбрать устройство…» в окне «Connection», то есть по воле оператора
    /// и тогда, когда он сам решил менять оснастку.
    ///
    /// \param asDialog true — вызов из уже работающего приложения: окно
    ///        ведёт себя как обычный модальный диалог (`QDialog::exec`).
    ///        false — прежний режим до главного окна, со своим циклом
    ///        событий. Разница существенна: `QCoreApplication::quit()`,
    ///        которым закрывался прежний режим, в работающем приложении
    ///        закрыл бы всю программу.
    std::unique_ptr< ScopeDevice > showSelectDeviceModal( libusb_context *context, int verboseLevel = 0,
                                                          bool autoConnect = true, bool asDialog = false );

    /// \brief Молчаливый опрос шины: ни окна, ни записи прошивки.
    ///
    /// Ровно то, чем окно выбора БЫТЬ НЕ ДОЛЖНО. Возвращает прибор, если на
    /// шине **ровно один** готовый к работе; во всех прочих случаях nullptr,
    /// и приложение запускается в демонстрационном режиме, показав главное
    /// окно. Прибору при этом не делается ничего: ни прошивки, ни смены
    /// драйвера, ни даже вопроса оператору.
    static std::unique_ptr< ScopeDevice > probeSingleReadyDevice( libusb_context *context, int verboseLevel = 0 );

    void showLibUSBFailedDialogModel( int error );

  private:
    /// Закрыть окно тем способом, который годится для текущего режима.
    void finishSelection();

    /// Вызвано из работающего приложения — закрываться через QDialog, а не
    /// через QCoreApplication::quit().
    bool asDialog = false;

    void updateDeviceList();
    void updateSupportedDevices();
    std::unique_ptr< Ui::SelectSupportedDevice > ui;
    UniqueUSBid selectedDevice = 0;
    bool demoModeClicked = false;
    int verboseLevel = 0;
    QPushButton *btnDemoMode;

    /// Кнопка записи прошивки. Появляется только у прибора, которому
    /// прошивка нужна, и нажимается **оператором** — перечисление списка в
    /// прибор не пишет ничего (распоряжение автора 2026-09-05).
    QPushButton *btnUploadFirmware = nullptr;

    /// Модель списка живёт в showSelectDeviceModal; кнопке нужен доступ к
    /// ней, чтобы записать прошивку в выбранный прибор.
    class DevicesListModel *devicesModel = nullptr;
};
