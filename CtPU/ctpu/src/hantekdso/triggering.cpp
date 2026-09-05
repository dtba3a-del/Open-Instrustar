// SPDX-License-Identifier: GPL-2.0-or-later
// Last edited: 2026-08-11 11:31:17 UTC

#include "triggering.h"

#include "autotrigger.h"
#include "hantekdsocontrol.h"
#include "movingaverage.h"
#include <QDebug>
#include <cmath>


Triggering::Triggering( const DsoSettingsScope *scope, const Dso::ControlSettings &controlsettings )
    : scope( scope ), controlsettings( controlsettings ) {
    if ( scope->verboseLevel > 1 )
        qDebug() << " Triggering::Triggering()";
}


// search for trigger point from defined point, default startPos = 0;
// return trigger position > 0 (0: no trigger found)
// [MOD] Step 2 of the chat-confirmed plan: `smoothed`/`smoothOffset` locate
// the level-CROSSING on a lightly pre-averaged copy of the same channel's
// samples (smoothed[j] == samples[j + smoothOffset]), so a single noisy
// sample sitting right at the trigger level can't flip which side of the
// level the crossing check sees. This is IN ADDITION TO the existing
// triggerAverage before/after confirmation below (unchanged, still reads
// the RAW `samples`) -- that confirms a candidate crossing is real once
// found; this reduces how often a lone noise sample creates a spurious
// candidate in the first place. Computed once per searchTriggeredPosition()
// call by the caller and passed in, instead of recomputing it on each of
// the up-to-3 calls this function gets per frame.
// At the default preTriggerSmoothN=1 (see searchTriggeredPosition()),
// MovingAverage::apply() is an identity fast-path, smoothOffset==0, and
// this function's behaviour is byte-for-byte identical to before this
// change -- so bumping preTriggerSmoothN back to 1 is a full, safe revert
// without touching this function again.
int Triggering::searchTriggerPoint( DSOsamples &result, Dso::Slope dsoSlope, int startPos,
                                     const std::vector< double > &smoothed, unsigned smoothOffset ) {
    int slope;
    if ( dsoSlope == Dso::Slope::Positive )
        slope = 1;
    else if ( dsoSlope == Dso::Slope::Negative )
        slope = -1;
    else
        return 0;

    unsigned channel = unsigned( controlsettings.trigger.source );
    const std::vector< double > &samples = result.data[ channel ];
    int sampleCount = int( samples.size() ); ///< number of available samples
    if ( startPos < 0 || startPos >= sampleCount )
        return 0;
    double triggerLevel = controlsettings.trigger.level[ channel ];
    // Задание 9: автоматика уровня по гистограмме — своя у каждого канала.
    // Моды гистограммы говорят, ГДЕ состояния сигнала; порог ставится между
    // ними, потому что в самой моде сигнал стоит, а не пересекает
    // (`autotrigger.h`). Уровень не назначается, если пересечения не бывает.
    if ( channel < scope->voltage.size() && scope->voltage[ channel ].triggerAuto ) {
        const auto autoLevel = AutoTrigger::level( samples );
        if ( autoLevel.valid )
            triggerLevel = autoLevel.value;
    }
    if ( scope->verboseLevel > 5 )
        qDebug() << "     Triggering::searchTriggerPoint()" << channel << triggerLevel << slope << startPos;
    int samplesDisplay = int( round( controlsettings.samplerate.target.duration * controlsettings.samplerate.current ) );
    int searchBegin;
    int searchEnd;
    if ( 0 == startPos ) {                                                      // search 1st trigger slope
        searchBegin = int( controlsettings.trigger.position * samplesDisplay ); // samples left of trigger
        searchEnd = sampleCount - ( int( samplesDisplay ) - searchBegin );      // samples right of trigger
    } else {                                                                    // search next slopes for duty cycle
        searchBegin = startPos;                                                 // search from start point ..
        searchEnd = sampleCount;                                                // .. up to end of samples
    }
    // Two possible search scenarios:
    // 1. search for the trigger slope that allows stable trace display (omit pre and post trigger area)
    // |-----------samples-----------| // available sample
    // |--disp--|                      // display size
    // |<<<<<T>>|--------------------| // >> = right = (disp-pre) i.e. right of trigger on screen
    // |<pre<|                         // << = left = pre
    // |--(samp-(disp-pre))-------|>>|
    // |<<<<<|????????????????????|>>| // ?? = search for trigger in this range [left,right]
    // 2. search duty cycle slopes without need for stable display margins
    // |<<<<<T???????????????????????| // ?? = search for other (duty cycle) slopes in this range

    const int triggerAverage = int( pow( 20, controlsettings.trigger.smooth ) ); // smooth 0,1,2 -> 1,20,400
    if ( searchBegin < triggerAverage )
        searchBegin = triggerAverage;
    if ( searchEnd >= sampleCount - triggerAverage )
        searchEnd = sampleCount - triggerAverage - 1;
    // Safety clamp: smoothed[] only has valid data for raw indices in
    // [smoothOffset, smoothOffset+smoothed.size()). In normal operation
    // smoothOffset is tiny (margin of a small pre-trigger window) and
    // triggerAverage is always >= 1, so this is a no-op in practice --
    // kept explicit so a future change to either constant can't read out
    // of bounds silently.
    if ( searchBegin < int( smoothOffset ) )
        searchBegin = int( smoothOffset );
    if ( searchEnd >= int( smoothOffset + smoothed.size() ) )
        searchEnd = int( smoothOffset + smoothed.size() ) - 1;
    if ( scope->verboseLevel > 5 )
        qDebug() << "     begin:" << searchBegin << "end:" << searchEnd;

    auto smoothedAt = [ &smoothed, smoothOffset ]( int rawIndex ) -> double {
        return smoothed[ size_t( rawIndex ) - smoothOffset ];
    };

    double prev = INT_MAX;
    int swTriggerStart = 0;
    for ( int i = searchBegin; i < searchEnd; i++ ) {
        double cur = smoothedAt( i );
        if ( slope * cur >= slope * triggerLevel && slope * prev < slope * triggerLevel ) { // trigger condition met
            // check for the previous few SampleSet samples, if they are also above/below the trigger value
            // use different averaging sizes for HF, normal and LF signals
            // [unchanged] confirmation still reads the RAW samples, not the smoothed copy --
            // it's a different, already-tuned (via the "smooth" UI setting) mechanism.
            bool triggerBefore = false;
            double mean = 0;
            int iii = 0;
            for ( int k = i - 1; k >= i - triggerAverage && k >= 0; k-- ) {
                mean += samples[ size_t( k ) ];
                iii++;
            }
            if ( iii ) {
                mean /= iii;
                triggerBefore = slope * mean < slope * triggerLevel;
            }
            // check for the next few SampleSet samples, if they are also above/below the trigger value
            bool triggerAfter = false;
            if ( triggerBefore ) { // search right side only if left side condition is met
                mean = 0;
                iii = 0;
                for ( int k = i + 1; k <= i + triggerAverage && k < sampleCount; k++ ) {
                    mean += samples[ size_t( k ) ];
                    iii++;
                }
                if ( iii ) {
                    mean /= iii;
                    triggerAfter = slope * mean > slope * triggerLevel;
                }
            }
            // if at least triggerAverage samples before and after trig meet the condition, set trigger
            if ( triggerBefore && triggerAfter ) {
                swTriggerStart = i;
                break;
            }
        }
        prev = cur;
    }
    if ( scope->verboseLevel > 5 )
        qDebug() << "     swT:" << swTriggerStart;
    return swTriggerStart;
} // Triggering::searchTriggerPoint()


