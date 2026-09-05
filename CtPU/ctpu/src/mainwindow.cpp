// SPDX-License-Identifier: GPL-2.0-or-later
// Last edited: 2026-08-11 11:31:17 UTC

#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "HorizontalDock.h"
#include "SpectrumDock.h"
#include "TriggerDock.h"
#include "VoltageDock.h"
#include "dockwindows.h"

#include "configdialog.h"
#include "dockwindows.h"
#include "documents.h"
#include "dsomodel.h"
#include "dsowidget.h"
#include "exporting/exporterinterface.h"
#include "exporting/exporterregistry.h"
#include "hantekdsocontrol.h"
#include "usb/scopedevice.h"
#include "viewconstants.h"

#include "dsosettings.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QToolButton>

#include "cameradialog.h"
#include "connectiondialog.h"
#include "ddsdialog.h"
#include "vdsobackend.h"
#include <QPainter>
#include <QPalette>
#include <QPrintDialog>
#include <QPrinter>
#include <QTimer>
#include <QValidator>

#include "CTPU_VERSION.h"

MainWindow::MainWindow( HantekDsoControl *dsoControl, DsoSettings *settings, ExporterRegistry *exporterRegistry, QWidget *parent )
    : QMainWindow( parent ), ui( new Ui::MainWindow ), dsoSettings( settings ), exporterRegistry( exporterRegistry ) {

    if ( dsoSettings->scope.verboseLevel > 1 )
        qDebug() << " MainWindow::MainWindow()";

    // suppress nasty warnings, e.g. "kf5.kio.core: Invalid URL ..." or "qt.qpa.xcb: QXcbConnection: XCB error: 3 (BadWindow) ..."
    QLoggingCategory::setFilterRules( "kf5.kio.core=false\nqt.qpa.xcb=false" );
    QVariantMap colorMap;
    QString iconPath = QString( ":/images/" );
    if ( QPalette().color( QPalette::Window ).lightness() < 128 ) { // automatic light/dark icon switch
        iconPath += "darktheme/";                                   // select top window icons accordingly
        colorMap.insert( "color-off", QColor( 208, 208, 208 ) );    // light grey normal
        colorMap.insert( "color-active", QColor( 255, 255, 255 ) ); // white when selected
    } else {
        iconPath += "lighttheme/";
    }
    elapsedTime.start();

    ui->setupUi( this );
    iconPause = QIcon( iconPath + "pause.svg" );
    iconPlay = QIcon( iconPath + "play.svg" );
    ui->actionSampling->setIcon( iconPause );
    QList< QKeySequence > shortcuts; // provide multiple shortcuts for ui->actionSampling.
    // 1st entry in list is shown as shortcut in menu
#ifdef Q_OS_WIN
    shortcuts << QKeySequence( Qt::Key::Key_S ); // WIN: <space> can be grabbed by buttons, e.g. CH1
#endif
    shortcuts << QKeySequence( Qt::Key::Key_Space );
    shortcuts << QKeySequence( Qt::Key::Key_Pause );
#ifndef Q_OS_WIN
    shortcuts << QKeySequence( Qt::Key::Key_S ); // else put this shortcut at the end of the list
#endif
    ui->actionSampling->setShortcuts( shortcuts );
    if ( dsoSettings->scope.toolTipVisible )
        ui->actionSampling->setToolTip( tr( "Start and stop the sampling" ) );
    else
        ui->actionSampling->setToolTip( QString() );
    ui->actionRefresh->setIcon( QIcon( iconPath + "refresh.svg" ) );
    ui->actionRefresh->setShortcut( Qt::Key::Key_R );
    if ( dsoSettings->scope.toolTipVisible )
        ui->actionRefresh->setToolTip( tr( "Refresh the screen trace for slow 'Roll' mode" ) );
    else
        ui->actionRefresh->setToolTip( QString() );
    ui->actionPhosphor->setIcon( QIcon( iconPath + "phosphor.svg" ) );
    ui->actionPhosphor->setShortcut( Qt::Key::Key_P );
    if ( dsoSettings->scope.toolTipVisible )
        ui->actionPhosphor->setToolTip( tr( "Let the traces fade out slowly" ) );
    else
        ui->actionPhosphor->setToolTip( QString() );
    ui->actionHistogram->setIcon( QIcon( iconPath + "histogram.svg" ) );
    ui->actionHistogram->setShortcut( Qt::Key::Key_H );
    if ( dsoSettings->scope.toolTipVisible )
        ui->actionHistogram->setToolTip( tr( "Show a histogram of the voltage levels on the right side of the trace" ) );
    else
        ui->actionHistogram->setToolTip( QString() );
    ui->actionZoom->setIcon( QIcon( iconPath + "zoom.svg" ) );
    ui->actionZoom->setShortcut( Qt::Key::Key_Z );
    if ( dsoSettings->scope.toolTipVisible )
        ui->actionZoom->setToolTip( tr( "Zoom the range between the markers '1' and '2'" ) );
    else
        ui->actionZoom->setToolTip( QString() );
    ui->actionMeasure->setIcon( QIcon( iconPath + "measure.svg" ) );
    ui->actionMeasure->setShortcut( Qt::Key::Key_M );
    if ( dsoSettings->scope.toolTipVisible )
        ui->actionMeasure->setToolTip( tr( "Enable cursor measurements" ) );
    else
        ui->actionMeasure->setToolTip( QString() );
    ui->actionOpen->setIcon( QIcon( iconPath + "open.svg" ) );
    ui->actionOpen->setToolTip( tr( "Load scope settings from a config file" ) );
    ui->actionSave->setIcon( QIcon( iconPath + "save.svg" ) );
    ui->actionSave->setToolTip( tr( "Save the scope settings to the default location" ) );
    ui->actionSave_as->setIcon( QIcon( iconPath + "save_as.svg" ) );
    ui->actionSave_as->setToolTip( tr( "Save the scope settings to a user defined file" ) );
    ui->actionExit->setIcon( QIcon( iconPath + "exit.svg" ) );
    ui->actionExit->setToolTip( tr( "Exit the application" ) );
    ui->actionSettings->setIcon( QIcon( iconPath + "settings.svg" ) );
    ui->actionSettings->setToolTip( tr( "Define scope settings, analysis parameters and colors" ) );
    ui->actionCalibrateOffset->setIcon( QIcon( iconPath + "offset.svg" ) );
    ui->actionCalibrateOffset->setToolTip( tr( "Short-circuit both inputs and slowly select all voltage gain settings" ) );
    ui->actionManualCommand->setIcon( QIcon( iconPath + "terminal.svg" ) );
    ui->actionManualCommand->setToolTip( tr( "Send low level commands directly to the scope: 'CC XX XX'" ) );
    ui->actionUserManual->setIcon( QIcon( iconPath + "book.svg" ) );
    ui->actionUserManual->setToolTip( tr( "Read the fine manual" ) );
    ui->actionACmodification->setIcon( QIcon( iconPath + "book.svg" ) );
    ui->actionACmodification->setToolTip( tr( "Documentation how to add HW for AC coupled inputs" ) );
    ui->actionFrequencyGeneratorModification->setIcon( QIcon( iconPath + "book.svg" ) );
    ui->actionFrequencyGeneratorModification->setToolTip(
        tr( "Documentation how to get jitter-free calibration frequency output" ) );
    ui->actionAbout->setIcon( QIcon( iconPath + "about.svg" ) );
    ui->actionAbout->setToolTip( tr( "Show info about the scope's HW and SW" ) );
    ui->actionAboutQt->setIcon( QIcon( iconPath + "qt.svg" ) );
    ui->actionAboutQt->setToolTip( tr( "Show info about Qt" ) );

    // Window title
    setWindowIcon( QIcon( ":/images/ctpu.svg" ) );
    // Имя приложения в заголовке — CtPU/BinTape/TimeChannel, а не апстрим:
    // пользователь видит форк, а не OpenHantek6022 (замечание 2026-08-21).
    setWindowTitle( dsoControl->getDevice()->isRealHW()
                        ? tr( "CtPU·BinTape·TimeChannel (%1) — %2 (FW%3)" )
                              .arg( QString::fromStdString( VERSION ), dsoControl->getModel()->name )
                              .arg( dsoControl->getDevice()->getFwVersion(), 4, 16, QChar( '0' ) )
                        : tr( "CtPU·BinTape·TimeChannel (%1) — " ).arg( QString::fromStdString( VERSION ) ) +
                              tr( "Demo Mode" ) );

#if ( QT_VERSION >= QT_VERSION_CHECK( 5, 6, 0 ) )
    setDockOptions( dockOptions() | QMainWindow::GroupedDragging );
#endif
    QAction *action;
    action = new QAction( QIcon( iconPath + "camera.svg" ), tr( "&Screenshot" ), this );
    action->setToolTip( tr( "Make an immediate screenshot of the program window and save it into the current directory" ) );
    connect( action, &QAction::triggered, this, [ this ]() { screenShot( SCREENSHOT, true ); } );
    ui->menuExport->addAction( action );

    action = new QAction( QIcon( iconPath + "clone.svg" ), tr( "&Hardcopy" ), this );
    action->setToolTip( tr( "Make an immediate (printable) hardcopy of the display and save it into the current directory" ) );
    connect( action, &QAction::triggered, this, [ this ]() {
        dsoWidget->switchToPrintColors();
        QTimer::singleShot( 20, this, [ this ]() { screenShot( HARDCOPY, true ); } );
    } );
    ui->menuExport->addAction( action );

    ui->menuExport->addSeparator();

    action = new QAction( QIcon( iconPath + "save_as.svg" ), tr( "Save Screenshot As .." ), this );
    action->setToolTip( tr( "Make a screenshot of the program window and define the storage location" ) );
    connect( action, &QAction::triggered, this, [ this ]() { screenShot( SCREENSHOT ); } );
    ui->menuExport->addAction( action );

    action = new QAction( QIcon( iconPath + "save_as.svg" ), tr( "Save Hardcopy As .." ), this );
    action->setToolTip( tr( "Make a (printable) hardcopy of the display and define the storage location" ) );
    connect( action, &QAction::triggered, this, [ this ]() {
        dsoWidget->switchToPrintColors();
        QTimer::singleShot( 20, this, [ this ]() { screenShot( HARDCOPY ); } );
    } );
    ui->menuExport->addAction( action );

    action = new QAction( QIcon( iconPath + "print.svg" ), tr( "&Print Screen .." ), this );
    action->setToolTip( tr( "Send the hardcopy to a printer" ) );
    connect( action, &QAction::triggered, this, [ this ]() {
        dsoWidget->switchToPrintColors();
        QTimer::singleShot( 20, this, [ this ]() { screenShot( PRINTER ); } );
    } );
    ui->menuExport->addAction( action );

    ui->menuExport->addSeparator();

    // XY Recorder export - manual snapshot of what's currently in RAM.
    // In TAPE + streaming mode that's only the current render window (tail);
    // the full record is already at the file chosen when recording started.
    // [MOD] TZ §7.6 — exports one CSV per enabled curve (xy_curve<N>_*.csv).
    action = new QAction( QIcon( iconPath + "exporter.svg" ), tr( "Export XY Data .." ), this );
    action->setToolTip( tr( "Exports TWO files per enabled curve:\n"
                            "  xy_field_*  — the XY record proper: value bins on BOTH axes with independent\n"
                            "     resolution, cell = visit density. The waveform shape survives here.\n"
                            "  xy_tape_*   — the long-time average along the tape (mean per tape segment).\n"
                            "     Useful for slow signals, but the shape is NOT recoverable from it." ) );
    connect( action, &QAction::triggered, this, [ this ]() {
        DsoSettingsScope *scope = dsoWidget->settingsScope();
        if ( !scope )
            return;
        const QString baseDir = QFileDialog::getExistingDirectory( this, tr( "Export XY Data — choose output directory" ) );
        if ( baseDir.isEmpty() )
            return;
        const QString timestamp = QDateTime::currentDateTime().toString( QStringLiteral( "yyyyMMdd_HHmmss" ) );
        for ( int i = 0; i < DsoSettingsScope::maxXYCurves; ++i ) {
            if ( i >= int( scope->xyCurves.size() ) || !scope->xyCurves[ i ].enabled )
                continue;
            const XYRecorder *rec = dsoWidget->xyRecorder( i );
            if ( !rec || rec->empty() )
                continue;
            // Два файла с говорящими именами (замечание пользователя
            // 2026-08-21: «XY Recorder» экспортировал усреднение по времени
            // под видом XY-записи). Поле — настоящая XY-запись; лента —
            // усреднение, и так и называется.
            rec->exportFieldCSV( baseDir + QStringLiteral( "/xy_field_curve%1_%2.csv" ).arg( i ).arg( timestamp ) );
            rec->exportCSV( baseDir + QStringLiteral( "/xy_tape_average_curve%1_%2.csv" ).arg( i ).arg( timestamp ) );
        }
    } );
    ui->menuExport->addAction( action );

    ui->menuExport->addSeparator();

    for ( auto *exporter : *exporterRegistry ) {
        action = new QAction( QIcon( iconPath + "exporter.svg" ), exporter->name(), this );
        action->setToolTip( tr( "Export captured data in %1 format for further processing" ).arg( exporter->format() ) );
        action->setCheckable( exporter->type() == ExporterInterface::Type::ContinuousExport );
        connect( action, &QAction::triggered, exporterRegistry, [ exporter, exporterRegistry ]( bool checked ) {
            exporterRegistry->setExporterEnabled( exporter,
                                                  exporter->type() == ExporterInterface::Type::ContinuousExport ? checked : true );
        } );
        ui->menuExport->addAction( action );
    }

    if ( dsoSettings->scope.toolTipVisible ) {
        ui->menuFile->setToolTipsVisible( true );
        ui->menuExport->setToolTipsVisible( true );
        ui->menuView->setToolTipsVisible( true );
        ui->menuOscilloscope->setToolTipsVisible( true );
        ui->menuHelp->setToolTipsVisible( true );
    }

    // [MOD] TZ §11 — SCPI server stub menu entry. The actual TCP listener on
    // port 5025 is intentionally NOT implemented in this iteration (it is
    // declared technical debt by the project owner). The menu item only
    // shows an informational dialog so operators know the feature is planned.
    QAction *scpiStubAction = new QAction( tr( "SCPI Server (stub).." ), this );
    scpiStubAction->setToolTip( tr( "Placeholder for the SCPI TCP control interface (TZ §11). "
                                    "Not yet implemented — no listener is started." ) );
    connect( scpiStubAction, &QAction::triggered, this, [ this ]() {
        QMessageBox::information( this, tr( "SCPI Server" ),
                                  tr( "<b>SCPI server is not yet implemented.</b><br><br>"
                                      "Planned commands (TZ §11.2):<br>"
                                      "&nbsp;&nbsp;CHAN&lt;N&gt;:CTPU:MODE/K/B/UNIT/ZERO/SPAN<br>"
                                      "&nbsp;&nbsp;MATH&lt;N&gt;:SOURA/SOURB/OPER/INV/ENAB<br>"
                                      "&nbsp;&nbsp;XY:REC:START/STOP, XY:CURV&lt;N&gt;:XCH/YCH/ENAB<br><br>"
                                      "Target: TCP port 5025, queued-connection to DsoSettings." ) );
    } );
    ui->menuOscilloscope->addAction( scpiStubAction );

    // ------------------------------------------------------------------
    // Кнопочный периметр вместо строки меню (решение пользователя,
    // 2026-08-21): «все окна вызываются не из меню, а кнопками».
    // Строка меню скрывается, а каждое её меню становится кнопкой с
    // иконкой в левом вертикальном столбце — том самом, который
    // пользователю нравится. Меню при этом не удаляются: они живут как
    // всплывающие списки на кнопках, поэтому все прежние связи, горячие
    // клавиши и пункты продолжают работать без переписывания.
    // ------------------------------------------------------------------
    ui->menuFile->setIcon( QIcon( iconPath + "file.svg" ) );
    ui->menuExport->setIcon( QIcon( iconPath + "exporter.svg" ) );
    ui->menuView->setIcon( QIcon( iconPath + "view.svg" ) );
    ui->menuOscilloscope->setIcon( QIcon( iconPath + "scope.svg" ) );
    ui->menuHelp->setIcon( QIcon( iconPath + "help.svg" ) );
    // Иконки пунктов, у которых их не было: кнопка без знака бесполезна,
    // а всплывающий список читается быстрее со знаками.
    ui->actionOpen->setIcon( QIcon( iconPath + "open.svg" ) );
    ui->actionSave->setIcon( QIcon( iconPath + "save.svg" ) );
    ui->actionSave_as->setIcon( QIcon( iconPath + "save_as.svg" ) );
    ui->actionExit->setIcon( QIcon( iconPath + "exit.svg" ) );
    ui->actionSettings->setIcon( QIcon( iconPath + "settings.svg" ) );
    ui->actionCalibrateOffset->setIcon( QIcon( iconPath + "offset.svg" ) );
    ui->actionManualCommand->setIcon( QIcon( iconPath + "terminal.svg" ) );
    ui->actionDocking_windows->setIcon( QIcon( iconPath + "docks.svg" ) );
    ui->actionToolbars->setIcon( QIcon( iconPath + "toolbars.svg" ) );
    ui->actionUserManual->setIcon( QIcon( iconPath + "book.svg" ) );
    ui->actionAbout->setIcon( QIcon( iconPath + "about.svg" ) );
    ui->actionAboutQt->setIcon( QIcon( iconPath + "qt.svg" ) );
    ui->actionACmodification->setIcon( QIcon( iconPath + "mod_ac.svg" ) );
    ui->actionFrequencyGeneratorModification->setIcon( QIcon( iconPath + "mod_fg.svg" ) );
    scpiStubAction->setIcon( QIcon( iconPath + "terminal.svg" ) );

    menuBar()->setVisible( false ); // органы управления — только кнопки

    ui->toolBar->addSeparator();
    const QList< QMenu * > menuButtons = { ui->menuFile, ui->menuExport, ui->menuView,
                                           ui->menuOscilloscope, ui->menuHelp };
    for ( QMenu *m : menuButtons ) {
        ui->toolBar->addAction( m->menuAction() );
        // InstantPopup: список открывается сразу по нажатию, без стрелки-
        // задержки — кнопка ведёт себя как кнопка, а не как «меню в меню».
        if ( QToolButton *btn = qobject_cast< QToolButton * >( ui->toolBar->widgetForAction( m->menuAction() ) ) )
            btn->setPopupMode( QToolButton::InstantPopup );
    }
    // Окно связи (замечание пользователя 2026-08-21: Oscill подключён, а
    // кнопки и окна про связь в приложении нет). Отдельной кнопкой, потому
    // что «с каким прибором и по какому протоколу» — вопрос, который
    // оператор задаёт первым, а не ищет в подменю.
    QAction *connectionAction = new QAction( QIcon( iconPath + "link.svg" ), tr( "Connection .." ), this );
    connectionAction->setToolTip( tr( "Which instrument and which protocol is in use right now" ) );
    connect( connectionAction, &QAction::triggered, this, [ this, dsoControl ]() {
        ConnectionDialog( dsoControl, this ).exec();
    } );
    ui->menuOscilloscope->addAction( connectionAction );

    // Окно генератора DDS (распоряжение автора 2026-09-04: «в GUI нет кнопки
    // окна DDS, органов управления и окна DDS»). Отдельной кнопкой рядом со
    // связью: генератор — это то, чего у пути B нет вовсе, и его состояние
    // оператор должен видеть, а не выяснять.
    //
    // Задняя сторона пока не подключена (nullptr): путь B генератора не
    // имеет, а коннектор пути A в приложение ещё не сведён. Окно об этом
    // говорит словами. Порядок работы — `docs/GUI-FIRST.md`: панель строится
    // первой и служит спецификацией.
    QAction *ddsAction = new QAction( QIcon( iconPath + "mod_fg.svg" ), tr( "DDS generator .." ), this );
    ddsAction->setToolTip( tr( "Waveform, frequency, duty cycle and output of the built-in generator" ) );
    connect( ddsAction, &QAction::triggered, this, [ this ]() {
        // Задняя сторона поднимается лениво и живёт до конца работы окна:
        // подъём стоит секунду на перечисление USB, платить её при старте
        // программы незачем.
        if ( !ddsBackend )
            ddsBackend = std::unique_ptr< VdsoBackend >( new VdsoBackend );
        ddsBackend->ensureOpen();
        DdsDialog( ddsBackend.get(), this ).exec();
    } );
    ui->menuOscilloscope->addAction( ddsAction );

    // Камера: ТРИ кнопки (распоряжение автора 2026-09-02). Собственного окна с
    // изображением у камеры нет - кадр живёт слоем на холсте, под сеткой и
    // кривыми. Поэтому кнопки делают ровно три разные вещи: настроить, снять
    // камеру, снять холст целиком.
    QAction *cameraSetupAction = new QAction( QIcon( iconPath + "camera_setup.svg" ), tr( "Camera .." ), this );
    cameraSetupAction->setToolTip( tr( "Choose the camera, switch the layer on or off, "
                                       "and set whatever the standard driver allows" ) );
    connect( cameraSetupAction, &QAction::triggered, this, [ this ]() {
        if ( !cameraDialog ) {
            cameraDialog = new CameraDialog( &dsoSettings->view, dsoWidget->camera(), this );
            connect( cameraDialog, &CameraDialog::cameraChanged, this, [ this ]() { dsoWidget->applyCameraSettings(); } );
            // Выбор камеры записывается в момент нажатия «Применить», а не при
            // выходе: сохранение при выходе висит на alwaysSave, и при
            // выключенном флаге явно сделанный выбор пропадал бы молча.
            connect( cameraDialog, &CameraDialog::cameraChoiceApplied, this,
                     [ this ]() { dsoSettings->saveCameraChoice(); } );
        }
        // Состояние слоя холста спрашивается при каждом открытии: программа
        // шейдера строится при первом показе холста, то есть позже создания
        // окна камеры.
        cameraDialog->setCanvasDiagnostic( dsoWidget->cameraLayerError() );
        cameraDialog->show();
        cameraDialog->raise();
        cameraDialog->activateWindow();
    } );

    QAction *cameraPhotoAction = new QAction( QIcon( iconPath + "camera_photo.svg" ), tr( "Save Camera Photo" ), this );
    cameraPhotoAction->setToolTip( tr( "Save the current camera frame as it is: no grid, no curves" ) );
    connect( cameraPhotoAction, &QAction::triggered, this, [ this ]() { saveCameraImage( false ); } );

    QAction *cameraShotAction = new QAction( QIcon( iconPath + "camera_shot.svg" ), tr( "Save Canvas with Camera" ), this );
    cameraShotAction->setToolTip( tr( "Save the canvas as seen on screen: the camera frame under the grid and the curves" ) );
    connect( cameraShotAction, &QAction::triggered, this, [ this ]() { saveCameraImage( true ); } );

    ui->menuView->addAction( cameraSetupAction );
    ui->menuExport->addAction( cameraPhotoAction );
    ui->menuExport->addAction( cameraShotAction );

    ui->toolBar->addSeparator();
    ui->toolBar->addAction( cameraSetupAction );
    ui->toolBar->addAction( cameraPhotoAction );
    ui->toolBar->addAction( cameraShotAction );

    ui->toolBar->addSeparator();
    ui->toolBar->addAction( connectionAction );
    ui->toolBar->addAction( ui->actionSettings ); // самое частое — отдельной кнопкой
    ui->toolBar->addAction( ui->actionExit );

    DsoSettingsScope *scope = &( dsoSettings->scope );
    const Dso::ControlSpecification *spec = dsoControl->getModel()->spec();

    registerDockMetaTypes();

    // Docking windows
    // Create dock windows before the dso widget, they fix messed up settings

    VoltageDock *voltageDock = new VoltageDock( scope, spec, this );
    HorizontalDock *horizontalDock = new HorizontalDock( scope, spec, this );
    TriggerDock *triggerDock = new TriggerDock( scope, spec, this );
    SpectrumDock *spectrumDock = new SpectrumDock( scope, this );

    addDockWidget( Qt::RightDockWidgetArea, voltageDock );
    addDockWidget( Qt::RightDockWidgetArea, horizontalDock );
    addDockWidget( Qt::RightDockWidgetArea, triggerDock );
    addDockWidget( Qt::RightDockWidgetArea, spectrumDock );

    restoreGeometry( dsoSettings->mainWindowGeometry );
    restoreState( dsoSettings->mainWindowState );

    // Central oszilloscope widget
    dsoWidget = new DsoWidget( &dsoSettings->scope, &dsoSettings->view, spec, this );
    setCentralWidget( dsoWidget );

    if ( dsoControl->getDevice()->isRealHW() ) { // enable online calibration and manual command input
        // Command field inside the status bar
        commandEdit = new QLineEdit( this );
        // allowed commands are either control command "cc E0..E6 xx [xx ...]" or frequency command "freq nn"
        QRegularExpressionValidator *v =
            new QRegularExpressionValidator( QRegularExpression( "(cc|CC) [eE][0-6]( [0-9a-fA-F]{1,2})+|freq \\d{1,6}" ), this );
        commandEdit->setValidator( v );
        commandEdit->hide();
        statusBar()->addPermanentWidget( commandEdit, 1 );

        connect( ui->actionCalibrateOffset, &QAction::toggled, this, [ this, dsoControl, scope ]( bool active ) {
            if ( active )
                active = ( QMessageBox::Apply ==
                           QMessageBox::information( this, tr( "Calibrate Offset" ),
                                                     tr( "Short-circuit both inputs and slowly select all voltage gain settings" ),
                                                     QMessageBox::Apply | QMessageBox::Abort, QMessageBox::Abort ) );
            if ( verboseLevel > 2 )
                qDebug() << "  Calibrate offset" << active;
            ui->actionCalibrateOffset->setChecked( active );
            dsoControl->calibrateOffset( active );
            scope->liveCalibrationActive = active;
        } );

        // disable calibration e.g. if zero signal too noisy or offset too big
        connect( dsoControl, &HantekDsoControl::liveCalibrationError, this, [ this, scope ]() {
            if ( verboseLevel > 2 )
                qDebug() << "  Live calibration error";
            scope->liveCalibrationActive = false;           // set incactive first to avoid ..
            ui->actionCalibrateOffset->setChecked( false ); // .. calibration storage actions
        } );

        connect( ui->actionManualCommand, &QAction::toggled, this, [ this ]( bool checked ) {
            commandEdit->setVisible( checked );
            if ( checked )
                commandEdit->setFocus();
        } );

        connect( commandEdit, &QLineEdit::returnPressed, this, [ this, dsoControl ]() {
            Dso::ErrorCode errorCode = dsoControl->stringCommand( commandEdit->text() );
            commandEdit->clear();
            ui->actionManualCommand->setChecked( false );
            if ( errorCode != Dso::ErrorCode::NONE )
                statusBar()->showMessage( tr( "Invalid command" ), 3000 );
        } );

    } else { // do not show these actions
        ui->actionCalibrateOffset->setVisible( false );
        ui->actionManualCommand->setVisible( false );
    }

    // Connect signals that display text in statusbar
    connect( dsoControl, &HantekDsoControl::statusMessage, this, [ this ]( QString text, int timeout ) {
        ui->actionManualCommand->setChecked( false );
        statusBar()->showMessage( text, timeout );
    } );

    // Connect signals to DSO controller and widget
    connect( horizontalDock, &HorizontalDock::samplerateChanged, dsoControl,
             [ dsoControl, this ]() { dsoControl->setSamplerate( dsoSettings->scope.horizontal.samplerate ); } );
    connect( horizontalDock, &HorizontalDock::samplerateChanged, dsoWidget,
             [ this ]() { dsoWidget->updateSamplerate( dsoSettings->scope.horizontal.samplerate ); } );
    connect( horizontalDock, &HorizontalDock::timebaseChanged, triggerDock, &TriggerDock::timebaseChanged );
    connect( horizontalDock, &HorizontalDock::timebaseChanged, dsoControl,
             [ dsoControl, this ]() { dsoControl->setRecordTime( dsoSettings->scope.horizontal.timebase * DIVS_TIME ); } );
    connect( horizontalDock, &HorizontalDock::timebaseChanged, dsoWidget,
             [ this ]() { dsoWidget->updateTimebase( dsoSettings->scope.horizontal.timebase ); } );
    connect( spectrumDock, &SpectrumDock::frequencybaseChanged, dsoWidget,
             [ this ]( double frequencybase ) { dsoWidget->updateFrequencybase( frequencybase ); } );
    connect( dsoControl, &HantekDsoControl::samplerateCalculated, horizontalDock,
             [ this, horizontalDock, spectrumDock ]( double samplerate, unsigned oversample ) {
                 // The timebase was set, let's adapt the samplerate accordingly
                 dsoSettings->scope.horizontal.samplerate = samplerate;
                 horizontalDock->setSamplerate( samplerate );
                 spectrumDock->setSamplerate( samplerate ); // mind the Nyquest frequency
                 dsoWidget->updateSamplerate( samplerate );
                 dsoWidget->updateOversample( oversample );
             } );
    connect( horizontalDock, &HorizontalDock::calfreqChanged, dsoControl,
             [ dsoControl, this ]() { dsoControl->setCalFreq( dsoSettings->scope.horizontal.calfreq ); } );
    connect( horizontalDock, &HorizontalDock::formatChanged, spectrumDock, [ = ]( Dso::GraphFormat format ) {
        ui->actionHistogram->setEnabled( format == Dso::GraphFormat::TY );
        spectrumDock->enableSpectrumDock( format == Dso::GraphFormat::TY );
    } );
    connect( horizontalDock, &HorizontalDock::xyConfigureRequested, dsoWidget, &DsoWidget::configureXYRecorder );
    connect( horizontalDock, &HorizontalDock::xyContinuousChanged, dsoWidget, &DsoWidget::updateXYContinuous );

    connect( triggerDock, &TriggerDock::modeChanged, dsoControl, &HantekDsoControl::setTriggerMode );
    connect( triggerDock, &TriggerDock::modeChanged, dsoWidget, &DsoWidget::updateTriggerMode );
    connect( triggerDock, &TriggerDock::modeChanged, horizontalDock, &HorizontalDock::triggerModeChanged );
    connect( triggerDock, &TriggerDock::modeChanged, this, [ this ]( Dso::TriggerMode mode ) {
        ui->actionRefresh->setVisible( Dso::TriggerMode::ROLL == mode && dsoSettings->scope.horizontal.samplerate < 10e3 );
    } );
    connect( dsoControl, &HantekDsoControl::samplerateCalculated, this, [ this ]( double samplerate ) {
        ui->actionRefresh->setVisible( Dso::TriggerMode::ROLL == dsoSettings->scope.trigger.mode && samplerate < 10e3 );
    } );
    connect( triggerDock, &TriggerDock::sourceChanged, dsoControl, &HantekDsoControl::setTriggerSource );
    connect( triggerDock, &TriggerDock::sourceChanged, dsoWidget, &DsoWidget::updateTriggerSource );
    connect( triggerDock, &TriggerDock::smoothChanged, dsoControl, &HantekDsoControl::setTriggerSmooth );
    // should we send the smooth mode also to dsoWidget?
    connect( triggerDock, &TriggerDock::slopeChanged, dsoControl, &HantekDsoControl::setTriggerSlope );
    connect( triggerDock, &TriggerDock::slopeChanged, dsoWidget, &DsoWidget::updateTriggerSlope );
    connect( dsoWidget, &DsoWidget::triggerPositionChanged, dsoControl, &HantekDsoControl::setTriggerPosition );
    connect( dsoWidget, &DsoWidget::triggerLevelChanged, dsoControl, &HantekDsoControl::setTriggerLevel );

    auto usedChanged = [ this, dsoControl, spec ]( ChannelID channel, unsigned channelMask ) {
        if ( channel > spec->channels )
            return;
        if ( dsoSettings->scope.verboseLevel > 2 )
            qDebug().noquote() << "  MW::uC()" << channel << QString::number( channelMask, 2 );
        bool mathUsed = 3 == channelMask; // dsoSettings->scope.anyUsed( spec->channels );

        // Normal channel, check if voltage/spectrum or math channel is used
        if ( channel < spec->channels )
            dsoControl->setChannelUsed( channel, mathUsed || dsoSettings->scope.anyUsed( channel ) );
        // Math channel, update all channels
        else if ( channel == spec->channels ) {
            for ( ChannelID c = 0; c < spec->channels; ++c )
                dsoControl->setChannelUsed( c, ( ( c + 1 ) & channelMask ) || dsoSettings->scope.anyUsed( c ) );
        }
    };
    connect( voltageDock, &VoltageDock::usedChannelChanged, usedChanged );
    connect( spectrumDock, &SpectrumDock::usedChannelChanged, usedChanged );

    connect( voltageDock, &VoltageDock::modeChanged, dsoWidget, &DsoWidget::updateMathMode );
    connect( voltageDock, &VoltageDock::gainChanged, dsoControl, [ dsoControl, spec ]( ChannelID channel, double gain ) {
        if ( channel > spec->channels )
            return;
        dsoControl->setGain( channel, gain );
    } );
    connect( voltageDock, &VoltageDock::probeAttnChanged, dsoControl, [ dsoControl, spec ]( ChannelID channel, double probeAttn ) {
        if ( channel > spec->channels )
            return;
        dsoControl->setProbe( channel, probeAttn );
    } );
    connect( voltageDock, &VoltageDock::invertedChanged, dsoControl, [ dsoControl, spec ]( ChannelID channel, bool inverted ) {
        if ( channel > spec->channels )
            return;
        dsoControl->setChannelInverted( channel, inverted );
    } );
    connect( voltageDock, &VoltageDock::couplingChanged, dsoWidget, &DsoWidget::updateVoltageCoupling );
    connect( voltageDock, &VoltageDock::couplingChanged, dsoControl,
             [ dsoControl, spec ]( ChannelID channel, Dso::Coupling coupling ) {
                 if ( channel > spec->channels )
                     return;
                 dsoControl->setCoupling( channel, coupling );
             } );
    connect( voltageDock, &VoltageDock::gainChanged, dsoWidget, &DsoWidget::updateVoltageGain );
    connect( voltageDock, &VoltageDock::usedChannelChanged, dsoWidget, &DsoWidget::updateVoltageUsed );
    connect( spectrumDock, &SpectrumDock::usedChannelChanged, dsoWidget, &DsoWidget::updateSpectrumUsed );
    connect( spectrumDock, &SpectrumDock::magnitudeChanged, dsoWidget, &DsoWidget::updateSpectrumMagnitude );

    // Started/stopped signals from oscilloscope
    connect( dsoControl, &HantekDsoControl::showSamplingStatus, this, [ this ]( bool enabled ) {
        QSignalBlocker blocker( ui->actionSampling );
        if ( enabled ) {
            ui->actionSampling->setIcon( iconPause );
            ui->actionSampling->setText( tr( "Stop" ) );
            ui->actionSampling->setStatusTip( tr( "Stop the oscilloscope" ) );
        } else {
            ui->actionSampling->setIcon( iconPlay );
            ui->actionSampling->setText( tr( "Start" ) );
            ui->actionSampling->setStatusTip( tr( "Start the oscilloscope" ) );
        }
        ui->actionSampling->setChecked( enabled );
    } );
    connect( ui->actionSampling, &QAction::triggered, dsoControl, &HantekDsoControl::enableSamplingUI );
    ui->actionSampling->setChecked( dsoControl->isSamplingUI() );

    connect( ui->actionRefresh, &QAction::triggered, dsoControl, &HantekDsoControl::restartSampling );

    connect( dsoControl, &HantekDsoControl::samplerateLimitsChanged, horizontalDock, &HorizontalDock::setSamplerateLimits );
    connect( dsoControl, &HantekDsoControl::samplerateSet, horizontalDock, &HorizontalDock::setSamplerateSteps );

    // Load settings to GUI
    connect( this, &MainWindow::settingsLoaded, voltageDock, &VoltageDock::loadSettings );
    connect( this, &MainWindow::settingsLoaded, horizontalDock, &HorizontalDock::loadSettings );
    connect( this, &MainWindow::settingsLoaded, spectrumDock, &SpectrumDock::loadSettings );
    connect( this, &MainWindow::settingsLoaded, triggerDock, &TriggerDock::loadSettings );
    connect( this, &MainWindow::settingsLoaded, dsoControl, &HantekDsoControl::applySettings );
    connect( this, &MainWindow::settingsLoaded, dsoWidget, &DsoWidget::updateSlidersSettings );

    connect( ui->actionOpen, &QAction::triggered, this, [ this, spec ]() {
        QString configFileName = QFileDialog::getOpenFileName( this, tr( "Open file" ), "", tr( "Settings (*.conf)" ), nullptr,
                                                               QFileDialog::DontUseNativeDialog );
        if ( !configFileName.isEmpty() ) {
            dsoSettings->loadFromFile( configFileName );
            restoreGeometry( dsoSettings->mainWindowGeometry );
            restoreState( dsoSettings->mainWindowState );

            emit settingsLoaded( &dsoSettings->scope, spec );

            dsoWidget->updateTimebase( dsoSettings->scope.horizontal.timebase );

            for ( ChannelID channel = 0; channel < spec->channels; ++channel ) {
                dsoWidget->updateVoltageUsed( channel, dsoSettings->scope.voltage[ channel ].used );
                dsoWidget->updateSpectrumUsed( channel, dsoSettings->scope.spectrum[ channel ].used );
            }
        }
    } );

    connect( ui->actionSave, &QAction::triggered, this, [ this ]() {
        dsoSettings->mainWindowGeometry = saveGeometry();
        dsoSettings->mainWindowState = saveState();
        dsoSettings->save();
    } );

    connect( ui->actionSave_as, &QAction::triggered, this, [ this ]() {
        QString configFileName = QFileDialog::getSaveFileName( this, tr( "Save settings" ), "", tr( "Settings (*.conf)" ), nullptr,
                                                               QFileDialog::DontUseNativeDialog );
        if ( configFileName.isEmpty() )
            return;
        if ( !configFileName.endsWith( ".conf" ) )
            configFileName.append( ".conf" );
        dsoSettings->mainWindowGeometry = saveGeometry();
        dsoSettings->mainWindowState = saveState();
        dsoSettings->saveToFile( configFileName );
    } );

    connect( ui->actionExit, &QAction::triggered, this, &QWidget::close );

    connect( ui->actionSettings, &QAction::triggered, this, [ this, voltageDock ]() {
        dsoSettings->mainWindowGeometry = saveGeometry();
        dsoSettings->mainWindowState = saveState();

        DsoConfigDialog *configDialog = new DsoConfigDialog( dsoSettings, this );
        configDialog->setModal( true );
        openConfigDialog = configDialog; // QPointer: auto-nulls when the dialog is destroyed
        // Настройки, принятые в диалоге, должны действовать сразу. Галка
        // «аппаратная доработка AC» раньше требовала перезапуска только
        // потому, что список видов связи строился один раз в конструкторе
        // дока: пользователь ставил галку и AC не появлялась.
        connect( configDialog, &QDialog::accepted, voltageDock, &VoltageDock::updateCouplingStrings );
        configDialog->show();
    } );

    connect( ui->actionPhosphor, &QAction::toggled, this, [ this ]( bool enabled ) {
        dsoSettings->view.digitalPhosphor = enabled;

        if ( dsoSettings->view.digitalPhosphor )
            ui->actionPhosphor->setStatusTip( tr( "Disable fading of previous graphs" ) );
        else
            ui->actionPhosphor->setStatusTip( tr( "Enable fading of previous graphs" ) );
    } );
    ui->actionPhosphor->setChecked( dsoSettings->view.digitalPhosphor );

    connect( ui->actionHistogram, &QAction::toggled, this, [ this ]( bool enabled ) {
        dsoSettings->scope.histogram = enabled;

        if ( dsoSettings->scope.histogram )
            ui->actionHistogram->setStatusTip( tr( "Hide histogram" ) );
        else
            ui->actionHistogram->setStatusTip( tr( "Show histogram" ) );
    } );
    ui->actionHistogram->setChecked( dsoSettings->scope.histogram );
    ui->actionHistogram->setEnabled( scope->horizontal.format == Dso::GraphFormat::TY );

    connect( ui->actionZoom, &QAction::toggled, this, [ this ]( bool enabled ) {
        dsoSettings->view.zoom = enabled;

        if ( dsoSettings->view.zoom )
            ui->actionZoom->setStatusTip( tr( "Hide magnified scope" ) );
        else
            ui->actionZoom->setStatusTip( tr( "Show magnified scope" ) );

        dsoWidget->updateZoom( enabled );
    } );
    ui->actionZoom->setChecked( dsoSettings->view.zoom );

    connect( ui->actionMeasure, &QAction::toggled, this, [ this ]( bool enabled ) {
        dsoSettings->view.cursorsVisible = enabled;

        if ( dsoSettings->view.cursorsVisible )
            ui->actionMeasure->setStatusTip( tr( "Hide measurements" ) );
        else
            ui->actionMeasure->setStatusTip( tr( "Show measurements" ) );

        dsoWidget->updateCursorGrid( enabled );
    } );
    ui->actionMeasure->setChecked( dsoSettings->view.cursorsVisible );

    connect( ui->actionUserManual, &QAction::triggered, this, [ this ]() { openDocument( UserManualName ); } );

    connect( ui->actionACmodification, &QAction::triggered, this, [ this ]() { openDocument( ACModificationName ); } );

    connect( ui->actionFrequencyGeneratorModification, &QAction::triggered, this,
             [ this ]() { openDocument( FrequencyGeneratorModificationName ); } );

    connect( ui->actionAbout, &QAction::triggered, this, [ this ]() {
        QString deviceSpec = dsoSettings->deviceName == "DEMO"
                                 ? tr( "<p>Demo Mode</p>" )
                                 : tr( "<p>Device: %1 (%2), FW%3</p>" )
                                       .arg( dsoSettings->deviceName, dsoSettings->deviceID ) // device type, ser. num
                                       .arg( dsoSettings->deviceFW, 4, 16,
                                             QChar( '0' ) ); // FW version
        QMessageBox::about(
            this, QString( "%1 (%2)" ).arg( QCoreApplication::applicationName(), VERSION ),
            QString( tr( "<h2 style='margin-bottom:2px'>CtPU</h2>"
                         "<p style='margin-top:0'><b>Code to Physical Unit</b> &middot; BinTape &middot; TimeChannel<br/>"
                         "<i>Измерительное ПО для InstruStar ISDS205B</i></p>"
                         "<p>Автор проекта, архитектуры и знака: <b>dtba3a-del</b><br/>"
                         "CtPU (перевод кода АЦП в физическую величину), MathStack, "
                         "мультикривые XY, BinTape (лента бинов), TimeChannel (время как канал) "
                         "&mdash; &copy; 2026-%1 dtba3a-del<br/>"
                         "<a href='https://github.com/dtba3a-del/-Ctpu-bintape-timechannel-'>"
                         "https://github.com/dtba3a-del/-Ctpu-bintape-timechannel-</a></p>"
                         "<hr/>"
                         "<p style='font-size:small'>Основой послужил проект OpenHantek6022 &mdash; "
                         "свободное ПО для USB-осциллографов Hantek6022. Его авторство ниже "
                         "сохранено полностью: это требование GPL и признание чужого труда.</p>"
                         "<p>OpenHantek6022 maintainer: Martin Homuth-Rosemann</p>"
                         "<p>Copyright &copy; 2010, 2011 Oliver Haag</p>"
                         "<p>Copyright &copy; 2012-%1 OpenHantek community<br/>"
                         "<a href='https://github.com/OpenHantek'>https://github.com/OpenHantek</a></p>"
                         "<p>Open source firmware copyright &copy; 2019-%1 Ho-Ro<br/>"
                         "<a href='https://github.com/Ho-Ro/Hantek6022API'>https://github.com/Ho-Ro/Hantek6022API</a></p>"
                         "<p>ISDS205B firmware derived from fx2adc by Steve Markgraf<br/>"
                         "<a href='https://github.com/steve-m/fx2adc'>https://github.com/steve-m/fx2adc</a></p>"
                         "<p>Licensed under the GNU General Public License.</p>" ) )
                    .arg( QDate::currentDate().year() ) // latest year

                + deviceSpec // "DEVICE (SERIALNUMBER) FW_VERSION" or "Demo Mode"

                + tr( "<p>Graphic: %1 - GLSL version %2</p>"
                      "<p>Qt version: %3</p>" )
                      .arg( GlScope::getOpenGLversion(), GlScope::getGLSLversion(), // graphic info
                            QT_VERSION_STR ) +                                      // Qt version info
                tr( "<p>Running since %1 seconds.</p>" ).arg( elapsedTime.elapsed() / 1000 ) );
    } );

    connect( ui->actionAboutQt, &QAction::triggered, this, [ this ]() {
        QMessageBox::aboutQt( this, QString( "%1 (%2)" ).arg( QCoreApplication::applicationName(), VERSION ) );
    } );

    emit settingsLoaded( &dsoSettings->scope, spec ); // trigger the previously connected docks, widgets, etc.

    dsoWidget->updateTimebase( dsoSettings->scope.horizontal.timebase );

    for ( ChannelID channel = 0; channel < spec->channels; ++channel ) {
        dsoWidget->updateVoltageUsed( channel, dsoSettings->scope.voltage[ channel ].used );
        dsoWidget->updateSpectrumUsed( channel, dsoSettings->scope.spectrum[ channel ].used );
    }
}


