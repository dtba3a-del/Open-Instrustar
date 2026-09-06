// SPDX-License-Identifier: GPL-2.0-or-later

#include "selectsupporteddevice.h"

#include <QDebug>
#include <QDesktopServices>
#include <QFileInfo>
#include <QTimer>
#include <QUrl>

#include "devicelistentry.h"
#include "deviceslistmodel.h"
#include "documents.h"
#include "dsomodel.h"
#include "modelregistry.h"
#include "usb/finddevices.h"
#include "usb/uploadFirmware.h"


SelectSupportedDevice::SelectSupportedDevice( QWidget *parent ) : QDialog( parent ), ui( new Ui::SelectSupportedDevice ) {
    ui->setupUi( this );
    ui->buttonBox->button( QDialogButtonBox::Ok )->setEnabled( false );
    btnDemoMode = new QPushButton( tr( "Demo Mode" ) );
    ui->buttonBox->addButton( btnDemoMode, QDialogButtonBox::AcceptRole );

    // Запись прошивки — отдельная кнопка и отдельное действие оператора.
    // Прежде она делалась молча, самим обходом списка устройств: открытое
    // окно перепрошивало прибор раз в секунду, никого не спросив. На машине
    // с вендорским драйвером это рушило рабочее состояние — прибор уходил в
    // 1d50:608e, за который в системе не отвечает никто.
    btnUploadFirmware = new QPushButton( tr( "Upload firmware" ) );
    btnUploadFirmware->setToolTip( tr( "Write the fx2lafw firmware into the instrument. This is what puts "
                                       "the instrument onto this application's own path; it is refused "
                                       "while the vendor driver holds the device." ) );
    btnUploadFirmware->setVisible( false );
    ui->buttonBox->addButton( btnUploadFirmware, QDialogButtonBox::ActionRole );
    connect( btnUploadFirmware, &QPushButton::clicked, this, [ this ]() {
        if ( !devicesModel || ui->cmbDevices->currentIndex() == -1 )
            return;
        const UniqueUSBid id = ui->cmbDevices->currentData( Qt::UserRole ).value< UniqueUSBid >();
        QString errorMessage;
        if ( devicesModel->uploadFirmware( id, errorMessage ) ) {
            ui->labelReadyState->setText( tr( "<p>Firmware sent. The instrument re-enumerates and will "
                                              "appear in the list again in a moment.</p>" ) );
        } else {
            ui->labelReadyState->setText( tr( "<p><b>Firmware not written.</b></p><p>%1</p>" )
                                              .arg( errorMessage.toHtmlEscaped() ) );
        }
        btnUploadFirmware->setEnabled( false );
    } );
    qRegisterMetaType< UniqueUSBid >( "UniqueUSBid" );

    connect( ui->buttonBox, &QDialogButtonBox::accepted, this, [ this ]() {
        if ( ui->cmbDevices->currentIndex() != -1 ) {
            selectedDevice = ui->cmbDevices->currentData( Qt::UserRole ).value< UniqueUSBid >();
        }
        finishSelection();
    } );

    connect( ui->buttonBox, &QDialogButtonBox::helpRequested, []() {
        QUrl url;
        if ( QFile( DocPath + UserManualName ).exists() )
            url = QUrl::fromLocalFile( QFileInfo( DocPath + UserManualName ).absoluteFilePath() );
        else
            url = QUrl( DocUrl + UserManualName );
        qDebug() << "open" << url;
        QDesktopServices::openUrl( url );
    } );

    connect( btnDemoMode, &QPushButton::clicked, this, [ this ]() {
        demoModeClicked = true;
        finishSelection();
    } );
}


/// Закрытие окна зависит от того, кто его открыл. До главного окна своего
/// цикла событий у программы ещё нет, и окно крутит цикл приложения — тогда
/// закрыть его можно только `quit()`. Из работающего приложения тот же
/// `quit()` закрыл бы программу целиком, поэтому там окно закрывается как
/// обычный диалог.
void SelectSupportedDevice::finishSelection() {
    if ( asDialog )
        accept();
    else
        QCoreApplication::instance()->quit();
}


