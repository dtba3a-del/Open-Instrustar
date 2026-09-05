// SPDX-License-Identifier: GPL-3.0-or-later
// Last edited: 2026-09-06 UTC

#include "vdsobackend.h"

#include "ivdsoloader.h"
#include "ivdsosession.h"
#include "ivdsounits.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QThread>

namespace {

/// Часы для коннектора. Сон настоящий: подъём библиотеки требует паузы на
/// перечисление USB, и обмануть её нечем. Окно генератора — модальное,
/// секунда ожидания в нём предсказуема.
class QtClock : public IVdso::Clock {
  public:
    void sleepMs( int ms ) override {
        if ( ms > 0 )
            QThread::msleep( (unsigned long) ms );
    }
    uint64_t nowMs() override { return uint64_t( QDateTime::currentMSecsSinceEpoch() ); }
};

// Ключ, под которым запоминается выбор оператора. Одно место — иначе у
// величины появится второй хозяин.
const char *SETTINGS_KEY = "paths/vdsoLibrary";

} // namespace


VdsoBackend::VdsoBackend()
    : m_clock( new QtClock ), m_loader( new IVdso::Loader ), m_session() {
    m_session.reset( new IVdso::Session( *m_loader, *m_clock ) );
    m_path = findLibrary();
}


VdsoBackend::~VdsoBackend() {
    if ( m_session )
        m_session->close();
}


QString VdsoBackend::findLibrary() {
    QStringList names;
    for ( const std::string &n : IVdso::Loader::fileNameCandidates() )
        names << QString::fromStdString( n );

    QStringList dirs;
    // 1. Рядом с исполняемым файлом — самый предсказуемый способ: положил
    //    файл к программе, и он работает.
    dirs << QCoreApplication::applicationDirPath();
    // 2. Выбор оператора, сделанный раньше.
    const QString saved = QSettings().value( SETTINGS_KEY ).toString();
    if ( !saved.isEmpty() ) {
        const QFileInfo fi( saved );
        if ( fi.exists() )
            return fi.absoluteFilePath();
    }
    // 3. Обычные места, куда кладут SDK вендора.
#ifdef Q_OS_WIN
    for ( const QByteArray &var : { QByteArray( "ProgramFiles" ), QByteArray( "ProgramFiles(x86)" ) } ) {
        const QString base = QString::fromLocal8Bit( qgetenv( var.constData() ) );
        if ( base.isEmpty() )
            continue;
        dirs << base + QStringLiteral( "/InstruStar" ) << base + QStringLiteral( "/Vimu Electronic" );
    }
#endif

    for ( const QString &d : dirs )
        for ( const QString &n : names ) {
            const QFileInfo fi( QDir( d ), n );
            if ( fi.exists() )
                return fi.absoluteFilePath();
        }
    return QString();
}


void VdsoBackend::ensureOpen() {
    if ( m_tried )
        return;
    m_tried = true;

    if ( m_path.isEmpty() ) {
        m_error = QCoreApplication::translate(
            "VdsoBackend",
            "vdso.dll was not found. The vendor installer does not put it on disk — it comes with the "
            "vendor SDK, and a copy lives in this repository at "
            "references/vendor/instrustar-sdk/bin/vdso.dll. Put it next to the program, or point at it "
            "with the button below; the choice is remembered." );
        return;
    }
    if ( !m_loader->load( m_path.toStdString() ) ) {
        m_error = QString::fromStdString( m_loader->lastError() );
        return;
    }
    if ( !m_session->open() ) {
        m_error = QString::fromStdString( m_session->lastError() );
        return;
    }
    if ( !m_session->passport().hasDds )
        m_error = QCoreApplication::translate( "VdsoBackend",
                                               "The instrument (%1) has no generator: DDS exists on the B and X "
                                               "models only." )
                      .arg( QString::fromStdString( m_session->passport().model ) );
}


bool VdsoBackend::available() const {
    return m_session && m_session->state() != IVdso::Session::State::Closed && m_session->passport().hasDds;
}


QString VdsoBackend::unavailableReason() const {
    if ( !m_error.isEmpty() )
        return m_error;
    return QCoreApplication::translate( "VdsoBackend", "The instrument has not been opened yet." );
}


unsigned int VdsoBackend::waveformMask() const {
    return m_session ? unsigned( m_session->passport().ddsBoxingMask ) : 0u;
}


// Границы частоты вендор через API не отдаёт — геттера нет. Значения из
// таблицы подбора модели: 1 Гц…20 МГц, шаг 1 Гц (у 2062B 0,1 Гц, но эта
// модель в работе не используется).
unsigned int VdsoBackend::frequencyMinHz() const { return 1u; }
unsigned int VdsoBackend::frequencyMaxHz() const { return 20000000u; }


bool VdsoBackend::apply( const DdsRequest &request ) {
    if ( !available() ) {
        m_error = unavailableReason();
        return false;
    }
    IVdso::DdsSettings d;
    d.boxingStyle = request.waveform;
    d.frequencyHz = request.frequencyHz;
    d.dutyPercent = request.dutyPercent;
    d.output = request.output;
    // Размах и смещение не трогаем: на 205B и 210B они не реализованы,
    // органов у них в окне нет (`docs/ОТВЕТЫ-2026-09-05.md` §5).
    d.zoomResistance = -1;
    d.biasResistance = -1;
    if ( m_session->applyDds( d ) )
        return true;
    m_error = QString::fromStdString( m_session->lastError() );
    return false;
}


QString VdsoBackend::lastError() const { return m_error; }


bool VdsoBackend::needsLibraryPath() const { return m_path.isEmpty() || !m_loader->loaded(); }


bool VdsoBackend::setLibraryPath( const QString &path ) {
    const QFileInfo fi( path );
    if ( !fi.exists() ) {
        m_error = QCoreApplication::translate( "VdsoBackend", "No such file: %1" ).arg( path );
        return false;
    }
    m_path = fi.absoluteFilePath();
    m_error.clear();
    m_tried = false;
    if ( m_session )
        m_session->close();
    ensureOpen();
    if ( available() ) {
        // Запоминается только удачный выбор: неудачный запомнить значило бы
        // закрепить ошибку.
        QSettings().setValue( SETTINGS_KEY, m_path );
        return true;
    }
    return false;
}


QString VdsoBackend::libraryPath() const { return m_path; }
