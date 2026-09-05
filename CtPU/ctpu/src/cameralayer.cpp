// SPDX-License-Identifier: GPL-3.0-or-later

#include "cameralayer.h"

#include <QTransform>

#include <QDateTime>

#ifdef CTPU_HAVE_CAMERA
#include <QVideoFrame>
#include <QVideoSurfaceFormat>
#endif


#ifdef CTPU_HAVE_CAMERA
namespace {
    /// Таблица регуляторов: наше имя ↔ параметр Qt ↔ подпись и единица.
    ///
    /// Список короткий не по лени. Qt 5 на Windows (плагин `directshow`)
    /// отображает в параметры Qt ровно пять свойств `IAMVideoProcAmp` —
    /// Brightness, Contrast, Saturation, Sharpness, WhiteBalance, — а
    /// остальные пропускает (`dscamerasession.cpp`,
    /// `updateImageProcessingParametersInfos()`). Показывать регулятор, по
    /// которому нельзя ни спросить, ни задать, значило бы обещать оператору
    /// управление, которого нет.
    struct ControlDef {
        const char *key;
        QCameraImageProcessingControl::ProcessingParameter parameter;
        const char *label;
        double minimum;
        double maximum;
        const char *unit;
    };

    const ControlDef controlTable[] = {
        { "brightness", QCameraImageProcessingControl::BrightnessAdjustment, QT_TRANSLATE_NOOP( "CameraLayer", "Brightness" ),
          -1.0, 1.0, QT_TRANSLATE_NOOP( "CameraLayer", "driver units" ) },
        { "contrast", QCameraImageProcessingControl::ContrastAdjustment, QT_TRANSLATE_NOOP( "CameraLayer", "Contrast" ), -1.0,
          1.0, QT_TRANSLATE_NOOP( "CameraLayer", "driver units" ) },
        { "saturation", QCameraImageProcessingControl::SaturationAdjustment, QT_TRANSLATE_NOOP( "CameraLayer", "Saturation" ),
          -1.0, 1.0, QT_TRANSLATE_NOOP( "CameraLayer", "driver units" ) },
        { "sharpening", QCameraImageProcessingControl::SharpeningAdjustment, QT_TRANSLATE_NOOP( "CameraLayer", "Sharpness" ),
          -1.0, 1.0, QT_TRANSLATE_NOOP( "CameraLayer", "driver units" ) },
        // Цветовая температура — единственный регулятор набора с настоящей
        // физической единицей, поэтому она и объявлена (правило
        // `docs/СЛЫШИМОСТЬ.md`: величина без единицы числом не показывается).
        { "colorTemperature", QCameraImageProcessingControl::ColorTemperature,
          QT_TRANSLATE_NOOP( "CameraLayer", "Colour temperature" ), 2000.0, 10000.0, "K" },
    };
} // namespace
#endif


#ifdef CTPU_HAVE_CAMERA
namespace {
    /// Имя формата кадра словами.
    ///
    /// Нужно затем, чтобы причина потери кадра читалась оператором, а не была
    /// числом. Перечислены те форматы, которые реально отдают камеры; всё
    /// прочее показывается номером — это честнее выдуманного имени.
    QString pixelFormatName( QVideoFrame::PixelFormat f ) {
        switch ( f ) {
        case QVideoFrame::Format_YUYV:
            return QStringLiteral( "YUYV" );
        case QVideoFrame::Format_UYVY:
            return QStringLiteral( "UYVY" );
        case QVideoFrame::Format_NV12:
            return QStringLiteral( "NV12" );
        case QVideoFrame::Format_NV21:
            return QStringLiteral( "NV21" );
        case QVideoFrame::Format_YV12:
            return QStringLiteral( "YV12" );
        case QVideoFrame::Format_YUV420P:
            return QStringLiteral( "YUV420P" );
        case QVideoFrame::Format_Jpeg:
            return QStringLiteral( "MJPG" );
        case QVideoFrame::Format_BGR24:
            return QStringLiteral( "BGR24" );
        case QVideoFrame::Format_BGR32:
            return QStringLiteral( "BGR32" );
        case QVideoFrame::Format_BGRA32:
            return QStringLiteral( "BGRA32" );
        default:
            return QStringLiteral( "#%1" ).arg( int( f ) );
        }
    }
} // namespace


CameraSurface::CameraSurface( QObject *parent ) : QAbstractVideoSurface( parent ) {}


