// SPDX-License-Identifier: GPL-3.0-or-later
#include "previewreplaygain.h"

#include <core/coresettings.h>
#include <core/engine/enginedefs.h>
#include <core/playlist/playlist.h>
#include <utils/settings/settingsmanager.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace Fooyin::Headphones {
namespace {
double preamp(float value)
{
    // Same supported range as fooyin's ReplayGain page; never repair persisted settings.
    return std::isfinite(value) ? std::clamp<double>(value, -20.0, 20.0) : 0.0;
}

std::optional<double> validValue(bool present, float value, bool peak = false)
{
    if(present && std::isfinite(value) && (!peak || value > 0.0F)) {
        return value;
    }
    return {};
}
} // namespace

PreviewReplayGain PreviewReplayGain::capture(const SettingsManager* settings)
{
    PreviewReplayGain policy;
    if(!settings) {
        return policy;
    }
    const int mode = settings->value<Settings::Core::RGMode>();
    if(mode < Engine::NoProcessing || mode > static_cast<int>(Engine::ApplyGain | Engine::PreventClipping)) {
        return policy;
    }
    policy.applyGain       = (mode & Engine::ApplyGain) != 0;
    policy.preventClipping = (mode & Engine::PreventClipping) != 0;
    // ReplayGainType is private to fooyin. These are its persisted RGType values:
    // Track = 0, Album = 1, PlaybackOrder = 2. Use public settings/playlist APIs only.
    const int type     = settings->value<Settings::Core::RGType>();
    policy.preferAlbum = type == 1
                      || (type == 2 && (settings->value<Settings::Core::PlayMode>() & Playlist::ShuffleTracks) == 0);
    policy.preampDb            = preamp(settings->value<Settings::Core::RGPreAmp>());
    policy.withoutInfoPreampDb = preamp(settings->value<Settings::Core::NonRGPreAmp>());
    return policy;
}

double PreviewReplayGain::linearGain(const Track& track) const
{
    if(!applyGain && !preventClipping) {
        return 1.0;
    }
    auto gain         = validValue(track.hasTrackGain(), track.rgTrackGain());
    auto fallbackGain = validValue(track.hasAlbumGain(), track.rgAlbumGain());
    auto peak         = validValue(track.hasTrackPeak(), track.rgTrackPeak(), true);
    auto fallbackPeak = validValue(track.hasAlbumPeak(), track.rgAlbumPeak(), true);
    if(preferAlbum) {
        std::swap(gain, fallbackGain);
        std::swap(peak, fallbackPeak);
    }
    if(!gain) {
        gain = fallbackGain;
    }
    if(!peak) {
        peak = fallbackPeak;
    }
    // Match fooyin's processor: raw RG getters, not effectiveRG* (which include
    // Opus header gain). Decoder-defined source gain is already part of the PCM.
    const double db = applyGain ? gain.value_or(0.0) + (gain ? preampDb : withoutInfoPreampDb) : 0.0;
    double linear = std::pow(10.0, db / 20.0);
    if(preventClipping && peak && linear > 1.0 / *peak) {
        linear = 1.0 / *peak;
    }
    // Malformed/extreme metadata must not introduce infinities into float PCM.
    return std::isfinite(linear) && linear <= std::numeric_limits<float>::max() ? linear : 1.0;
}

} // namespace Fooyin::Headphones
