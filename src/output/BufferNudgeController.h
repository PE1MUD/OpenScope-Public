#pragma once

class BufferNudgeController
{
public:
    void reset();

    // Compatibility with older AsioCapture startup code. 0.4.96 has no
    // separately enabled/disabled controller state; reset() is sufficient.
    // 0.4.96 SIMPLE POSITION SERVO.
    //
    // Input is the cheap/proven READY -> BMD frame-completion margin.
    // No slope estimate, no PID, no learned/permanent ppm correction.
    //
    // Acquisition:
    //   - entered after start/reset/target change
    //   - moves quickly toward target with temporary ASRC nudges
    //
    // Settled maintenance:
    //   - wide deadband
    //   - requires a persistent error before moving
    //   - small temporary correction and cooldown
    //
    // PPM here is only the temporary actuator used to move buffer POSITION.
    // It is never interpreted as a measured clock error and never learned.
    void forceAcquisition();

    void update(
        double completionMarginMs,
        double targetMarginMs,
        double sampleDtSec,
        double asioBufferPeriodMs);

    double appliedPpm() const { return nudgePpm_; }
    double nudgePpm() const { return nudgePpm_; }
    double averageMarginMs() const { return averageMarginMs_; }
    bool marginValid() const { return averageMarginValid_; }
    bool nudgeActive() const { return nudgePpm_ != 0.0; }
    double nudgeRemainingMs() const { return nudgeRemainingMs_; }

    bool acquisitionActive() const { return acquisitionActive_; }

    bool maintenanceHoldActive() const
    {
        return !acquisitionActive_ && maintenanceCooldownSec_ > 0.0;
    }

    bool maintenanceTriggerTiming() const
    {
        return !acquisitionActive_ &&
               maintenanceCooldownSec_ <= 0.0 &&
               maintenanceOutsideSec_ > 0.0;
    }

    // Intentionally disabled in 0.4.96.
private:
    static constexpr double BufferAverageHz = 0.35;

    // All position thresholds are expressed in ASIO quanta (Q), not fixed ms.
    // The multipliers preserve the proven 64-frame / 48 kHz tuning exactly:
    // Q = 1.333 ms -> 0.60, 1.00, 1.50, 2, 3, 4, 6 and 12 ms.
    static constexpr double AcquireDoneBandQ = 0.45;
    static constexpr double MaintenanceDeadbandQ = 0.75;
    static constexpr double MaintenancePersistenceSec = 5.0;
    static constexpr double MaintenanceCooldownSec = 10.0;

    double averageMarginMs_ = 0.0;
    bool averageMarginValid_ = false;

    bool targetValid_ = false;
    double lastTargetMarginMs_ = 0.0;

    bool acquisitionActive_ = true;
    double acquireInsideSec_ = 0.0;

    double maintenanceOutsideSec_ = 0.0;
    int maintenanceOutsideDirection_ = 0;
    double maintenanceCooldownSec_ = 0.0;

    double nudgePpm_ = 0.0;
    double nudgeRemainingMs_ = 0.0;
};