/// Форматы, которые мы обязуемся принять.
///
/// Список не «на всякий случай пошире»: по нему построитель графа решает,
/// нужен ли преобразователь. Перечислено ровно то, что
/// `QVideoFrame::imageFormatFromPixelFormat()` умеет отдать как `QImage`;
/// первым — `RGB32`, потому что именно его DirectShow берёт запасным
/// вариантом (`dscamerasession.cpp::configurePreviewFormat()`).
QList< QVideoFrame::PixelFormat > CameraSurface::supportedPixelFormats( QAbstractVideoBuffer::HandleType type ) const {
    if ( type != QAbstractVideoBuffer::NoHandle )
        return {}; // кадры в памяти GPU нам не годятся: мы их читаем
    return { QVideoFrame::Format_RGB32, QVideoFrame::Format_ARGB32, QVideoFrame::Format_ARGB32_Premultiplied,
             QVideoFrame::Format_RGB24, QVideoFrame::Format_RGB565, QVideoFrame::Format_RGB555 };
}


bool CameraSurface::present( const QVideoFrame &frame ) {
    QVideoFrame copy( frame );
    if ( !copy.map( QAbstractVideoBuffer::ReadOnly ) ) {
        emit frameDropped( tr( "the frame could not be mapped into memory" ) );
        return false;
    }
    const QImage::Format fmt = QVideoFrame::imageFormatFromPixelFormat( copy.pixelFormat() );
    QImage image;
    if ( fmt != QImage::Format_Invalid )
        image = QImage( copy.bits(), copy.width(), copy.height(), copy.bytesPerLine(), fmt ).copy();
    const QString formatName = pixelFormatName( copy.pixelFormat() );
    copy.unmap();

    if ( image.isNull() ) {
        // Тот самый молчаливый отказ, из-за которого изображения не было.
        // Теперь он назван: оператор видит формат, а не пустой холст.
        emit frameDropped( tr( "the camera delivers %1, which does not convert to an image" ).arg( formatName ) );
        return false;
    }
    emit frameArrived( image );
    return true;
}
#endif


CameraLayer::CameraLayer( QObject *parent ) : QObject( parent ) {}


int CameraLayer::normalizedRotation( int rotationDeg ) {
    int r = rotationDeg % 360;
    if ( r < 0 )
        r += 360;
    // К ближайшему кратному 90: промежуточные углы потребовали бы
    // интерполяции и полей, а это уже обработка изображения, которой у слоя
    // нет и не должно быть (`cameralayer.h`: только вывод).
    return ( ( r + 45 ) / 90 % 4 ) * 90;
}


QImage CameraLayer::applyOrientation( const QImage &image, int rotationDeg, bool mirrorH, bool mirrorV ) {
    const int rotation = normalizedRotation( rotationDeg );
    if ( image.isNull() || ( rotation == 0 && !mirrorH && !mirrorV ) )
        return image; // обычный путь не стоит ничего: ни копии, ни прохода
    QImage result = ( mirrorH || mirrorV ) ? image.mirrored( mirrorH, mirrorV ) : image;
    if ( rotation != 0 ) {
        QTransform t;
        t.rotate( rotation );
        result = result.transformed( t );
    }
    return result;
}


void CameraLayer::setOrientation( int rotationDeg, bool mirrorH, bool mirrorV ) {
    const int rotation = normalizedRotation( rotationDeg );
    if ( rotation == m_rotation && mirrorH == m_mirrorH && mirrorV == m_mirrorV )
        return;
    m_rotation = rotation;
    m_mirrorH = mirrorH;
    m_mirrorV = mirrorV;
    // Уже полученный кадр переориентируется немедленно: иначе до следующего
    // кадра холст показывал бы прежнюю ориентацию, и оператор решил бы, что
    // регулятор не работает. У остановленной камеры следующего кадра может не
    // быть вовсе.
    if ( !m_frame.isNull() ) {
        m_frame = applyOrientation( m_frameAsReceived, m_rotation, m_mirrorH, m_mirrorV );
        emit frameReady();
    }
}


CameraLayer::~CameraLayer() { stop(); }


QList< CameraLayer::Device > CameraLayer::availableDevices() {
    QList< Device > devices;
#ifdef CTPU_HAVE_CAMERA
    for ( const QCameraInfo &info : QCameraInfo::availableCameras() )
        devices.append( Device{ info.deviceName(), info.description() } );
#endif
    return devices;
}


QStringList CameraLayer::availableCameras() {
    QStringList names;
    for ( const Device &d : availableDevices() )
        names << d.description;
    return names;
}


