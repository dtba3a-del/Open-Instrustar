// SPDX-License-Identifier: GPL-3.0-or-later

#include "cameradialog.h"

#include "cameralayer.h"
#include "viewsettings.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>


CameraDialog::CameraDialog( DsoSettingsView *view, CameraLayer *layer, QWidget *parent )
    : QDialog( parent ), view( view ), layer( layer ) {
    setWindowTitle( tr( "Camera" ) );
    setModal( false ); // окно настройки не должно останавливать прибор

    QVBoxLayout *root = new QVBoxLayout( this );

    // --- 1. Какая камера ------------------------------------------------
    QGroupBox *deviceGroup = new QGroupBox( tr( "Camera" ), this );
    QFormLayout *deviceForm = new QFormLayout( deviceGroup );

    deviceBox = new QComboBox( deviceGroup );
    QPushButton *rescan = new QPushButton( tr( "Rescan" ), deviceGroup );
    rescan->setToolTip( tr( "Ask the system for the list of cameras again" ) );

    QHBoxLayout *deviceRow = new QHBoxLayout;
    deviceRow->addWidget( deviceBox, 1 );
    deviceRow->addWidget( rescan, 0 );
    deviceForm->addRow( tr( "Device" ), deviceRow );

    enabledBox = new QCheckBox( tr( "Show the camera layer" ), deviceGroup );
    deviceForm->addRow( QString(), enabledBox );

    statusLabel = new QLabel( deviceGroup );
    statusLabel->setWordWrap( true );
    deviceForm->addRow( tr( "State" ), statusLabel );

    root->addWidget( deviceGroup );

    // --- 2. Слой на холсте: это НАШ регулятор, не драйверный -------------
    QGroupBox *layerGroup = new QGroupBox( tr( "Layer on the canvas" ), this );
    QFormLayout *layerForm = new QFormLayout( layerGroup );

    opacitySlider = new QSlider( Qt::Horizontal, layerGroup );
    opacitySlider->setRange( 0, 100 );
    opacitySlider->setValue( int( view->cameraLayerOpacity * 100.0 + 0.5 ) );
    opacityLabel = new QLabel( layerGroup );
    QHBoxLayout *opacityRow = new QHBoxLayout;
    opacityRow->addWidget( opacitySlider, 1 );
    opacityRow->addWidget( opacityLabel, 0 );
    layerForm->addRow( tr( "Opacity" ), opacityRow );

    // Ориентация. «Правильной» ориентации у камеры над платой нет по
    // построению: как её поставили, так она и снимает. Знает об этом только
    // оператор, поэтому здесь регулятор, а не константа в коде.
    rotationBox = new QComboBox( layerGroup );
    rotationBox->addItem( tr( "0°" ), 0 );
    rotationBox->addItem( tr( "90° clockwise" ), 90 );
    rotationBox->addItem( tr( "180°" ), 180 );
    rotationBox->addItem( tr( "270° clockwise" ), 270 );
    {
        const int index = rotationBox->findData( CameraLayer::normalizedRotation( view->cameraRotation ) );
        rotationBox->setCurrentIndex( index < 0 ? 0 : index );
    }
    layerForm->addRow( tr( "Rotation" ), rotationBox );

    mirrorHBox = new QCheckBox( tr( "Mirror left to right" ), layerGroup );
    mirrorHBox->setChecked( view->cameraMirrorH );
    layerForm->addRow( QString(), mirrorHBox );

    mirrorVBox = new QCheckBox( tr( "Mirror top to bottom" ), layerGroup );
    mirrorVBox->setChecked( view->cameraMirrorV );
    layerForm->addRow( QString(), mirrorVBox );

    QLabel *orientNote = new QLabel( tr( "<i>Mirrors are applied first, then the rotation. The same orientation goes to "
                                         "the canvas and to the saved camera photo: two shots of one moment must not "
                                         "differ by a turn.</i>" ),
                                     layerGroup );
    orientNote->setWordWrap( true );
    layerForm->addRow( orientNote );

    QLabel *orderNote = new QLabel( tr( "Layer order on the canvas: background, then the camera frame, then the grid "
                                        "and the curves. The camera has no window of its own — it is seen through "
                                        "everything drawn above it." ),
                                    layerGroup );
    orderNote->setWordWrap( true );
    layerForm->addRow( orderNote );

    root->addWidget( layerGroup );

    // --- 3. Что даёт стандартный драйвер --------------------------------
    controlsGroup = new QGroupBox( tr( "Standard driver controls" ), this );
    controlsLayout = new QVBoxLayout( controlsGroup );
    root->addWidget( controlsGroup );

    // --- 4. Почему выбор явный ------------------------------------------
    QLabel *note = new QLabel(
        tr( "<i>The choice is explicit on purpose. A laptop normally carries a second camera that is not meant for "
            "taking pictures — the infrared sensor of face recognition. It is registered under "
            "<tt>KSCATEGORY_SENSOR_CAMERA</tt>, and by the driver documentation &laquo;IR streams will show up as "
            "regular capture streams in DShow&raquo;, which is exactly the enumeration Qt uses on Windows. So such a "
            "camera can stand in this list next to the ordinary one and give an almost black frame. The application "
            "never picks a camera for you; see <tt>docs/CAMERA-LAYER.md</tt> §2.</i>" ),
        this );
    note->setWordWrap( true );
    root->addWidget( note );

    QDialogButtonBox *buttons = new QDialogButtonBox( QDialogButtonBox::Close, this );
    // «Применить» отдельной кнопкой (требование автора 2026-09-02): выбор
    // прибора вступает в силу нажатием, а не закрытием окна. Закрытие ничего
    // не применяет - иначе оператор не знает, что именно он подтвердил.
    applyButton = buttons->addButton( tr( "Apply" ), QDialogButtonBox::ApplyRole );
    applyButton->setToolTip( tr( "Open the chosen camera and remember the choice" ) );
    connect( applyButton, &QPushButton::clicked, this, [ this ]() { applySelection(); } );
    connect( buttons, &QDialogButtonBox::rejected, this, &QDialog::close );
    root->addWidget( buttons );

    // --- Связи ----------------------------------------------------------
    opacitySaveTimer = new QTimer( this );
    opacitySaveTimer->setSingleShot( true );
    opacitySaveTimer->setInterval( 400 );
    connect( opacitySaveTimer, &QTimer::timeout, this, [ this ]() { emit cameraChoiceApplied(); } );
    connect( rescan, &QPushButton::clicked, this, [ this ]() { reloadDeviceList(); } );
    connect( deviceBox, QOverload< int >::of( &QComboBox::currentIndexChanged ), this, [ this ]( int ) { markPending(); } );
    connect( enabledBox, &QCheckBox::toggled, this, [ this ]( bool ) { markPending(); } );
    // Окно узнаёт о состоянии от слоя, а не опрашивает его по таймеру: опрос
    // добавил бы второй источник того же знания. Регуляторы драйвера
    // перестраиваются здесь же - параметры становятся известны не в момент
    // открытия камеры, а когда построен граф, и ждать второго открытия окна
    // оператор не обязан.
    connect( layer, &CameraLayer::stateChanged, this, [ this ]() {
        rebuildControls();
        updateStatus();
    } );
    // Прозрачность - НАШ параметр, он действует сразу: смотреть на слой и
    // подбирать прозрачность вслепую, через «Применить», было бы мучением.
    connect( opacitySlider, &QSlider::valueChanged, this, [ this ]( int v ) {
        this->view->cameraLayerOpacity = double( v ) / 100.0;
        opacityLabel->setText( QStringLiteral( "%1 %" ).arg( v ) );
        emit cameraChanged();
        opacitySaveTimer->start(); // запись одна на жест, а не на каждый шаг
    } );

    // Ориентация действует сразу и сразу же запоминается: жеста, растянутого
    // во времени, у неё нет — щелчок один.
    connect( rotationBox, QOverload< int >::of( &QComboBox::currentIndexChanged ), this,
             [ this ]( int ) { applyOrientation(); } );
    connect( mirrorHBox, &QCheckBox::toggled, this, [ this ]( bool ) { applyOrientation(); } );
    connect( mirrorVBox, &QCheckBox::toggled, this, [ this ]( bool ) { applyOrientation(); } );

    opacityLabel->setText( QStringLiteral( "%1 %" ).arg( opacitySlider->value() ) );
    reloadDeviceList();
    {
        const QSignalBlocker block( enabledBox );
        enabledBox->setChecked( view->cameraLayerEnabled );
    }
    pending = false;
    applyButton->setEnabled( false );
    rebuildControls();
    updateStatus();
}


