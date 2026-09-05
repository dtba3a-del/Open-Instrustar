// SPDX-License-Identifier: GPL-3.0-or-later
//
// Регресс-тест на молчаливую подмену камеры.
//
// Дефект, ради которого написан. Прежняя редакция `CameraLayer::start()`
// принимала пустое имя как «возьми первую доступную». Распоряжение автора
// 2026-09-02: «нам встроенная камера НЕ нужна. нужен явный выбор конкретной из
// списка доступных». Подстановка первой попавшейся опасна не удобством, а
// достоверностью: ноутбук штатно несёт вторую камеру, не предназначенную для
// съёмки (инфракрасный сенсор распознавания лица), и она перечисляется рядом с
// обычной — «IR streams will show up as regular capture streams in DShow»
// (документация драйверов Windows, цитаты — `docs/CAMERA-LAYER.md` §2).
// Оператор увидел бы картинку и считал, что видит назначенную камеру.
//
// Тест закрепляет три свойства. Он не требует ни камеры, ни Qt Multimedia:
// проверяются именно отказы, а они обязаны наступать всегда.

#include <QtTest>

#include "cameralayer.h"
#include "camerashaders.h"

class TestCameraLayer : public QObject {
    Q_OBJECT
  private slots:
    void testEmptyIdIsNotAWildcard();
    void testUnknownIdIsNotSubstituted();
    void testStopClearsFrameAndDevice();
    void testSurfaceAdvertisesOnlyConvertibleFormats();
    void testShaderSourcesCarryAVersionDirective();
    void testShaderSourcesMatchTheirDialect();
    void testFitScaleKeepsProportions();
    void testHugeFrameIsNormalizedToTheCanvas();
    void testRotationIsNormalizedToQuarterTurns();
    void testOrientationMovesPixelsWhereItSays();
    void testOrientationIdentityCostsNothing();
};


namespace {
    /// Кадр-указатель: три разноцветных пикселя в трёх углах 3x2.
    /// Углы и есть то, по чему видно поворот; симметричная картинка не
    /// различила бы поворот на 180° и пару зеркал.
    QImage cornerMarkedImage() {
        QImage img( 3, 2, QImage::Format_RGB32 );
        img.fill( Qt::black );
        img.setPixel( 0, 0, qRgb( 255, 0, 0 ) );  // верхний левый - красный
        img.setPixel( 2, 0, qRgb( 0, 255, 0 ) );  // верхний правый - зелёный
        img.setPixel( 0, 1, qRgb( 0, 0, 255 ) );  // нижний левый - синий
        return img;
    }
} // namespace


/// Пустой идентификатор — «камера не выбрана», а не «любая».
void TestCameraLayer::testEmptyIdIsNotAWildcard() {
    CameraLayer layer;
    QVERIFY( !layer.start( QString() ) );
    QVERIFY( !layer.isActive() );
    QVERIFY( layer.currentDeviceId().isEmpty() );
    // Отказ обязан быть назван словами: молчаливый false оператор не прочтёт.
    QVERIFY( !layer.lastError().isEmpty() );
    // И назван значением: именно «не выбрана», а не «нет такой». Проверка по
    // значению, а не по тексту, — единственная, которая ловит откат к «первой
    // доступной» и на машине С камерой: там прежняя редакция вернула бы
    // Refusal::None, а без камеры — Refusal::NotPresent. Оба не равны
    // NotSelected, и тест падает в обоих случаях.
    QCOMPARE( layer.lastRefusal(), CameraLayer::Refusal::NotSelected );
}


/// Запомненной камеры нет в системе — открывается НИЧТО, а не соседняя.
void TestCameraLayer::testUnknownIdIsNotSubstituted() {
    CameraLayer layer;
    QVERIFY( !layer.start( QStringLiteral( "no-such-camera-0000" ) ) );
    QVERIFY( !layer.isActive() );
    QVERIFY( layer.currentDeviceId().isEmpty() );
    QVERIFY( !layer.lastError().isEmpty() );
#ifdef CTPU_HAVE_CAMERA
    QCOMPARE( layer.lastRefusal(), CameraLayer::Refusal::NotPresent );
#else
    QCOMPARE( layer.lastRefusal(), CameraLayer::Refusal::NoMultimedia );
#endif
}