bool CameraLayer::start( const QString &deviceId ) {
    stop();
    m_lastError.clear();
    m_lastRefusal = Refusal::None;

    // Пустой идентификатор — это «камера не выбрана», а не «возьми первую».
    // Распоряжение автора 2026-09-02: выбор только явный. Подстановка первой
    // доступной показала бы оператору чужую картинку под видом назначенной —
    // например инфракрасный сенсор распознавания лица, который на ноутбуке
    // стоит в том же списке (`docs/CAMERA-LAYER.md` §2).
    //
    // Проверка стоит ДО #ifdef: правило о выборе — про оператора, а не про
    // состав сборки, и обязано действовать одинаково везде.
    if ( deviceId.trimmed().isEmpty() ) {
        m_lastError = tr( "no camera selected" );
        m_lastRefusal = Refusal::NotSelected;
        return false;
    }

#ifdef CTPU_HAVE_CAMERA
    const QList< QCameraInfo > cams = QCameraInfo::availableCameras();
    QCameraInfo chosen;
    for ( const QCameraInfo &info : cams )
        if ( info.deviceName() == deviceId ) {
            chosen = info;
            break;
        }
    if ( chosen.isNull() ) {
        // Камера, записанная в настройках, отключена или переименована.
        // Сообщаем именно это, а не открываем другую.
        m_lastError = tr( "the selected camera is not present: %1" ).arg( deviceId );
        m_lastRefusal = Refusal::NotPresent;
        return false;
    }

    m_camera = std::make_unique< QCamera >( chosen );
    m_surface = std::make_unique< CameraSurface >();

    connect( m_surface.get(), &CameraSurface::frameArrived, this, [ this ]( const QImage &image ) {
        // Отметку ставит регистрирующая система, а не камера
        // (`docs/TIMEBASE.md` §1).
        m_frameTimeMs = QDateTime::currentMSecsSinceEpoch();
        // Кадр как пришёл хранится отдельно: смена ориентации не должна
        // накладываться на уже повёрнутый кадр — два поворота подряд дали бы
        // 180° вместо 90°.
        m_frameAsReceived = image;
        m_frame = applyOrientation( image, m_rotation, m_mirrorH, m_mirrorV );
        ++m_framesReceived;
        if ( m_framesReceived == 1 )
            emit stateChanged(); // первый кадр меняет то, что написано в окне
        emit frameReady();
    } );
    connect( m_surface.get(), &CameraSurface::frameDropped, this, [ this ]( const QString &reason ) {
        ++m_framesDropped;
        if ( m_lastDropReason != reason ) {
            m_lastDropReason = reason;
            emit stateChanged();
        }
    } );

    // Камера открывается НЕ мгновенно: QCamera::start() лишь запускает
    // загрузку, а граф строится дальше сам. Пока он не построен, драйвер не
    // знает своих же параметров, и список регуляторов пуст. Прежде окно
    // спрашивало их сразу после start() и получало пустоту - оттого регуляторы
    // и появлялись только со второго открытия окна. Теперь о готовности
    // сообщает сама камера.
    connect( m_camera.get(), &QCamera::statusChanged, this, [ this ]( QCamera::Status status ) {
        if ( status == QCamera::ActiveStatus && !m_imageControl && m_camera ) {
            if ( QMediaService *service = m_camera->service() )
                m_imageControl = service->requestControl< QCameraImageProcessingControl * >();
        }
        emit stateChanged();
    } );
    // Отказ, наступивший ПОСЛЕ запуска, тоже должен быть назван: молчаливо
    // погасшая камера выглядит как работающая.
    connect( m_camera.get(), QOverload< QCamera::Error >::of( &QCamera::error ), this, [ this ]( QCamera::Error ) {
        if ( m_camera )
            m_lastError = m_camera->errorString();
        emit stateChanged();
    } );

    // Видоискатель ставится ДО start(): именно по объявленным поверхностью
    // форматам строится граф. После запуска переговоры о формате уже прошли.
    m_camera->setViewfinder( m_surface.get() );
    m_camera->start();
    if ( m_camera->error() != QCamera::NoError ) {
        m_lastError = m_camera->errorString();
        m_lastRefusal = Refusal::SurfaceFailed;
        m_camera->stop();
        m_camera.reset();
        m_surface.reset();
        return false;
    }
    if ( QMediaService *service = m_camera->service() )
        m_imageControl = service->requestControl< QCameraImageProcessingControl * >();
    m_active = true;
    m_deviceId = deviceId;
    return true;
#else
    Q_UNUSED( deviceId )
    m_lastError = tr( "built without Qt Multimedia: the camera layer is not available" );
    m_lastRefusal = Refusal::NoMultimedia;
    return false;
#endif
}


