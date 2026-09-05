// SPDX-License-Identifier: GPL-2.0-or-later
// Last edited: 2026-08-11 11:31:17 UTC
//
// Configuration page for CtPU (Conversion to Physical Units), the math-stack,
// and XY multi-curve (TZ §12.1). The page is added as a fourth tab in the
// DsoConfigDialog. Одно окно без вкладок (правило GUI 2026-08-29): прежние
// вкладки стали озаглавленными разделами одной прокручиваемой страницы:
//   - CtPU tab:   per physical channel (CH1, CH2) — mode, k, b, unit, Zero/Span.
//   - Math tab:   M1..M4 — srcA, srcB, op, invert, enabled, unit, k, b.
//   - XY tab:     curve 0..3 — xChannel, yChannel, enabled.
//
// All controls write back to `settings->scope` in saveSettings(). The page is
// purely a UI shell — no validation beyond what the spinbox ranges enforce.
// Hot-path code reads the same `scope->voltage[]`, `scope->mathStack[]`, and
// `scope->xyCurves[]` vectors that this page edits.

#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QToolButton>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QScrollArea>
#include <QWidget>
#include <memory>
#include <vector>

struct DsoSettings;
class PPresult;

class DsoConfigCtpuMathPage : public QWidget {
    Q_OBJECT
  public:
    explicit DsoConfigCtpuMathPage( DsoSettings *settings, QWidget *parent = nullptr );
  public slots:
    void saveSettings();
    /// \brief Live update from the running acquisition (TZ discussion,
    /// CCtPU v1). Called from MainWindow::showNewData() while this page's
    /// dialog is open, so the "Live" column on the CtPU tab tracks the
    /// currently selected measurement source (Top/Base/Amplitude/Overshoot/
    /// per-cycle DC/std-dev/...) in real time, without averaging raw
    /// samples naively here — all the noise-robust work already happened in
    /// SpectrumGenerator::process().
    void updateLiveData( std::shared_ptr< PPresult > data );

  private:
    DsoSettings *settings;
    QScrollArea *scrollArea; ///< одно окно без вкладок (правило GUI 2026-08-29)
    QWidget *ctpuTab;
    QWidget *mathTab;
    QWidget *xyTab;

    // CtPU tab — per physical channel controls (size = 2 for CH1+CH2).
    struct CtpuChannelUI {
        QComboBox *modeCombo = nullptr;
        QLineEdit *unitEdit = nullptr;
        QDoubleSpinBox *kSpin = nullptr;
        QDoubleSpinBox *bSpin = nullptr;
        QDoubleSpinBox *zeroVSpin = nullptr;
        QDoubleSpinBox *spanVSpin = nullptr;
        QDoubleSpinBox *spanPhysSpin = nullptr;
        QPushButton *zeroButton = nullptr;
        QPushButton *spanButton = nullptr;
        QLabel *kLabel = nullptr;
        QLabel *bLabel = nullptr;
        // --- Live measurement source (CCtPU v1) ---
        QComboBox *sourceCombo = nullptr;   ///< which of the 12 measurements to show/capture
        QLabel *liveValueLabel = nullptr;   ///< live-updating readout of the selected measurement
        QPushButton *captureZeroButton = nullptr; ///< copies live value -> zeroVSpin (does NOT recompute k/b)
        QPushButton *captureSpanButton = nullptr; ///< copies live value -> spanVSpin (does NOT recompute k/b)
        double lastLiveValue = 0.0;         ///< last value shown, so capture buttons work without re-deriving it
        bool haveLiveValue = false;
    };
    std::vector< CtpuChannelUI > ctpuUI;

    // Math tab — per math channel controls (size = maxMathChannels = 4).
    struct MathChannelUI {
        QCheckBox *enabledCheck = nullptr;
        QSpinBox *srcASpin = nullptr;
        QSpinBox *srcBSpin = nullptr;
        QComboBox *opCombo = nullptr;
        QCheckBox *invertCheck = nullptr;
        QLineEdit *unitEdit = nullptr;
        QDoubleSpinBox *kSpin = nullptr;
        QDoubleSpinBox *bSpin = nullptr;
    };
    std::vector< MathChannelUI > mathUI;

    // XY tab — per curve controls (size = maxXYCurves = 4).
    struct XYCurveUI {
        QCheckBox *enabledCheck = nullptr;
        /// У3: выбор оси по ИМЕНИ канала (CH1/CH2/M1..M4) + «Time» —
        /// время как полноценный канал (timechannel.h). Числовой спинбокс
        /// заставлял оператора помнить, что 0=CH1 — мёртвая эргономика.
        QComboBox *xChannelCombo = nullptr;
        QComboBox *yChannelCombo = nullptr;
        QLabel *previewLabel = nullptr;
        /// Задание 4 очереди: цвет кривой назначается явно. Кнопка красится в
        /// действующий цвет, поэтому совпадение двух кривых видно сразу.
        QToolButton *colorButton = nullptr;
        QColor color;
    };
    std::vector< XYCurveUI > xyUI;

    void buildCtpuTab( unsigned realChannelCount );
    void buildMathTab();
    void buildXYTab();
    void updateCtpuLabels();
    void setCurveColorSwatch( int curve );
};