/// Закрытая камера не оставляет за собой кадр.
///
/// Иначе на холсте навечно застыл бы последний кадр выключенного источника —
/// картинка есть, источника нет.
void TestCameraLayer::testStopClearsFrameAndDevice() {
    CameraLayer layer;
    layer.stop();
    QVERIFY( layer.frame().isNull() );
    QCOMPARE( layer.frameTimeMs(), qint64( 0 ) );
    QVERIFY( layer.currentDeviceId().isEmpty() );
    QVERIFY( !layer.isActive() );
    // Регуляторы у закрытой камеры не выдумываются.
    QVERIFY( layer.supportedControls().isEmpty() );
    QVERIFY( !layer.setControl( QStringLiteral( "brightness" ), 0.5 ) );
    // Счёт кадров закрытой камеры — ноль, а не «сколько было в прошлый раз».
    QCOMPARE( layer.framesReceived(), quint64( 0 ) );
    QCOMPARE( layer.framesDropped(), quint64( 0 ) );
    QVERIFY( layer.lastDropReason().isEmpty() );
}


/// Приёмник кадров объявляет только то, что умеет превратить в изображение.
///
/// Дефект, ради которого написан: без поверхности DirectShow оставлял формат
/// камеры как есть (`YUYV`/`MJPG`), а `imageFormatFromPixelFormat()` отдаёт для
/// них `QImage::Format_Invalid` — кадры шли и молча выбрасывались. Список
/// форматов и есть то, по чему граф решает вставить преобразователь; соврать в
/// нём значило бы вернуть дефект.
void TestCameraLayer::testSurfaceAdvertisesOnlyConvertibleFormats() {
#ifdef CTPU_HAVE_CAMERA
    CameraSurface surface;
    const QList< QVideoFrame::PixelFormat > formats = surface.supportedPixelFormats( QAbstractVideoBuffer::NoHandle );
    QVERIFY( !formats.isEmpty() );
    QVERIFY( formats.contains( QVideoFrame::Format_RGB32 ) ); // запасной вариант DirectShow
    for ( QVideoFrame::PixelFormat f : formats )
        QVERIFY2( QVideoFrame::imageFormatFromPixelFormat( f ) != QImage::Format_Invalid,
                  "объявлен формат, который не превращается в QImage" );
    // Форматы камеры, которые в QImage не переводятся, объявлять нельзя.
    QVERIFY( !formats.contains( QVideoFrame::Format_YUYV ) );
    QVERIFY( !formats.contains( QVideoFrame::Format_Jpeg ) );
    // Кадры в памяти GPU мы читать не умеем и не обещаем.
    QVERIFY( surface.supportedPixelFormats( QAbstractVideoBuffer::GLTextureHandle ).isEmpty() );
#else
    QSKIP( "сборка без Qt Multimedia: поверхности нет" );
#endif
}


