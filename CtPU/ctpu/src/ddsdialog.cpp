// SPDX-License-Identifier: GPL-3.0-or-later
// Last edited: 2026-09-05 UTC

#include "ddsdialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QFileDialog>
#include <QVBoxLayout>


namespace {

// Формы сигнала. Имён вендор объявил ПЯТЬ, а маску поддержки отдаёт
// семибитную. Формы 5 и 6 не описаны нигде: ни в заголовке, ни в
// руководствах. Догадываться о них нельзя, а вот выбрать и измерить на
// выходе — можно, и это даст ответ без догадок. Поэтому они в списке
// стоят и названы честно.
struct WaveformName {
    unsigned int code;
    const char *title;
    const char *note;
};

const WaveformName WAVEFORMS[] = {
    { 0, QT_TRANSLATE_NOOP( "DdsDialog", "Sine" ), "" },
    { 1, QT_TRANSLATE_NOOP( "DdsDialog", "Square" ), "" },
    { 2, QT_TRANSLATE_NOOP( "DdsDialog", "Triangle" ), "" },
    { 3, QT_TRANSLATE_NOOP( "DdsDialog", "Sawtooth up" ), "" },
    { 4, QT_TRANSLATE_NOOP( "DdsDialog", "Sawtooth down" ), "" },
    { 5, QT_TRANSLATE_NOOP( "DdsDialog", "Form 5" ),
      QT_TRANSLATE_NOOP( "DdsDialog", "not documented — select it and measure the output" ) },
    { 6, QT_TRANSLATE_NOOP( "DdsDialog", "Form 6" ),
      QT_TRANSLATE_NOOP( "DdsDialog", "not documented — select it and measure the output" ) },
};

} // namespace


DdsDialog::DdsDialog( DdsBackend *backend, QWidget *parent ) : QDialog( parent ), m_backend( backend ) {
    setWindowTitle( tr( "DDS generator" ) );

    auto *layout = new QVBoxLayout( this );

    m_state = new QLabel( this );
    m_state->setWordWrap( true );
    m_state->setTextFormat( Qt::RichText );
    layout->addWidget( m_state );

    auto *box = new QGroupBox( tr( "Output signal" ), this );
    auto *form = new QFormLayout( box );

    m_waveform = new QComboBox( box );
    for ( const WaveformName &w : WAVEFORMS ) {
        QString title = tr( w.title );
        if ( *w.note )
            title += QStringLiteral( " — " ) + tr( w.note );
        m_waveform->addItem( title, w.code );
    }
    form->addRow( tr( "Waveform" ), m_waveform );

    // Целые герцы: вендорский SetDDSPinlv принимает unsigned int, дробных
    // не берёт. Показывать «Гц» с точкой значило бы обещать шаг, которого
    // у прибора нет.
    m_frequency = new QSpinBox( box );
    m_frequency->setRange( 1, 20000000 );
    m_frequency->setValue( 1000 );
    m_frequency->setSuffix( tr( " Hz" ) );
    m_frequency->setGroupSeparatorShown( true );
    m_frequency->setToolTip( tr( "Whole hertz only: the instrument takes an integer, "
                                 "step 1 Hz" ) );
    form->addRow( tr( "Frequency" ), m_frequency );

    m_duty = new QSpinBox( box );
    m_duty->setRange( 1, 99 );
    m_duty->setValue( 50 );
    m_duty->setSuffix( tr( " %" ) );
    form->addRow( tr( "Duty cycle" ), m_duty );

    m_output = new QCheckBox( tr( "Output enabled" ), box );
    form->addRow( QString(), m_output );

    layout->addWidget( box );

    // Ни «размаха», ни «смещения» здесь нет, и это сказано вслух: иначе
    // отсутствие органа читается как недоделка.
    auto *why = new QLabel(
        tr( "<p><i>There are deliberately no amplitude and no offset controls. "
            "The vendor selection table gives the generator exactly three "
            "parameters — DAC bits, frequency range and step; amplitude and "
            "offset are listed for no model. On ISDS205B and ISDS210B they are "
            "not implemented, and ISDS205X is not in use. A control that does "
            "nothing is worse than a missing one: it lies silently.</i></p>" ),
        this );
    why->setWordWrap( true );
    layout->addWidget( why );

    m_result = new QLabel( this );
    m_result->setWordWrap( true );
    layout->addWidget( m_result );

    auto *buttons = new QDialogButtonBox( QDialogButtonBox::Close, this );

    // Кнопка выбора библиотеки. Появляется только тогда, когда задней
    // стороне действительно не хватает файла — иначе она была бы шумом.
    m_pickLibrary = buttons->addButton( tr( "Locate vdso.dll .." ), QDialogButtonBox::ActionRole );
    m_pickLibrary->setVisible( false );
    connect( m_pickLibrary, &QPushButton::clicked, this, [ this ]() {
        if ( !m_backend )
            return;
        const QString file = QFileDialog::getOpenFileName( this, tr( "Locate the vendor library vdso.dll" ),
                                                           m_backend->libraryPath(),
                                                           tr( "Vendor library (vdso.dll VDSO.dll);;All files (*)" ) );
        if ( file.isEmpty() )
            return;
        if ( !m_backend->setLibraryPath( file ) )
            m_result->setText( tr( "<p><b>Not usable:</b> %1</p>" ).arg( m_backend->lastError().toHtmlEscaped() ) );
        else
            m_result->clear();
        refreshState();
    } );

    m_apply = buttons->addButton( tr( "Apply" ), QDialogButtonBox::ApplyRole );
    connect( m_apply, &QPushButton::clicked, this, &DdsDialog::applyRequest );
    connect( buttons, &QDialogButtonBox::rejected, this, &QDialog::reject );
    layout->addWidget( buttons );

    refreshState();
}