MainWindow::~MainWindow() {
    if ( dsoSettings->scope.verboseLevel > 1 )
        qDebug() << " MainWindow::~MainWindow()";
    delete ui;
}


void MainWindow::showNewData( std::shared_ptr< PPresult > newData ) {
    if ( !newData )
        return;
    if ( dsoSettings->scope.verboseLevel > 5 )
        qDebug() << "     MainWindow::showNewData()" << newData->tag;
    // Data first, every frame: recorders (XY/BinTape) may never lose a frame.
    dsoWidget->ingestNew( newData );
    // Display second, mailbox-1: replace the unrendered frame instead of
    // queueing behind it (D-05). If the slot was empty, schedule ONE render
    // behind whatever is already in the GUI queue; further frames arriving
    // before it runs only refresh the slot. Display latency is thus bounded
    // by a single queue drain, never by backlog depth.
    const bool renderAlreadyScheduled = bool( pendingRenderData );
    pendingRenderData = std::move( newData );
    if ( !renderAlreadyScheduled )
        QMetaObject::invokeMethod( this, &MainWindow::renderPendingData, Qt::QueuedConnection );
}


void MainWindow::renderPendingData() {
    std::shared_ptr< PPresult > newData = std::move( pendingRenderData );
    pendingRenderData.reset(); // moved-from is already empty; be explicit
    if ( !newData )
        return;
    if ( dsoSettings->scope.verboseLevel > 5 )
        qDebug() << "     MainWindow::renderPendingData()" << newData->tag;
    dsoWidget->showNew( newData );
    if ( openConfigDialog )
        openConfigDialog->updateLiveData( newData );
}