void CameraLayer::stop() {
#ifdef CTPU_HAVE_CAMERA
    if ( m_imageControl && m_camera ) {
        if ( QMediaService *service = m_camera->service() )
            service->releaseControl( m_imageControl );
    }
    m_imageControl = nullptr;
    if ( m_camera ) {
        m_camera->stop();
        m_camera->setViewfinder( static_cast< QAbstractVideoSurface * >( nullptr ) );
    }
    m_camera.reset();
    m_surface.reset();
#endif
    m_active = false;
    m_deviceId.clear();
    // Кадр не сохраняем: слоя нет — значит нет и картинки. Иначе на холсте
    // навечно застыл бы последний кадр закрытой камеры.
    m_frame = QImage();
    m_frameAsReceived = QImage();
    m_frameTimeMs = 0;
    m_framesReceived = 0;
    m_framesDropped = 0;
    m_lastDropReason.clear();
}


QList< CameraLayer::Control > CameraLayer::supportedControls() const {
    QList< Control > controls;
#ifdef CTPU_HAVE_CAMERA
    if ( !m_imageControl )
        return controls;
    for ( const auto &def : controlTable ) {
        // Спрашиваем драйвер, а не гадаем: в DirectShow неподдержанное
        // свойство отвечает отказом на GetRange, и Qt его в поддержанные не
        // заносит.
        if ( !m_imageControl->isParameterSupported( def.parameter ) )
            continue;
        Control c;
        c.key = QString::fromLatin1( def.key );
        c.label = tr( def.label );
        c.minimum = def.minimum;
        c.maximum = def.maximum;
        c.unit = QString::fromUtf8( def.unit ) == QStringLiteral( "K" ) ? QStringLiteral( "K" ) : tr( def.unit );
        const QVariant v = m_imageControl->parameter( def.parameter );
        c.value = v.isValid() ? v.toDouble() : ( def.minimum + def.maximum ) / 2.0;
        controls.append( c );
    }
#endif
    return controls;
}


bool CameraLayer::setControl( const QString &key, double value ) {
#ifdef CTPU_HAVE_CAMERA
    if ( !m_imageControl )
        return false;
    for ( const auto &def : controlTable ) {
        if ( key != QLatin1String( def.key ) )
            continue;
        if ( !m_imageControl->isParameterSupported( def.parameter ) )
            return false;
        const QVariant v = def.parameter == QCameraImageProcessingControl::ColorTemperature
                               ? QVariant( qint32( value ) )
                               : QVariant( value );
        if ( !m_imageControl->isParameterValueSupported( def.parameter, v ) )
            return false;
        m_imageControl->setParameter( def.parameter, v );
        return true;
    }
#else
    Q_UNUSED( key )
    Q_UNUSED( value )
#endif
    return false;
}


bool CameraLayer::hasAutoWhiteBalance() const {
#ifdef CTPU_HAVE_CAMERA
    if ( !m_imageControl )
        return false;
    return m_imageControl->isParameterValueSupported(
        QCameraImageProcessingControl::WhiteBalancePreset,
        QVariant::fromValue< QCameraImageProcessing::WhiteBalanceMode >( QCameraImageProcessing::WhiteBalanceAuto ) );
#else
    return false;
#endif
}


bool CameraLayer::setAutoWhiteBalance( bool automatic ) {
#ifdef CTPU_HAVE_CAMERA
    if ( !m_imageControl )
        return false;
    const auto mode = automatic ? QCameraImageProcessing::WhiteBalanceAuto : QCameraImageProcessing::WhiteBalanceManual;
    const QVariant v = QVariant::fromValue< QCameraImageProcessing::WhiteBalanceMode >( mode );
    if ( !m_imageControl->isParameterValueSupported( QCameraImageProcessingControl::WhiteBalancePreset, v ) )
        return false;
    m_imageControl->setParameter( QCameraImageProcessingControl::WhiteBalancePreset, v );
    return true;
#else
    Q_UNUSED( automatic )
    return false;
#endif
}


bool CameraLayer::autoWhiteBalance() const {
#ifdef CTPU_HAVE_CAMERA
    if ( !m_imageControl )
        return false;
    const QVariant v = m_imageControl->parameter( QCameraImageProcessingControl::WhiteBalancePreset );
    return v.isValid() && v.value< QCameraImageProcessing::WhiteBalanceMode >() == QCameraImageProcessing::WhiteBalanceAuto;
#else
    return false;
#endif
}