std::unique_ptr< ScopeDevice > SelectSupportedDevice::probeSingleReadyDevice( libusb_context *context, int verboseLevel ) {
    // Ни окна, ни таймера, ни записи прошивки: один проход по шине.
    FindDevices findDevices( context, verboseLevel );
    findDevices.updateDeviceList();

    const FindDevices::DeviceList *devices = findDevices.getDevices();
    if ( !devices )
        return nullptr;

    UniqueUSBid ready = 0;
    int readyCount = 0;
    for ( auto it = devices->begin(); it != devices->end(); ++it ) {
        if ( !it->second )
            continue;
        // «Готов» означает: прибор отзывается СВОИМ путём, и писать в него
        // ничего не надо. Прибору, которому нужна прошивка, здесь не делается
        // НИЧЕГО — в этом и суть распоряжения: запуск программы не имеет права
        // менять состояние железа. Он просто не считается готовым, и программа
        // открывает главное окно в демонстрационном режиме.
        if ( it->second->needsFirmware() )
            continue;
        QString errorMessage;
        if ( !it->second->connectDevice( errorMessage ) ) {
            if ( verboseLevel > 2 )
                qDebug() << "  probeSingleReadyDevice: не отвечает —" << errorMessage;
            continue;
        }
        it->second->disconnectFromDevice();
        ready = it->first;
        ++readyCount;
    }
    if ( verboseLevel )
        qDebug() << "probeSingleReadyDevice: готовых приборов" << readyCount;
    if ( readyCount != 1 )
        return nullptr;
    return findDevices.takeDevice( ready );
}