void MainWindow::exporterStatusChanged( const QString &exporterName, const QString &status ) {
    if ( dsoSettings->scope.verboseLevel > 3 )
        qDebug() << "   MainWindow::exporterStatusChanged()" << exporterName << status;
    ui->statusbar->showMessage( tr( "%1: %2" ).arg( exporterName, status ) );
}


void MainWindow::exporterProgressChanged() {
    if ( dsoSettings->scope.verboseLevel > 3 )
        qDebug() << "   MainWindow::exporterProgressChanged()";
    exporterRegistry->checkForWaitingExporters();
}


// make screenshot (type == SCREENSHOT) from the complete program window with screen colors ...
// ... or a printable hardcopy (type == HARDCOPY) from the scope widget only ...
// ... with printer colors and scaled to double height for zoomed screens ...
// ... or send this hardcopy directly to the printer (type == PRINTER).
// autoSafe == true -> do not ask for filename, save as PNG with default name into active directory
/// Сохранить кадр камеры или холст целиком.
///
/// Распоряжение автора 2026-09-02: два варианта сохранения - чистое фото и та
/// область холста, «где под сеткой находится слой изображения камеры».
///
/// Пустого файла не пишем: если кадров не было, честно говорим об этом. Файл с
/// чёрным прямоугольником оператор принял бы за снимок.
void MainWindow::saveCameraImage( bool wholeCanvas ) {
    const QImage image = wholeCanvas ? dsoWidget->canvasWithCamera() : dsoWidget->cameraPhoto();
    if ( image.isNull() ) {
        QMessageBox::information( this, QCoreApplication::applicationName(),
                                  wholeCanvas ? tr( "The canvas gave no image." )
                                              : tr( "No camera frame: the camera is not selected, not switched on, "
                                                    "or has not delivered a frame yet." ) );
        return;
    }
    const QString stamp = QDateTime::currentDateTime().toString( QStringLiteral( "yyyyMMdd_hhmmss" ) );
    // Имя говорит, что внутри: camera - только камера, canvas - холст со всеми
    // слоями. Два снимка одного момента не должны различаться только временем.
    const QString name = stamp + ( wholeCanvas ? QStringLiteral( "_canvas.png" ) : QStringLiteral( "_camera.png" ) );
    const QString path = QDir( lastSaveAsDir ).absoluteFilePath( name );
    if ( image.save( path ) )
        statusBar()->showMessage( path, 5000 );
    else
        QMessageBox::critical( this, QCoreApplication::applicationName(),
                               tr( "Write error\n%1" ).arg( QFileInfo{ path }.absoluteFilePath() ) );
}