/// Ориентация из окна — в настройки, а оттуда в слой.
///
/// Отдельным методом, потому что источник у ориентации один — настройки вида,
/// — и записывать её из трёх обработчиков по отдельности значило бы завести
/// три хозяина одной величине.
void CameraDialog::applyOrientation() {
    view->cameraRotation = rotationBox->currentData().toInt();
    view->cameraMirrorH = mirrorHBox->isChecked();
    view->cameraMirrorV = mirrorVBox->isChecked();
    emit cameraChanged();       // слой получит ориентацию и перерисуется
    emit cameraChoiceApplied(); // и запись на диск, без ожидания «Применить»
}


/// Изменение помечено, но НЕ применено.
void CameraDialog::markPending() {
    pending = true;
    applyButton->setEnabled( true );
    updateStatus();
}


/// Перечитать список камер и восстановить в нём запомненный выбор.
///
/// Если запомненной камеры в системе нет, выбор падает на «не выбрана», а не
/// на соседнюю: подставленная камера выглядела бы как назначенная.
void CameraDialog::reloadDeviceList() {
    const QSignalBlocker block( deviceBox );
    const QString wanted = view->cameraDeviceId;
    deviceBox->clear();
    deviceBox->addItem( tr( "— not selected —" ), QString() );
    for ( const CameraLayer::Device &d : CameraLayer::availableDevices() )
        deviceBox->addItem( d.description.isEmpty() ? d.id : d.description, d.id );

    int index = 0;
    if ( !wanted.isEmpty() ) {
        const int found = deviceBox->findData( wanted );
        if ( found > 0 )
            index = found;
    }
    deviceBox->setCurrentIndex( index );
    updateStatus();
}