std::unique_ptr< ScopeDevice > SelectSupportedDevice::showSelectDeviceModal( libusb_context *context, int verboseLevel,
                                                                             bool autoConnect, bool asDialogMode ) {
    asDialog = asDialogMode;
    std::unique_ptr< FindDevices > findDevices = std::unique_ptr< FindDevices >( new FindDevices( context, verboseLevel ) );
    std::unique_ptr< DevicesListModel > model =
        std::unique_ptr< DevicesListModel >( new DevicesListModel( findDevices.get(), verboseLevel ) );
    // Кнопке записи прошивки нужна та же модель: запись идёт через неё.
    devicesModel = model.get();
    ui->cmbDevices->setModel( model.get() );

    QString userManualPath;
    if ( QFile( DocPath + UserManualName ).exists() )
        userManualPath = DocPath + UserManualName;
    else
        userManualPath = DocUrl + UserManualName;

    QString messageDeviceReady = tr( "<p><br/><b>The device is ready for use.</b></p><p>Please observe the "
                                     "<a href='%1'>"
                                     "user manual</a> for safe operation.</p>" )
                                     .arg( userManualPath );

    QString messageNoDevices = tr( "<p>CtPU·BinTape·TimeChannel is searching for compatible devices ...</p>"
                                   "<p><img align='right' height='150' src=':/switch_6022BL.png'>"
                                   "Don't forget to switch your device into oscilloscope mode if it has multiple modes.</p>" );
#if defined( Q_OS_WIN )
    messageNoDevices += tr( "<p>Please make sure you have installed the windows usb driver correctly</p>" );
#elif defined( Q_OS_LINUX )
    QFile libRules( "/usr/lib/udev/rules.d/60-ctpu.rules" );
    QFile etcRules( "/etc/udev/rules.d/60-ctpu.rules" );
    if ( !libRules.exists() && !etcRules.exists() ) {
        messageNoDevices += tr( "<p>Please make sure you have copied the udev rules file to<br/>"
                                "<b>%1</b> or<br/><b>%2</b><br/>"
                                "for correct USB access permissions.</p>" )
                                .arg( etcRules.fileName(), libRules.fileName() );
    }
#endif
    messageNoDevices += tr( "<hr/><p>Even without a device you can explore the program's function. "
                            "Just press the <b>Demo Mode</b> button below.</p>" );

    connect( ui->cmbDevices, static_cast< void ( QComboBox::* )( int ) >( &QComboBox::currentIndexChanged ), this,
             [ this, &messageDeviceReady ]( int index ) {
                 if ( index == -1 ) {
                     ui->buttonBox->button( QDialogButtonBox::Ok )->setEnabled( false );
                     return;
                 }
                 if ( ui->cmbDevices->currentData( Qt::UserRole + 1 ).toBool() ) { // canConnect
                     ui->buttonBox->button( QDialogButtonBox::Ok )->setEnabled( true );
                     btnUploadFirmware->setVisible( false );
                     ui->labelReadyState->setText( messageDeviceReady );
                 } else {
                     ui->buttonBox->button( QDialogButtonBox::Ok )->setEnabled( false );
                     if ( ui->cmbDevices->currentData( Qt::UserRole + 2 ).toBool() ) { // needFirmware
                         // Ничего не записано и не записывается: решение за
                         // оператором. Прибор под вендорским драйвером —
                         // рабочий прибор, и молча переводить его на другой
                         // путь недопустимо.
                         btnUploadFirmware->setVisible( true );
                         btnUploadFirmware->setEnabled( true );
                         ui->labelReadyState->setText(
                             tr( "<p>This instrument answers with its permanent firmware and needs the "
                                 "fx2lafw firmware to be used by this application.</p>"
                                 "<p><b>Nothing has been written to it.</b> Press <b>Upload firmware</b> "
                                 "if that is what you want. If the instrument is currently held by the "
                                 "vendor driver and works in the vendor software, the upload will be "
                                 "refused — switch the driver with Zadig first.</p>" ) );
                     } else { // something went wrong, inform user
                         btnUploadFirmware->setVisible( false );
                         ui->labelReadyState->setText( tr( "<p><br/><b>Connection failed!</b></p>" ) +
                                                       ui->cmbDevices->currentData( Qt::UserRole + 3 ).toString() );
                     }
                 }
             } );

    updateSupportedDevices();

    QTimer timer;
    timer.setInterval( 1000 );
    connect( &timer, &QTimer::timeout, this, [ this, &model, &findDevices, &messageDeviceReady, &messageNoDevices, autoConnect ]() {
        static int supportedDevices = -1; // max number of devices that can connect or need firmware
        static int readyDevices = -1;
        if ( findDevices->updateDeviceList() ) { // searching...
            model->updateDeviceList();
        }
        supportedDevices = qMax( supportedDevices, model->rowCount( QModelIndex() ) );
        int index = 0;
        int devices = 0;
        for ( index = 0; index < model->rowCount( QModelIndex() ); ++index ) {
            if ( ui->cmbDevices->itemData( index, Qt::UserRole + 1 ).toBool() )
                ++devices; // count devices that can connect
        }
        // printf( "%d, %d, %d devices\n", model->rowCount( QModelIndex() ), supportedDevices, devices );
        if ( 1 == devices && 1 == supportedDevices ) { // only one device ready, start it without user action
            int mIndex = 0;
            for ( mIndex = 0; mIndex < model->rowCount( QModelIndex() ); ++mIndex ) {
                if ( ui->cmbDevices->itemData( mIndex, Qt::UserRole + 1 ).toBool() ) // can connect
                    break;
            }
            ui->cmbDevices->setCurrentIndex( mIndex );
            if ( autoConnect && ui->buttonBox->button( QDialogButtonBox::Ok )->isEnabled() ) { // if scope is ready to run
                ui->buttonBox->button( QDialogButtonBox::Ok )->click();                        // start it without user activity
            }
        } else if ( devices && model->rowCount( QModelIndex() ) ) {
            // more than 1 devices ready
            if ( devices != readyDevices ) { // find 1st ready device
                int mIndex = 0;
                for ( mIndex = 0; mIndex < model->rowCount( QModelIndex() ); ++mIndex ) {
                    if ( ui->cmbDevices->itemData( mIndex, Qt::UserRole + 1 ).toBool() ) // can connect
                        break;
                }
                readyDevices = devices;
                ui->labelReadyState->setText( messageDeviceReady );
                ui->cmbDevices->setCurrentIndex( mIndex );
            }
        } else { // no devices found (not yet)
            ui->buttonBox->button( QDialogButtonBox::Ok )->setEnabled( false );
            ui->labelReadyState->setText( messageNoDevices );
        }
    } );
    timer.start();
    QCoreApplication::sendEvent( &timer, new QTimerEvent( timer.timerId() ) ); // immediate timer event

    if ( asDialog ) {
        setModal( true );
        QDialog::exec();
    } else {
        show();
        QCoreApplication::instance()->exec();
    }
    timer.stop();
    close();
    if ( demoModeClicked )
        return std::unique_ptr< ScopeDevice >( new ScopeDevice() );
    return findDevices->takeDevice( selectedDevice );
}


void SelectSupportedDevice::showLibUSBFailedDialogModel( int error ) {
    ui->labelReadyState->setText( tr( "Can't initialize USB: %1" ).arg( libUsbErrorString( error ) ) );
    ui->buttonBox->button( QDialogButtonBox::Ok )->setEnabled( false );
    show();
    QCoreApplication::instance()->exec();
    close();
}


void SelectSupportedDevice::updateSupportedDevices() {
    QString devices;
    for ( const DSOModel *model : ModelRegistry::get()->models() ) {
        devices.append( model->name ).append( " " );
    }
    ui->labelSupportedDevices->setText( devices );
}