void MainWindow::screenShot( screenshotType_t screenshotType, bool autoSafe ) {
    if ( dsoSettings->scope.verboseLevel > 2 )
        qDebug() << "  MainWindow::screenShot()" << screenshotType << autoSafe;
    auto activeWindow = screenshotType == SCREENSHOT ? qApp->activeWindow() : dsoWidget;
    QPixmap screenshot( activeWindow->size() ); // prepare a pixmap with the correct size
    int sw = screenshot.width();
    int sh = screenshot.height();
    if ( dsoSettings->scope.verboseLevel > 3 )
        qDebug() << "   screenshot size:" << sw << "x" << sh;

    int exportScale = dsoSettings->view.exportScaleValue;
    if ( exportScale > 1 ) { // upscale - e.g. for HiDPI downscaled screen
        screenshot.setDevicePixelRatio( exportScale );
        screenshot = screenshot.scaled( sw *= exportScale, sh *= exportScale );
        if ( dsoSettings->scope.verboseLevel > 3 )
            qDebug() << "   screenshot size scaled:" << sw << "x" << sh;
    }

    QDateTime now = QDateTime::currentDateTime();
    QString docName = now.toString( tr( "yyyy-MM-dd hh:mm:ss" ) );
    QString fileName = now.toString( tr( "yyyyMMdd_hhmmss" ) );
    fileName = QDir( lastSaveAsDir ).absoluteFilePath( fileName );
    statusBar()->showMessage( docName ); // show date in bottom line

    if ( screenshotType != SCREENSHOT && dsoSettings->view.zoom && dsoSettings->view.zoomImage &&
         dsoSettings->view.zoomHeightIndex == 0 ) {
        screenshot = screenshot.scaled( sw, sh *= 2 ); // make double height for 1:1 zoomed screen
    }

    activeWindow->render( &screenshot ); // take the screenshot
    statusBar()->clearMessage();         // remove bottom line
    dsoWidget->restoreScreenColors();    // switch back to normal colors (for HARDCOPY/PRINTER)

    // here we have a screeshot, now handle the different destinations.
    QPrinter printer( QPrinter::HighResolution );
#if ( QT_VERSION >= QT_VERSION_CHECK( 5, 10, 0 ) )
    printer.setPdfVersion( QPrinter::PdfVersion_A1b );
#endif
#if ( QT_VERSION >= QT_VERSION_CHECK( 5, 15, 0 ) )
    printer.setPageSize( QPageSize( QPageSize::A4 ) );
#else
    printer.setPageSize( QPrinter::A4 );
#endif
    printer.setPageMargins( QMarginsF( 20, 20, 20, 20 ), QPageLayout::Millimeter );
    printer.setPageOrientation( sw > sh ? QPageLayout::Landscape : QPageLayout::Portrait );
    printer.setCreator( QCoreApplication::applicationName() );
    printer.setDocName( docName );

    if ( screenshotType != PRINTER ) { // save under filename
        QStringList filters;
        fileName += ".png";
        if ( autoSafe ) { // save under default name as PNG without asking
            if ( !screenshot.save( fileName ) )
                QMessageBox::critical( this, QCoreApplication::applicationName(),
                                       tr( "Write error\n%1" ).arg( QFileInfo{ fileName }.absoluteFilePath() ) );
            return;
        }
        filters << tr( "Image (*.png *.jpg)" ) << tr( "Portable Document Format (*.pdf)" );
        QFileDialog fileDialog( this, tr( "Save Screenshot" ), fileName, filters.join( ";;" ) );
        fileDialog.setOption( QFileDialog::DontUseNativeDialog );
        fileDialog.setAcceptMode( QFileDialog::AcceptSave );
        if ( fileDialog.exec() != QDialog::Accepted )
            return;
        lastSaveAsDir = fileDialog.directory().absolutePath();
        fileName = fileDialog.selectedFiles().first();
        if ( filters.indexOf( fileDialog.selectedNameFilter() ) == 0 ) { // save as image
            if ( !screenshot.save( fileName ) )
                QMessageBox::critical( this, QCoreApplication::applicationName(),
                                       tr( "Write error\n%1" ).arg( QFileInfo{ fileName }.absoluteFilePath() ) );
            return;
        }

        // else create a *.pdf with a scaled and centered image
        printer.setOutputFormat( QPrinter::PdfFormat );
        printer.setOutputFileName( fileName );
        // supports screen resolution up to about 9600 x 9600 pixel
        int resolution = 75;
        printer.setResolution( resolution );
        int pw = printer.pageLayout().paintRectPixels( resolution ).width();
        int ph = printer.pageLayout().paintRectPixels( resolution ).height();
        int scale = qMin( pw / sw, ph / sh );
        while ( scale < 2 && resolution < 1200 ) {
            resolution *= 2;
            printer.setResolution( resolution );
            pw = printer.pageLayout().paintRectPixels( resolution ).width();
            ph = printer.pageLayout().paintRectPixels( resolution ).height();
            scale = qMin( pw / sw, ph / sh );
        }
    } else { // Show the printing dialog
        printer.setDocName( fileName + ".pdf" );
        QPrintDialog dialog( &printer );
        dialog.setWindowTitle( tr( "Print oscillograph" ) );
        if ( dialog.exec() != QDialog::Accepted ) {
            return;
        }
    }
    // send the pixmap to *.pdf or printer
    int pw = printer.pageLayout().paintRectPixels( printer.resolution() ).width();
    int ph = printer.pageLayout().paintRectPixels( printer.resolution() ).height();

    int scale = qMin( pw / sw, ph / sh );

    // qDebug() << sw << sh << pw << ph << scale << resolution;
    if ( scale < 1 )
        qDebug() << "Screenshot size too big, page will be cropped";
    else if ( scale > 1 ) // upscale accordingly
        screenshot = screenshot.scaled( sw *= scale, sh *= scale );
    printer.newPage();
    QPainter p( &printer );
    p.drawPixmap( ( pw - sw ) / 2, ( ph - sh ) / 2, screenshot ); // center the picture
    p.end();
}


bool MainWindow::openDocument( QString docName ) {
    QUrl url;
    if ( QFile( DocPath + docName ).exists() )
        url = QUrl::fromLocalFile( QFileInfo( DocPath + docName ).absoluteFilePath() );
    else
        url = QUrl( DocUrl + docName );
    if ( verboseLevel > 2 )
        qDebug() << "  " << url;
    return QDesktopServices::openUrl( url );
}


/// \brief Save the settings before exiting.
/// \param event The close event that should be handled.
void MainWindow::closeEvent( QCloseEvent *event ) {
    if ( dsoSettings->scope.verboseLevel > 2 )
        qDebug() << "  MainWindow::closeEvent()";
    if ( dsoSettings->alwaysSave ) {
        dsoSettings->mainWindowGeometry = saveGeometry();
        dsoSettings->mainWindowState = saveState();
        dsoSettings->save();
    }
    QMainWindow::closeEvent( event );
}