/// Применить выбор: запомнить его в настройках и открыть/закрыть камеру.
void CameraDialog::applySelection() {
    view->cameraDeviceId = deviceBox->currentData().toString();
    view->cameraLayerEnabled = enabledBox->isChecked();

    layer->stop();
    if ( view->cameraLayerEnabled && !view->cameraDeviceId.isEmpty() )
        layer->start( view->cameraDeviceId );

    pending = false;
    applyButton->setEnabled( false );
    rebuildControls();
    updateStatus();
    emit cameraChanged();
    emit cameraChoiceApplied(); // выбор записывается сразу, а не при выходе
}


/// Собрать группу регуляторов по тому, что объявил драйвер.
///
/// Пустая группа не остаётся пустой: в ней стоит строка о том, что драйвер не
/// даёт ни одного регулятора. Пустой прямоугольник оператор читает как
/// недоделку программы, а это факт о драйвере.
void CameraDialog::rebuildControls() {
    while ( QLayoutItem *item = controlsLayout->takeAt( 0 ) ) {
        if ( QWidget *w = item->widget() )
            w->deleteLater();
        delete item;
    }
    autoWhiteBalanceBox = nullptr;

    if ( !layer->isActive() ) {
        controlsLayout->addWidget( new QLabel( tr( "The camera is closed — there is nothing to ask the driver about." ),
                                               controlsGroup ) );
        return;
    }

    const QList< CameraLayer::Control > controls = layer->supportedControls();
    if ( controls.isEmpty() && !layer->hasAutoWhiteBalance() ) {
        QLabel *none = new QLabel( tr( "The standard driver of this camera offers no controls." ), controlsGroup );
        none->setWordWrap( true );
        controlsLayout->addWidget( none );
        return;
    }

    QWidget *form = new QWidget( controlsGroup );
    QFormLayout *formLayout = new QFormLayout( form );
    for ( const CameraLayer::Control &c : controls ) {
        QSlider *slider = new QSlider( Qt::Horizontal, form );
        // Ползунок целочисленный, величина вещественная: масштабируем на 1000
        // шагов и обратно — шаг мельче того, что различает драйвер.
        const int steps = 1000;
        slider->setRange( 0, steps );
        const double span = c.maximum - c.minimum;
        slider->setValue( span > 0 ? int( ( c.value - c.minimum ) / span * steps + 0.5 ) : 0 );

        QLabel *value = new QLabel( form );
        const QString key = c.key;
        const double minimum = c.minimum;
        const QString unit = c.unit;
        auto show = [ value, minimum, span, steps, unit ]( int raw ) {
            const double v = minimum + span * double( raw ) / steps;
            // Правило `docs/СЛЫШИМОСТЬ.md`: число без единицы не показывается.
            value->setText( QStringLiteral( "%1 %2" ).arg( v, 0, 'f', unit == QStringLiteral( "K" ) ? 0 : 2 ).arg( unit ) );
        };
        show( slider->value() );
        connect( slider, &QSlider::valueChanged, this, [ this, key, minimum, span, steps, show ]( int raw ) {
            show( raw );
            layer->setControl( key, minimum + span * double( raw ) / steps );
        } );

        QHBoxLayout *row = new QHBoxLayout;
        row->addWidget( slider, 1 );
        row->addWidget( value, 0 );
        formLayout->addRow( c.label, row );
    }

    if ( layer->hasAutoWhiteBalance() ) {
        autoWhiteBalanceBox = new QCheckBox( tr( "Automatic white balance" ), form );
        autoWhiteBalanceBox->setChecked( layer->autoWhiteBalance() );
        connect( autoWhiteBalanceBox, &QCheckBox::toggled, this,
                 [ this ]( bool on ) { layer->setAutoWhiteBalance( on ); } );
        formLayout->addRow( QString(), autoWhiteBalanceBox );
    }

    controlsLayout->addWidget( form );

    QLabel *where = new QLabel( tr( "<i>These values live in the camera driver, not in the scope settings: the driver "
                                    "keeps them for the device itself.</i>" ),
                                controlsGroup );
    where->setWordWrap( true );
    controlsLayout->addWidget( where );
}


