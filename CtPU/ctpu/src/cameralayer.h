// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/// \file cameralayer.h
/// \brief Кадр с камеры как самый задний слой холста.
///
/// Задание 3 очереди прототипа (`docs/PROTOTYPE-QUEUE.md`): «веб-камера
/// (стандартная USB): изображение на холст на самый задний план, однако
/// изображение с неё обязательно должно быть видимым сквозь все слои выше неё
/// (с какой-то прозрачностью)».
///
/// ## Что здесь есть и чего здесь нет
///
/// Здесь — **доставка кадра и то, что даёт стандартный драйвер**: выбрать
/// камеру, открыть её, получить последний кадр как `QImage`, показать
/// регуляторы, которые драйвер объявил поддержанными. Ни измерения, ни
/// обработки: камера как измерительный канал (пункт реестра **L-08**) —
/// отдельная задача, и инструмент для неё ещё не выбран. Смешивать вывод на
/// холст с измерением по изображению нельзя: это разные вещи с разными
/// требованиями к достоверности.
///
/// Кадр несёт **время регистрации** (`frameTimeMs`), а не время камеры:
/// отметку ставит регистрирующая система (`docs/TIMEBASE.md` §1). Собственная
/// метка камеры, если появится, ляжет рядом как `src_time`, а не вместо.
///
/// ## Выбор камеры — только явный
///
/// Распоряжение автора 2026-09-02: «нам встроенная камера НЕ нужна. нужен
/// явный выбор конкретной из списка доступных». Поэтому **пустой
/// идентификатор означает «камера не выбрана», а не «возьми первую»**:
/// `start()` в этом случае честно возвращает `false`. Молчаливая подстановка
/// первой попавшейся камеры была бы худшим видом ошибки — оператор видит
/// картинку и считает, что видит ту камеру, которую назначил.
///
/// Основание не только в удобстве. Ноутбук штатно несёт **вторую камеру, не
/// предназначенную для съёмки**: инфракрасный сенсор распознавания лица.
/// По документации драйверов Windows такая камера регистрируется в
/// `KSCATEGORY_SENSOR_CAMERA`, а её появление в обычном списке зависит от
/// ключа `SkipCameraEnumeration`; при этом «IR streams will show up as regular
/// capture streams in DShow». Qt 5 на Windows перечисляет камеры через
/// DirectShow, то есть ИК-камера может стоять в списке рядом с обычной и
/// давать почти чёрный кадр. Разбор с цитатами —
/// `docs/CAMERA-LAYER.md` §2.
///
/// ## Откуда берутся кадры: поверхность, а не проба
///
/// Кадры снимаются через `QAbstractVideoSurface`, назначенную камере
/// видоискателем, а не через `QVideoProbe`. Причина установлена по исходникам
/// Qt (разбор с цитатами — `docs/CAMERA-LAYER.md` §8): в
/// `dscamerasession.cpp::configurePreviewFormat()` запрос перевода потока в
/// `RGB32` стоит **внутри `if (m_surface)`**. Без поверхности DirectShow
/// оставляет формат камеры как есть — у обычной UVC-камеры это `YUYV` или
/// `MJPG`, — а `QVideoFrame::imageFormatFromPixelFormat()` для них возвращает
/// `QImage::Format_Invalid`. Кадры при этом идут, но в `QImage` не
/// превращаются.
///
/// Поверхность объявляет, что принимает `RGB32`, и построитель графа сам
/// вставляет преобразователь. Это же убирает **двоевластие**: путь к кадру
/// один, а не два.
///
/// Сборка без Qt Multimedia допустима: тогда класс есть, но кадров не даёт, и
/// приложение собирается и работает. Отсутствие камеры не должно ронять
/// прибор.

#include <QImage>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#ifdef CTPU_HAVE_CAMERA
#include <QAbstractVideoSurface>
#include <QCamera>
#include <QCameraInfo>
#include <QCameraImageProcessing>
#include <QCameraImageProcessingControl>
#include <QMediaService>
#include <QVideoFrame>
#include <memory>
#endif

