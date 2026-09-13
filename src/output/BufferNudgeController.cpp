#include "BufferNudgeController.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double Pi = 3.1415926535897932384626433832795;
constexpr double TwoPi = 2.0 * Pi;

double acquisitionPpmForError(double absErrorQ)
{
    // Position-only actuator ladder expressed in ASIO quanta.
    // At the original 64-frame / 48 kHz Q these are exactly the old
    // 12 / 6 / 3 / 1.5 / 0.60 ms thresholds.
    if (absErrorQ > 9.0)
        return 8000.0;
    if (absErrorQ > 4.5)
        return 5000.0;
    if (absErrorQ > 2.25)
        return 2500.0;
    if (absErrorQ > 1.125)
        return 1000.0;
    if (absErrorQ > 0.45)
        return 400.0;
    return 0.0;
}

double maintenancePpmForError(double absErrorQ)
{
    // Deliberately calm once settled; thresholds are 3Q and 1.5Q.
    if (absErrorQ > 3.0)
        return 1000.0;
    if (absErrorQ > 1.5)
        return 500.0;
    return 250.0;
}
}

void BufferNudgeController::reset()
{
    averageMarginMs_ = 0.0;
    averageMarginValid_ = false;

    targetValid_ = false;
    lastTargetMarginMs_ = 0.0;

    acquisitionActive_ = true;
    acquireInsideSec_ = 0.0;

    maintenanceOutsideSec_ = 0.0;
    maintenanceOutsideDirection_ = 0;
    maintenanceCooldownSec_ = 0.0;

    nudgePpm_ = 0.0;
    nudgeRemainingMs_ = 0.0;
}


void BufferNudgeController::forceAcquisition()
{
    acquisitionActive_ = true;
    acquireInsideSec_ = 0.0;
    maintenanceOutsideSec_ = 0.0;
    maintenanceOutsideDirection_ = 0;
    maintenanceCooldownSec_ = 0.0;
}

void BufferNudgeController::update(
    double completionMarginMs,
    double targetMarginMs,
    double sampleDtSec,
    double asioBufferPeriodMs)
{
    if (!std::isfinite(completionMarginMs) ||
        !std::isfinite(targetMarginMs) ||
        !std::isfinite(sampleDtSec) ||
        !std::isfinite(asioBufferPeriodMs) ||
        asioBufferPeriodMs <= 0.0)
    {
        return;
    }

    const double dt = std::clamp(sampleDtSec, 0.001, 0.250);

    const bool firstTarget = !targetValid_;
    const bool targetChanged =
        targetValid_ && std::abs(targetMarginMs - lastTargetMarginMs_) > 0.01;

    if (firstTarget || targetChanged)
    {
        targetValid_ = true;
        lastTargetMarginMs_ = targetMarginMs;

        // A deliberate buffer-size change is a fresh acquisition. Re-seed the
        // cheap margin average from NOW so old target history cannot slow it.
        averageMarginMs_ = completionMarginMs;
        averageMarginValid_ = true;

        acquisitionActive_ = true;
        acquireInsideSec_ = 0.0;
        maintenanceOutsideSec_ = 0.0;
        maintenanceOutsideDirection_ = 0;
        maintenanceCooldownSec_ = 0.0;
        nudgePpm_ = 0.0;
        nudgeRemainingMs_ = 0.0;
    }
    else if (!averageMarginValid_)
    {
        averageMarginMs_ = completionMarginMs;
        averageMarginValid_ = true;
    }
    else
    {
        const double alpha =
            1.0 - std::exp(-TwoPi * BufferAverageHz * dt);
        averageMarginMs_ +=
            alpha * (completionMarginMs - averageMarginMs_);
    }

    const double errorMs = averageMarginMs_ - targetMarginMs;
    const double absErrorMs = std::abs(errorMs);
    const double errorQ = errorMs / asioBufferPeriodMs;
    const double absErrorQ = std::abs(errorQ);
    const double acquireDoneBandMs = AcquireDoneBandQ * asioBufferPeriodMs;
    const double maintenanceDeadbandMs = MaintenanceDeadbandQ * asioBufferPeriodMs;

    // Positive margin error means too much buffer. Positive ASRC correction
    // drains it faster, so the actuator sign follows errorMs.
    if (acquisitionActive_)
    {
        if (absErrorMs <= acquireDoneBandMs)
        {
            // 0.5.00: hand over immediately once acquisition reaches the
            // target band. The old 0.75 s hold could keep a bad-clock source
            // stuck in acquisition forever, so maintenance/micro nudges never ran.
            nudgePpm_ = 0.0;
            nudgeRemainingMs_ = 0.0;
            acquisitionActive_ = false;
            acquireInsideSec_ = 0.0;
            maintenanceCooldownSec_ = MaintenanceCooldownSec;
            maintenanceOutsideSec_ = 0.0;
            maintenanceOutsideDirection_ = 0;
            return;
        }

        acquireInsideSec_ = 0.0;
        const double ppm = acquisitionPpmForError(absErrorQ);
        nudgePpm_ = std::copysign(ppm, errorMs);
        nudgeRemainingMs_ = errorMs;
        return;
    }

    // Settled mode: sit still unless an error remains convincingly outside
    // the band. No continuous chasing of individual callback wiggles.
    if (maintenanceCooldownSec_ > 0.0)
    {
        maintenanceCooldownSec_ =
            std::max(0.0, maintenanceCooldownSec_ - dt);
        nudgePpm_ = 0.0;
        nudgeRemainingMs_ = 0.0;
        return;
    }

    if (absErrorMs <= maintenanceDeadbandMs)
    {
        const bool wasCorrecting = nudgePpm_ != 0.0;
        maintenanceOutsideSec_ = 0.0;
        maintenanceOutsideDirection_ = 0;
        nudgePpm_ = 0.0;
        nudgeRemainingMs_ = 0.0;
        if (wasCorrecting)
            maintenanceCooldownSec_ = MaintenanceCooldownSec;
        return;
    }

    const int direction = errorMs > 0.0 ? 1 : -1;
    if (maintenanceOutsideDirection_ != direction)
    {
        maintenanceOutsideDirection_ = direction;
        maintenanceOutsideSec_ = 0.0;
    }

    maintenanceOutsideSec_ += dt;
    if (maintenanceOutsideSec_ < MaintenancePersistenceSec)
    {
        nudgePpm_ = 0.0;
        nudgeRemainingMs_ = errorMs;
        return;
    }

    // A confirmed maintenance correction stays position-only and modest.
    const double ppm = maintenancePpmForError(absErrorQ);
    nudgePpm_ = std::copysign(ppm, errorMs);
    nudgeRemainingMs_ = errorMs;

    // Once it rolls back into the deadband the branch above stops the nudge
    // and the next excursion again has to survive the full persistence timer.
}