/// Исходник шейдера слоя начинается директивой, а не меткой диалекта.
///
/// Дефект, ради которого написан, — тот, из-за которого кадра не было НА
/// ХОЛСТЕ при исправно работающей камере. Исходник склеивался как
/// `GLSLversion + "attribute highp vec3 vertex;…"`, а `GLSLversion` — метка
/// («1.50»), а не директива `#version`. Получалась строка
/// `1.50attribute highp vec3 vertex;`, шейдер не компилировался ни при какой
/// конфигурации, программа не линковалась, слой не рисовался. Ни сборка, ни
/// запуск при этом не отказывали — только предупреждение в поток.
///
/// Проверка текстовая и потому идёт на сборочной машине, где графики нет:
/// сломалось именно ПОСТРОЕНИЕ исходника, а его видно без контекста OpenGL.
void TestCameraLayer::testShaderSourcesCarryAVersionDirective() {
    const QStringList dialects = { QStringLiteral( GLES100 ), QStringLiteral( GLSL120 ), QStringLiteral( GLSL150 ),
                                   QStringLiteral( "не такой диалект" ) };
    for ( const QString &d : dialects ) {
        const CameraShaders::Sources src = CameraShaders::forGlslVersion( d );
        for ( const QString &text : { src.vertex, src.fragment } ) {
            QVERIFY2( text.startsWith( QLatin1String( "#version " ) ),
                      qPrintable( QStringLiteral( "исходник для %1 начинается не с #version: %2" )
                                      .arg( d, text.left( 24 ) ) ) );
            // Метка диалекта в текст шейдера не попадает ни при каком виде.
            QVERIFY( !text.contains( QLatin1String( "1.50" ) ) );
            QVERIFY( !text.contains( QLatin1String( "1.20" ) ) );
            QVERIFY( !text.contains( QLatin1String( "1.00 ES" ) ) );
        }
    }
}


/// Каждый диалект получает СВОИ ключевые слова, а не чужие.
void TestCameraLayer::testShaderSourcesMatchTheirDialect() {
    const CameraShaders::Sources es = CameraShaders::forGlslVersion( QStringLiteral( GLES100 ) );
    QVERIFY( es.vertex.startsWith( QLatin1String( "#version 100" ) ) );
    QVERIFY( es.vertex.contains( QLatin1String( "attribute" ) ) );
    QVERIFY( es.fragment.contains( QLatin1String( "texture2D" ) ) );
    QVERIFY( es.fragment.contains( QLatin1String( "gl_FragColor" ) ) );

    const CameraShaders::Sources v120 = CameraShaders::forGlslVersion( QStringLiteral( GLSL120 ) );
    QVERIFY( v120.vertex.startsWith( QLatin1String( "#version 120" ) ) );
    QVERIFY( v120.vertex.contains( QLatin1String( "attribute" ) ) );
    QVERIFY( v120.fragment.contains( QLatin1String( "texture2D" ) ) );

    const CameraShaders::Sources v150 = CameraShaders::forGlslVersion( QStringLiteral( GLSL150 ) );
    QVERIFY( v150.vertex.startsWith( QLatin1String( "#version 150" ) ) );
    // В ядре 1.50 нет ни `attribute`, ни `varying`, ни `texture2D`, ни
    // `gl_FragColor`: там `in`/`out`/`texture` и собственный выход.
    QVERIFY( !v150.vertex.contains( QLatin1String( "attribute" ) ) );
    QVERIFY( !v150.vertex.contains( QLatin1String( "varying" ) ) );
    QVERIFY( !v150.fragment.contains( QLatin1String( "texture2D" ) ) );
    QVERIFY( !v150.fragment.contains( QLatin1String( "gl_FragColor" ) ) );
    QVERIFY( v150.fragment.contains( QLatin1String( "out vec4" ) ) );

    // Множитель нормализации есть во ВСЕХ трёх: иначе на одном из диалектов
    // кадр растягивался бы по холсту, а на других нет.
    for ( const CameraShaders::Sources &s : { es, v120, v150 } )
        QVERIFY( s.vertex.contains( QLatin1String( "fit" ) ) );
}