/// Сообщение о непостроенном слое холста; пустое — слой построен.
void CameraDialog::setCanvasDiagnostic( const QString &text ) {
    canvasDiagnostic = text.trimmed();
    updateStatus();
}


/// Показать состояние словами, включая причину отказа.
void CameraDialog::updateStatus() {
    // Отказ слоя холста старше всех прочих состояний: при нём изображения не
    // будет ни с какой камерой и ни при каких настройках, и говорить в этом
    // случае «открыта, кадры идут» значило бы солгать оператору.
    if ( !canvasDiagnostic.isEmpty() ) {
        statusLabel->setText( tr( "THE CANVAS LAYER IS NOT BUILT — the frame cannot appear under the grid.\n%1" )
                                  .arg( canvasDiagnostic ) );
        return;
    }
    if ( pending ) {
        statusLabel->setText( tr( "changed — press Apply; nothing has been applied yet" ) );
        return;
    }
    if ( CameraLayer::availableDevices().isEmpty() ) {
        statusLabel->setText( tr( "the system reports no cameras" ) );
        return;
    }
    if ( deviceBox->currentData().toString().isEmpty() ) {
        statusLabel->setText( tr( "no camera selected — the layer stays off" ) );
        return;
    }
    if ( layer->isActive() ) {
        // «Открыта» и «кадры идут» — разные вещи, и подменять одно другим
        // нельзя: ровно эта подмена скрывала отсутствие изображения. Счёт
        // кадров говорит о втором.
        QString text = tr( "open: %1" ).arg( deviceBox->currentText() );
        if ( layer->framesReceived() )
            text += tr( ", frames received: %1" ).arg( layer->framesReceived() );
        else
            text += tr( ", NO frames yet" );
        if ( layer->framesDropped() )
            text += tr( "; dropped %1 — %2" ).arg( layer->framesDropped() ).arg( layer->lastDropReason() );
        statusLabel->setText( text );
        return;
    }
    const QString why = layer->lastError();
    statusLabel->setText( why.isEmpty() ? tr( "selected, the layer is off" ) : tr( "not open — %1" ).arg( why ) );
}