int Triggering::searchTriggeredPosition( DSOsamples &result ) {
    static Dso::Slope nextSlope = Dso::Slope::Positive; // for alternating slope mode X
    ChannelID channel = ChannelID( controlsettings.trigger.source );
    // Задание 9 очереди: в режиме FREERUN уровень НЕ ищется вовсе. Поиск —
    // самая дорогая часть кадра, и именно он сдерживал поток; отдавать данные
    // подряд как пришли и означает его не выполнять.
    if ( controlsettings.trigger.mode == Dso::TriggerMode::FREERUN )
        return result.triggeredPosition = 0;
    // Trigger channel not in use
    if ( !scope->anyUsed( channel ) || result.data.empty() || result.data[ channel ].empty() )
        return result.triggeredPosition = 0;
    if ( scope->verboseLevel > 4 )
        qDebug() << "    Triggering::searchTriggeredPosition()" << result.tag;
    triggeredPositionRaw = 0;
    double pulseWidth1 = 0.0;
    double pulseWidth2 = 0.0;

    size_t sampleCount = result.data[ channel ].size();              // number of available samples
    double timeDisplay = controlsettings.samplerate.target.duration; // time for full screen width
    double sampleRate = result.samplerate;                           //
    unsigned samplesDisplay = unsigned( round( timeDisplay * controlsettings.samplerate.current ) );
    if ( sampleCount < samplesDisplay ) // not enough samples to adjust for jitter.
        return result.triggeredPosition = 0;
    // search for trigger point in a range that leaves enough samples left and right of trigger for display
    // find also up to two alternating slopes after trigger point -> calculate pulse widths and duty cycle.
    if ( controlsettings.trigger.slope != Dso::Slope::Both ) // up or down
        nextSlope = controlsettings.trigger.slope;           // use this slope

    // [MOD] Step 2 of the chat-confirmed plan: pre-average once per frame,
    // reused across all 3 searchTriggerPoint() calls below instead of
    // recomputing it 3x. preTriggerSmoothN=1 is an identity fast-path in
    // MovingAverage::apply() -- i.e. this is currently a conservative,
    // easily-reverted default, NOT yet tuned against real noisy hardware
    // (no ISDS205/INA333 signal available in this environment to tune
    // against -- see chat). Raise it once real capture data is available.
    static const unsigned preTriggerSmoothN = 1; // placeholder: 1 == disabled (byte-identical to pre-Step-2 behaviour)
    const std::vector< double > smoothedTrigger = MovingAverage::apply( result.data[ channel ], preTriggerSmoothN );
    const unsigned smoothOffset = MovingAverage::margin( preTriggerSmoothN );

    triggeredPositionRaw = searchTriggerPoint( result, nextSlope, 0, smoothedTrigger, smoothOffset ); // get 1st slope position
    if ( triggeredPositionRaw ) { // triggered -> search also following other slope (calculate pulse width)
        if ( int slopePos2 = searchTriggerPoint( result, mirrorSlope( nextSlope ), triggeredPositionRaw, smoothedTrigger,
                                                  smoothOffset ) ) {
            pulseWidth1 = ( slopePos2 - triggeredPositionRaw ) / sampleRate;
            if ( int slopePos3 =
                     searchTriggerPoint( result, nextSlope, slopePos2, smoothedTrigger, smoothOffset ) ) { // search 3rd slope
                pulseWidth2 = ( slopePos3 - slopePos2 ) / sampleRate;
            }
        }
        if ( controlsettings.trigger.slope == Dso::Slope::Both ) // trigger found and alternating?
            nextSlope = mirrorSlope( nextSlope );                // use opposite direction next time
    }

    result.triggeredPosition = triggeredPositionRaw; // align trace to trigger position
    result.pulseWidth1 = pulseWidth1;
    result.pulseWidth2 = pulseWidth2;
    if ( scope->verboseLevel > 5 ) // HACK: This assumes that positive=0 and negative=1
        qDebug() << "     nextSlope:"
                 << "/\\"[ int( nextSlope ) ] << "triggeredPositionRaw:" << triggeredPositionRaw;
    return result.triggeredPosition;
} // Triggering::searchTriggeredPosition()