#ifdef CTPU_HAVE_CAMERA
/// \brief Приёмник кадров камеры.
///
/// Объявляет ровно те форматы, которые умеет превратить в `QImage`
/// `QVideoFrame::imageFormatFromPixelFormat()`. Объявление здесь — не
/// формальность: именно по нему построитель графа DirectShow решает, вставлять
/// ли преобразователь из `YUYV`/`MJPG`. Соврать в этом списке значило бы
/// получать кадры, которые некуда деть.
class CameraSurface : public QAbstractVideoSurface {
    Q_OBJECT
  public:
    explicit CameraSurface( QObject *parent = nullptr );

    QList< QVideoFrame::PixelFormat > supportedPixelFormats(
        QAbstractVideoBuffer::HandleType type = QAbstractVideoBuffer::NoHandle ) const override;
    bool present( const QVideoFrame &frame ) override;

  signals:
    /// Кадр получен и превращён в изображение.
    void frameArrived( const QImage &image );
    /// Кадр получен, но использовать его нельзя. Причина названа словами:
    /// молчаливая потеря кадра — это ровно тот дефект, из-за которого
    /// изображение не появлялось, и повторяться он не должен.
    void frameDropped( const QString &reason );
};
#endif


/// \brief Источник кадров для заднего слоя холста.
class CameraLayer : public QObject {
    Q_OBJECT
  public:
    /// \brief Камера в списке доступных.
    ///
    /// `id` — то, чем камера адресуется и что запоминается в настройках
    /// (`QCameraInfo::deviceName()`); `description` — то, что читает
    /// оператор. Запоминать описание нельзя: у двух одинаковых камер оно
    /// совпадает, а адресуют они разное железо.
    struct Device {
        QString id;
        QString description;
    };

    /// \brief Регулятор, который объявил поддержанным стандартный драйвер.
    ///
    /// Список строится **по ответу драйвера**, а не по нашему представлению о
    /// том, что бывает у камер: в DirectShow неподдержанное свойство отвечает
    /// отказом на `GetRange`, и Qt такой параметр в поддержанные не заносит.
    /// Поэтому пустой список — это факт о драйвере, а не недоделка.
    /// \brief Почему камера не открылась.
    ///
    /// Причина — **данные, а не строка**: по строке нельзя ни проверить
    /// поведение тестом, ни отличить отказ от отказа после перевода
    /// интерфейса. `NotSelected` и `NotPresent` — разные вещи, и смешивать их
    /// нельзя: первое значит «оператор не выбрал», второе — «выбранного нет».
    enum class Refusal {
        None,        ///< отказа не было
        NotSelected, ///< идентификатор пуст: камера не выбрана
        NotPresent,  ///< выбранной камеры нет в системе
        SurfaceFailed, ///< камера найдена, но приёмник кадров к ней не встал
        NoMultimedia ///< сборка без Qt Multimedia
    };

    struct Control {
        QString key;         ///< внутреннее имя: brightness, contrast, …
        QString label;       ///< подпись оператору
        double minimum = -1; ///< в единицах Qt
        double maximum = 1;
        double value = 0;
        QString unit; ///< пусто — величина безразмерная (см. `docs/СЛЫШИМОСТЬ.md`)
    };

    explicit CameraLayer( QObject *parent = nullptr );
    ~CameraLayer() override;

    /// \brief Камеры, которые видит система. Пусто, если сборка без Qt Multimedia.
    static QList< Device > availableDevices();

    /// \brief Имена доступных камер (для сообщений).
    static QStringList availableCameras();

    /// \brief Открыть камеру по идентификатору.
    /// \param deviceId `QCameraInfo::deviceName()`. **Пусто — камера не
    ///        выбрана**, метод возвращает false и ничего не открывает.
    /// \return false, если камера не выбрана, не найдена, не открылась или
    ///         сборка без Qt Multimedia. Молчаливой подмены не происходит.
    bool start( const QString &deviceId );
    void stop();

    bool isActive() const { return m_active; }

    /// \brief Идентификатор открытой камеры; пусто — не открыта.
    QString currentDeviceId() const { return m_deviceId; }

    /// \brief Почему не открылась, словами для оператора. Пусто — ошибки не было.
    QString lastError() const { return m_lastError; }

    /// \brief Почему не открылась, значением для программы и теста.
    Refusal lastRefusal() const { return m_lastRefusal; }

    /// \brief Последний полученный кадр. Пустой QImage, если кадров не было.
    ///
    /// Кадр отдаётся УЖЕ в выбранной оператором ориентации: и холст, и снимок
    /// «фото камеры» берут его отсюда, поэтому два снимка одного момента не
    /// могут разойтись поворотом. Ориентация имеет одного хозяина — настройки,
    /// а применяется в одном месте — здесь.
    const QImage &frame() const { return m_frame; }

