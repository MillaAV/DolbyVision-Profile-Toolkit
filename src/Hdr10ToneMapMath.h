#pragma once

#include <algorithm>
#include <cmath>

namespace Hdr10ToneMap {

struct Source {
    double masteringPeak = 0.0;
    double maxCLL = 0.0;
    double maxFALL = 0.0;
};

struct Display {
    double peak = 0.0;
    double fullFramePeak = 0.0;
};

struct Plan {
    bool valid = false;
    bool compression = false;
    double sourcePeak = 0.0;
    double displayPeak = 0.0;
    double knee = 0.0;
};

inline double PqToNits(double pq) {
    constexpr double m1 = 2610.0 / 16384.0;
    constexpr double m2 = 2523.0 / 32.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = 2413.0 / 128.0;
    constexpr double c3 = 2392.0 / 128.0;
    if (!std::isfinite(pq)) return 0.0;
    pq = std::clamp(pq, 0.0, 1.0);
    const double p = std::pow(pq, 1.0 / m2);
    const double denominator = c2 - c3 * p;
    if (denominator <= 0.0) return 10000.0;
    return std::clamp(10000.0 * std::pow(std::max(p - c1, 0.0) / denominator, 1.0 / m1), 0.0, 10000.0);
}

inline double NitsToPq(double nits) {
    constexpr double m1 = 2610.0 / 16384.0;
    constexpr double m2 = 2523.0 / 32.0;
    constexpr double c1 = 3424.0 / 4096.0;
    constexpr double c2 = 2413.0 / 128.0;
    constexpr double c3 = 2392.0 / 128.0;
    if (!std::isfinite(nits)) return 0.0;
    const double p = std::pow(std::clamp(nits, 0.0, 10000.0) / 10000.0, m1);
    return std::clamp(std::pow((c1 + c2 * p) / (1.0 + c3 * p), m2), 0.0, 1.0);
}

inline Plan MakePlan(const Source& source, const Display& display) {
    Plan plan{};
    if (!std::isfinite(display.peak) || display.peak < 80.0 || display.peak > 10000.0) return plan;
    const double masteringPeak = std::isfinite(source.masteringPeak) && source.masteringPeak > 0.0 ? source.masteringPeak : 0.0;
    const double contentPeak = std::isfinite(source.maxCLL) && source.maxCLL > 0.0 ? source.maxCLL : 0.0;
    if (masteringPeak == 0.0 && contentPeak == 0.0) return plan;
    plan.sourcePeak = std::clamp(std::max(masteringPeak, contentPeak), 1.0, 10000.0);
    plan.displayPeak = display.peak;
    plan.compression = plan.sourcePeak > plan.displayPeak;
    const double ratio = std::clamp(plan.displayPeak / plan.sourcePeak, 0.0, 1.0);
    plan.knee = plan.compression ? plan.displayPeak * std::clamp(0.75 * ratio, 0.20, 0.75) : plan.displayPeak;
    plan.valid = true;
    return plan;
}

inline double MapNits(double nits, const Plan& plan) {
    if (!std::isfinite(nits)) return 0.0;
    nits = std::max(nits, 0.0);
    if (!plan.valid || !plan.compression) return nits;
    if (nits <= plan.knee) return nits;
    if (nits >= plan.sourcePeak) return plan.displayPeak;
    const double rolloff = plan.displayPeak - plan.knee;
    const double available = plan.sourcePeak - plan.knee;
    if (rolloff <= 0.0 || available <= rolloff) return std::min(nits, plan.displayPeak);
    const double curvature = (available - rolloff) / (available * rolloff);
    const double distance = nits - plan.knee;
    return plan.knee + distance / (1.0 + curvature * distance);
}

struct Rgb {
    double r = 0.0;
    double g = 0.0;
    double b = 0.0;
};

inline Rgb MapLinearBt2020Rgb(const Rgb& rgb, const Plan& plan) {
    if (!plan.valid || !plan.compression) return rgb;
    const double y = std::max(0.0, 0.2627 * rgb.r + 0.6780 * rgb.g + 0.0593 * rgb.b);
    if (y <= 0.0) return rgb;
    const double scale = MapNits(y, plan) / y;
    return {rgb.r * scale, rgb.g * scale, rgb.b * scale};
}

}
