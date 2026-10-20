#include "director.hpp"
#include "transport.hpp"
#include <algorithm>
#include <cmath>
namespace vrst::preview {
namespace {
void shrink(Warp &warp, double percent) {
    if (percent <= 0)
        return;
    auto &out = warp.output;
    const double cx = (out.left + out.right) * .5, cy = (out.up + out.down) * .5;
    const double keep = 1 - percent / 100;
    out = {cx + (out.left - cx) * keep, cx + (out.right - cx) * keep,
           cy + (out.up - cy) * keep, cy + (out.down - cy) * keep};
}
bool captured_corners(const Warp &warp) {
    for (double u : {0., 1.})
        for (double v : {0., 1.}) {
            const auto ray =
                rotate(warp.correction, {warp.output.left + u * (warp.output.right - warp.output.left),
                                         warp.output.up + v * (warp.output.down - warp.output.up), -1});
            const auto uv = map_uv(warp, u, v);
            if (!std::isfinite(ray.z) || ray.z >= -.05 || !std::isfinite(uv[0]) || !std::isfinite(uv[1]) ||
                uv[0] < 0 || uv[0] > 1 || uv[1] < 0 || uv[1] > 1)
                return false;
        }
    return true;
}
double required_crop(const Warp &warp, double maximum) {
    if (captured_corners(warp) || maximum <= 0)
        return 0;
    double low = 0, high = maximum;
    auto cropped = warp;
    shrink(cropped, maximum);
    if (!captured_corners(cropped))
        return maximum;
    for (unsigned i = 0; i < 28; ++i) {
        const double mid = (low + high) * .5;
        cropped = warp;
        shrink(cropped, mid);
        if (captured_corners(cropped))
            high = mid;
        else
            low = mid;
    }
    return high;
}
} // namespace
DirectorSettings sanitize_director_settings(DirectorSettings settings) {
    const auto safe = [](double value, double fallback, double low, double high) {
        return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
    };
    settings.lookahead_frames = std::round(safe(settings.lookahead_frames, 3, 1, 4));
    settings.auto_crop_max_percent = safe(settings.auto_crop_max_percent, 15, 0, 30);
    settings.auto_crop_release_ms = safe(settings.auto_crop_release_ms, 1200, 100, 5000);
    settings.speed_fov_percent = safe(settings.speed_fov_percent, 6, 0, 15);
    settings.speed_fov_full_kph = safe(settings.speed_fov_full_kph, 220, 30, 400);
    settings.speed_fov_response_ms = safe(settings.speed_fov_response_ms, 1000, 100, 5000);
    if (settings.telemetry_game > 2)
        settings.telemetry_game = 0;
    return settings;
}
Quat centered_orientation(const Frame *frames, std::size_t count) {
    if (!frames || !count)
        return {};
    count = std::min<std::size_t>(count, 9);
    Quat reference{}, sum{0, 0, 0, 0};
    bool have_reference = false;
    for (std::size_t i = 0; i < count; ++i) {
        const auto &v = frames[i].views[0];
        Quat q{v.orientation[0], v.orientation[1], v.orientation[2], v.orientation[3]};
        if (!frames[i].view_count || !valid(q))
            continue;
        q = normalized(q);
        if (!have_reference) {
            reference = q;
            have_reference = true;
        }
        const double sign = q.x * reference.x + q.y * reference.y + q.z * reference.z + q.w * reference.w < 0
                                ? -1.
                                : 1.;
        sum.x += sign * q.x;
        sum.y += sign * q.y;
        sum.z += sign * q.z;
        sum.w += sign * q.w;
    }
    return have_reference ? normalized(sum) : Quat{};
}
void DirectorController::update(Warp &warp, const DirectorSettings &raw, std::uint64_t time_ns,
                                 std::uint64_t sequence, double speed_kph, bool bypass) {
    const auto settings = sanitize_director_settings(raw);
    if (bypass || (!settings.auto_crop_enabled && !settings.speed_fov_enabled)) {
        reset();
        return;
    }
    const bool advance = !initialized_ || (sequence != previous_sequence_ && time_ns > previous_time_);
    const double dt = !initialized_ ? 1. / 60 : time_ns > previous_time_
                                                   ? std::min(.25, double(time_ns - previous_time_) / 1e9)
                                                   : 0.;
    status_.telemetry_game = settings.telemetry_game;
    if (!settings.speed_fov_enabled) {
        status_.speed_crop_percent = 0;
        status_.speed_available = false;
        status_.speed_kph = 0;
    } else {
        status_.speed_crop_percent = std::min(status_.speed_crop_percent, settings.speed_fov_percent);
        if (advance) {
            status_.speed_available = std::isfinite(speed_kph) && speed_kph >= 0 && speed_kph <= 1000;
            status_.speed_kph = status_.speed_available ? speed_kph : 0;
            const double target = status_.speed_available
                                      ? settings.speed_fov_percent *
                                            (1 - std::clamp(speed_kph / settings.speed_fov_full_kph, 0., 1.))
                                      : 0;
            status_.speed_crop_percent +=
                (target - status_.speed_crop_percent) * -std::expm1(-dt * 1000 / settings.speed_fov_response_ms);
        }
        // Existing speed reserve reduces the extra crop needed for correction.
        shrink(warp, status_.speed_crop_percent);
    }
    if (!settings.auto_crop_enabled)
        status_.auto_crop_percent = 0;
    else {
        status_.auto_crop_percent = std::min(status_.auto_crop_percent, settings.auto_crop_max_percent);
        if (advance) {
            const double required = required_crop(warp, settings.auto_crop_max_percent);
            if (required > status_.auto_crop_percent)
                status_.auto_crop_percent = required;
            else if (status_.auto_crop_percent - required > .05)
                status_.auto_crop_percent +=
                    (required - status_.auto_crop_percent) *
                    -std::expm1(-dt * 1000 / settings.auto_crop_release_ms);
        }
        shrink(warp, status_.auto_crop_percent);
    }
    if (advance) {
        previous_sequence_ = sequence;
        previous_time_ = time_ns;
        initialized_ = true;
    }
}
void DirectorController::reset() {
    status_ = {};
    previous_time_ = previous_sequence_ = 0;
    initialized_ = false;
}
void DirectorController::update_image(ImageWarp &warp, const DirectorSettings &settings,
                                      std::uint64_t time_ns, std::uint64_t sequence, double speed_kph,
                                      bool bypass, const ImageWarp *uncorrected) {
    // The axis-aligned bounds of an affine image have the same center and
    // shrink factor as its corners, allowing one shared controller/frame clock.
    const auto bounds = [](const std::array<double, 3> &row) {
        const double center = row[2] + .5 * (row[0] + row[1]);
        const double radius = .5 * (std::abs(row[0]) + std::abs(row[1]));
        return std::array<double, 2>{center - radius, center + radius};
    };
    const auto x = bounds(warp.row_x), y = bounds(warp.row_y);
    Warp proxy;
    proxy.output = {2 * x[0] - 1, 2 * x[1] - 1, 1 - 2 * y[0], 1 - 2 * y[1]};
    update(proxy, settings, time_ns, sequence, speed_kph, bypass);
    const double keep = (1 - status_.auto_crop_percent / 100) * (1 - status_.speed_crop_percent / 100);
    auto crop = [keep](ImageWarp &image) {
        if (keep == 1)
            return;
        for (auto *row : {&image.row_x, &image.row_y}) {
            (*row)[2] += .5 * ((*row)[0] + (*row)[1]) * (1 - keep);
            (*row)[0] *= keep;
            (*row)[1] *= keep;
        }
    };
    crop(warp);
    if (!settings.auto_crop_enabled || bypass)
        return;
    auto fits = [&](const ImageWarp &candidate) {
        const auto bx = bounds(candidate.row_x), by = bounds(candidate.row_y);
        return std::isfinite(bx[0]) && std::isfinite(bx[1]) && std::isfinite(by[0]) &&
               std::isfinite(by[1]) && bx[0] >= 0 && bx[1] <= 1 && by[0] >= 0 && by[1] <= 1;
    };
    if (fits(warp))
        return;
    auto neutral = uncorrected ? *uncorrected : ImageWarp{};
    crop(neutral);
    const auto desired = warp;
    const auto blend = [&](double fraction) {
        auto candidate = desired;
        for (unsigned i = 0; i < 3; ++i) {
            candidate.row_x[i] = neutral.row_x[i] + (desired.row_x[i] - neutral.row_x[i]) * fraction;
            candidate.row_y[i] = neutral.row_y[i] + (desired.row_y[i] - neutral.row_y[i]) * fraction;
        }
        candidate.correction_fraction = desired.correction_fraction * fraction;
        return candidate;
    };
    double low = 0, high = 1;
    for (unsigned i = 0; i < 28; ++i) {
        const double mid = (low + high) * .5;
        if (fits(blend(mid)))
            low = mid;
        else
            high = mid;
    }
    warp = blend(low);
}
} // namespace vrst::preview