/// Кадр вписывается в холст целиком и без искажения пропорций.
void TestCameraLayer::testFitScaleKeepsProportions() {
    // Кадр шире холста — упирается в ширину, по высоте остаётся поле.
    const QSizeF wide = CameraShaders::fitScale( QSize( 4000, 1000 ), QSize( 1000, 800 ) );
    QCOMPARE( wide.width(), 1.0 );
    QVERIFY( wide.height() < 1.0 );
    // Пропорция сохранена: доля_ширины/доля_высоты == аспект_кадра/аспект_холста.
    QVERIFY( qAbs( ( wide.width() / wide.height() ) - ( 4.0 / ( 1000.0 / 800.0 ) ) ) < 1e-9 );

    // Кадр выше холста — упирается в высоту.
    const QSizeF tall = CameraShaders::fitScale( QSize( 1000, 4000 ), QSize( 1000, 800 ) );
    QCOMPARE( tall.height(), 1.0 );
    QVERIFY( tall.width() < 1.0 );

    // Аспекты совпали — кадр занимает холст целиком, без полей.
    const QSizeF same = CameraShaders::fitScale( QSize( 1920, 1080 ), QSize( 960, 540 ) );
    QCOMPARE( same.width(), 1.0 );
    QCOMPARE( same.height(), 1.0 );

    // Негодные размеры не порождают ни деления на ноль, ни пустого слоя.
    QCOMPARE( CameraShaders::fitScale( QSize(), QSize( 100, 100 ) ), QSizeF( 1.0, 1.0 ) );
    QCOMPARE( CameraShaders::fitScale( QSize( 100, 100 ), QSize() ), QSizeF( 1.0, 1.0 ) );
}


/// Кадр 40 Mp доходит до холста, а не пропадает вместе со слоем.
///
/// Распоряжение автора 2026-09-02: «размер изображения камеры может быть
/// разный… от 0,31 Mp до 40 Mp и это нужно нормализовать на не фиксированный
/// размер фона. иначе никак».
void TestCameraLayer::testHugeFrameIsNormalizedToTheCanvas() {
    const QSize canvas( 1280, 800 );

    // 40 Mp (7296x5472) при пределе драйвера 4096: обе стороны укладываются в
    // холст, пропорция сохраняется.
    const QSize huge = CameraShaders::normalizedFrameSize( QSize( 7296, 5472 ), canvas, 4096 );
    QVERIFY( !huge.isEmpty() );
    QVERIFY( huge.width() <= canvas.width() );
    QVERIFY( huge.height() <= canvas.height() );
    QVERIFY( qAbs( double( huge.width() ) / huge.height() - 7296.0 / 5472.0 ) < 0.01 );

    // 0,31 Mp (640x480) меньше холста — уменьшать нечего, пересчёта нет.
    QVERIFY( CameraShaders::normalizedFrameSize( QSize( 640, 480 ), canvas, 4096 ).isEmpty() );

    // Предел драйвера меньше холста — командует он.
    const QSize limited = CameraShaders::normalizedFrameSize( QSize( 7296, 5472 ), QSize( 8000, 6000 ), 2048 );
    QVERIFY( limited.width() <= 2048 && limited.height() <= 2048 );

    // Предел неизвестен (0) — ограничивает холст, и это не отказ.
    const QSize noLimit = CameraShaders::normalizedFrameSize( QSize( 7296, 5472 ), canvas, 0 );
    QVERIFY( noLimit.width() <= canvas.width() && noLimit.height() <= canvas.height() );

    // Крайне вытянутый кадр не вырождается в нулевую сторону: нулевая сторона
    // это снова «слоя нет», только по другой причине.
    const QSize thin = CameraShaders::normalizedFrameSize( QSize( 40000, 3 ), canvas, 4096 );
    QVERIFY( thin.width() >= 1 && thin.height() >= 1 );
}


/// Угол приводится к четверти оборота, и приведение не зависит от знака.
void TestCameraLayer::testRotationIsNormalizedToQuarterTurns() {
    QCOMPARE( CameraLayer::normalizedRotation( 0 ), 0 );
    QCOMPARE( CameraLayer::normalizedRotation( 90 ), 90 );
    QCOMPARE( CameraLayer::normalizedRotation( 360 ), 0 );
    QCOMPARE( CameraLayer::normalizedRotation( 450 ), 90 );
    QCOMPARE( CameraLayer::normalizedRotation( -90 ), 270 );
    QCOMPARE( CameraLayer::normalizedRotation( -360 ), 0 );
    // Промежуточный угол не превращается в наклон: у слоя нет обработки
    // изображения, только вывод.
    QCOMPARE( CameraLayer::normalizedRotation( 100 ), 90 );
    QCOMPARE( CameraLayer::normalizedRotation( 44 ), 0 );
}