DdsRequest DdsDialog::currentRequest() const {
    DdsRequest r;
    r.waveform = m_waveform->currentData().toUInt();
    r.frequencyHz = unsigned( m_frequency->value() );
    r.dutyPercent = m_duty->value();
    r.output = m_output->isChecked();
    return r;
}


void DdsDialog::refreshState() {
    const bool ready = m_backend && m_backend->available();
    if ( m_pickLibrary )
        m_pickLibrary->setVisible( m_backend && !ready && m_backend->needsLibraryPath() );

    if ( !m_backend ) {
        m_state->setText(
            tr( "<h3>No generator available</h3>"
                "<p>This build has no path to a DDS generator at all.</p>"
                "<p><b>Path B</b> — fx2lafw + libusb, the path this application walks — "
                "carries no generator: neither sigrok nor its descendants support one. "
                "<b>Path A</b> — the vendor <tt>vdso.dll</tt> — does carry it (17 functions), "
                "and the connector for it is written and tested "
                "(<tt>ictpu/</tt>), but it is not yet joined to the application.</p>"
                "<p>The window exists so the controls are settled before the engine "
                "reaches them: the panel is the specification "
                "(<tt>docs/GUI-FIRST.md</tt>).</p>" ) );
    } else if ( !ready ) {
        m_state->setText( tr( "<h3>Generator not available now</h3><p>%1</p>" )
                              .arg( m_backend->unavailableReason().toHtmlEscaped() ) );
    } else {
        m_state->setText( tr( "<h3>Generator ready</h3><p>Library: <tt>%1</tt></p>" )
                              .arg( m_backend->libraryPath().toHtmlEscaped() ) );
    }

    // Границы частоты берутся у прибора, когда он есть: ряд у моделей
    // разный (0,1 Гц у 2062B против 1 Гц у остальных).
    if ( ready ) {
        const unsigned lo = m_backend->frequencyMinHz();
        const unsigned hi = m_backend->frequencyMaxHz();
        if ( lo && hi && hi >= lo )
            m_frequency->setRange( int( lo ), int( hi ) );

        // Форма, которой прибор не объявляет, выключается, а не прячется:
        // спрятанный орган не объясняет, почему его нет.
        const unsigned mask = m_backend->waveformMask();
        if ( mask ) {
            for ( int i = 0; i < m_waveform->count(); ++i ) {
                const unsigned code = m_waveform->itemData( i ).toUInt();
                const bool supported = ( mask & ( 1u << code ) ) != 0;
                m_waveform->setItemData( i, supported ? QVariant() : QVariant( 0 ), Qt::UserRole - 1 );
            }
        }
    }

    m_waveform->setEnabled( ready );
    m_frequency->setEnabled( ready );
    m_duty->setEnabled( ready );
    m_output->setEnabled( ready );
    m_apply->setEnabled( ready );
}


void DdsDialog::applyRequest() {
    if ( !m_backend )
        return;
    if ( m_backend->apply( currentRequest() ) ) {
        m_result->setText( tr( "<p><b>Applied.</b></p>" ) );
    } else {
        // Отказ называется словами: у сеттеров вендора возврата нет, и
        // единственная проверка — чтение назад, которую делает движок.
        m_result->setText( tr( "<p><b>Not applied:</b> %1</p>" ).arg( m_backend->lastError().toHtmlEscaped() ) );
    }
    refreshState();
}