    /// \brief Ориентация кадра: поворот и зеркала.
    ///
    /// Требование автора 2026-09-02 («сделай повороты и зеркалку в
    /// настройках»). Камера над платой стоит как придётся, и «правильной»
    /// ориентации у неё нет по построению: её знает только оператор. Поэтому
    /// это регулятор, а не константа в коде.
    ///
    /// \param rotationDeg 0, 90, 180 или 270; иное приводится к ближайшему
    ///        кратному 90 по модулю 360.
    /// \param mirrorH отражение слева направо, \param mirrorV — сверху вниз.
    void setOrientation( int rotationDeg, bool mirrorH, bool mirrorV );

    /// \brief Повернуть и отразить изображение.
    ///
    /// Статическая и чистая: проверяется тестом без камеры и без OpenGL.
    /// Порядок операций закреплён — СНАЧАЛА зеркала, ПОТОМ поворот; при
    /// обратном порядке «повернуть на 90 и отразить» давало бы другой
    /// результат, и подписи в окне перестали бы отвечать увиденному.
    static QImage applyOrientation( const QImage &image, int rotationDeg, bool mirrorH, bool mirrorV );

    /// Поворот, приведённый к 0/90/180/270.
    static int normalizedRotation( int rotationDeg );

    /// \brief Время РЕГИСТРАЦИИ кадра (мс от эпохи), 0 — кадров не было.
    qint64 frameTimeMs() const { return m_frameTimeMs; }

    /// \name Счёт кадров — чтобы «камера открыта» не подменяло «кадры идут»
    ///@{
    /// \brief Сколько кадров дошло до холста.
    quint64 framesReceived() const { return m_framesReceived; }
    /// \brief Сколько кадров пришлось выбросить.
    quint64 framesDropped() const { return m_framesDropped; }
    /// \brief Почему выброшен последний. Пусто — потерь не было.
    QString lastDropReason() const { return m_lastDropReason; }
    ///@}

    /// \name Регуляторы стандартного драйвера
    ///@{
    /// \brief Что драйвер объявил поддержанным. Пусто — драйвер не даёт ничего.
    QList< Control > supportedControls() const;
    /// \brief Задать значение. false — параметр не поддержан или камера закрыта.
    bool setControl( const QString &key, double value );
    /// \brief Автоматический баланс белого поддержан?
    bool hasAutoWhiteBalance() const;
    /// \brief Включить/выключить автоматический баланс белого.
    bool setAutoWhiteBalance( bool automatic );
    /// \brief Текущее состояние автоматического баланса белого.
    bool autoWhiteBalance() const;
    ///@}

  signals:
    /// Пришёл новый кадр — холсту пора перерисоваться.
    void frameReady();
    /// Изменилось то, что показывает окно настройки: счёт кадров, причина
    /// потери. Отдельный сигнал, потому что перерисовывать холст на каждую
    /// потерю не нужно, а сказать о ней нужно.
    void stateChanged();

  private:
    QImage m_frame;
    /// Кадр как пришёл от камеры, до поворота и зеркал. Нужен потому, что
    /// смена ориентации обязана считаться от исходного кадра, а не от уже
    /// повёрнутого: иначе два поворота по 90° дали бы 180°.
    QImage m_frameAsReceived;
    int m_rotation = 0;
    bool m_mirrorH = false;
    bool m_mirrorV = false;
    qint64 m_frameTimeMs = 0;
    bool m_active = false;
    QString m_deviceId;
    QString m_lastError;
    Refusal m_lastRefusal = Refusal::None;
    quint64 m_framesReceived = 0;
    quint64 m_framesDropped = 0;
    QString m_lastDropReason;
#ifdef CTPU_HAVE_CAMERA
    std::unique_ptr< QCamera > m_camera;
    std::unique_ptr< CameraSurface > m_surface;
    /// Управление обработкой изображения у драйвера. Взято через
    /// `QMediaService::requestControl()` на время жизни камеры и возвращено в
    /// `stop()`: одалживать его на каждый вопрос значило бы дёргать драйвер
    /// на каждое движение ползунка.
    QCameraImageProcessingControl *m_imageControl = nullptr;
#endif
};