/// Поворот и зеркала двигают пиксели туда, куда обещано подписью.
///
/// Требование автора 2026-09-02: «сделай повороты и зеркалку в настройках».
/// Проверка по УГЛАМ, а не по размеру: размер совпадает и у поворота на 180°,
/// и у пары зеркал, и подменой одного другим ошибку не поймать.
void TestCameraLayer::testOrientationMovesPixelsWhereItSays() {
    const QImage src = cornerMarkedImage();
    const QRgb red = qRgb( 255, 0, 0 ), green = qRgb( 0, 255, 0 ), blue = qRgb( 0, 0, 255 );

    // Зеркало слева направо: красный уходит из левого верхнего в правый верхний.
    const QImage h = CameraLayer::applyOrientation( src, 0, true, false );
    QCOMPARE( h.size(), src.size() );
    QCOMPARE( h.pixel( 2, 0 ), red );
    QCOMPARE( h.pixel( 0, 0 ), green );

    // Зеркало сверху вниз: красный уходит вниз, синий поднимается наверх.
    const QImage v = CameraLayer::applyOrientation( src, 0, false, true );
    QCOMPARE( v.pixel( 0, 1 ), red );
    QCOMPARE( v.pixel( 0, 0 ), blue );

    // Поворот на 90° по часовой: стороны меняются местами, верхний левый
    // угол уходит в верхний правый.
    const QImage r90 = CameraLayer::applyOrientation( src, 90, false, false );
    QCOMPARE( r90.width(), src.height() );
    QCOMPARE( r90.height(), src.width() );
    QCOMPARE( r90.pixel( 1, 0 ), red );

    // Поворот на 180° равен паре зеркал — но это РАЗНЫЕ пути к одному, и
    // равенство здесь проверяется, а не предполагается.
    const QImage r180 = CameraLayer::applyOrientation( src, 180, false, false );
    const QImage both = CameraLayer::applyOrientation( src, 0, true, true );
    QCOMPARE( r180.size(), src.size() );
    QCOMPARE( r180, both );

    // Порядок закреплён: сначала зеркала, потом поворот. Обратный порядок дал
    // бы другой угол, и подпись в окне перестала бы отвечать увиденному.
    const QImage mirrorThenRotate = CameraLayer::applyOrientation( src, 90, true, false );
    QTransform t;
    t.rotate( 90 );
    QCOMPARE( mirrorThenRotate, src.mirrored( true, false ).transformed( t ) );

    // 360° — это 0°, а не третий вариант.
    QCOMPARE( CameraLayer::applyOrientation( src, 360, false, false ), src );
}


/// Ориентация по умолчанию не трогает кадр вовсе.
///
/// Не удобство, а цена: кадр приходит десятки раз в секунду, и лишний проход
/// по 40 Mp на каждом кадре был бы платой за настройку, которой никто не
/// пользуется.
void TestCameraLayer::testOrientationIdentityCostsNothing() {
    const QImage src = cornerMarkedImage();
    const QImage same = CameraLayer::applyOrientation( src, 0, false, false );
    QCOMPARE( same, src );
    // Пустой кадр остаётся пустым при любой ориентации: поворачивать нечего.
    QVERIFY( CameraLayer::applyOrientation( QImage(), 90, true, true ).isNull() );
}


// Окна тесту не нужны: проверяются отказы открытия, а не рисование. GUILESS —
// чтобы прогон не требовал дисплея на сборочной машине.
QTEST_GUILESS_MAIN( TestCameraLayer )
#include "test_cameralayer.moc"