bool Triggering::provideTriggeredData( DSOsamples &result ) {
    if ( scope->verboseLevel > 4 )
        qDebug() << "    Triggering::provideTriggeredData()" << result.tag;
    static DSOsamples triggeredResult; // storage for last triggered trace samples
    if ( result.triggeredPosition ) {  // live trace has triggered
        // Use this trace and save it also
        triggeredResult.data = result.data;
        triggeredResult.samplerate = result.samplerate;
        triggeredResult.clipped = result.clipped;
        triggeredResult.triggeredPosition = result.triggeredPosition;
        result.liveTrigger = true;
    } else if ( controlsettings.trigger.mode == Dso::TriggerMode::NORMAL ) { // Not triggered in NORMAL mode
        // Use saved trace (even if it is empty)
        result.data = triggeredResult.data;
        result.samplerate = triggeredResult.samplerate;
        result.clipped = triggeredResult.clipped;
        result.triggeredPosition = triggeredResult.triggeredPosition;
        result.liveTrigger = false; // show red "TR" top left
    } else {                        // Not triggered and not NORMAL mode
        // Use the free running trace, discard history
        triggeredResult.data.clear();          // discard trace
        triggeredResult.triggeredPosition = 0; // not triggered
        result.liveTrigger = false;            // show red "TR" top left
    }
    return result.liveTrigger;
} // bool Triggering::provideTriggeredData()
