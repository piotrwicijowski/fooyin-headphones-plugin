// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <core/track.h>

namespace Fooyin {
class SettingsManager;
}

namespace Fooyin::Headphones {

// Transient, session-owned snapshot. Configuration belongs exclusively to fooyin.
struct PreviewReplayGain
{
    bool applyGain {false};
    bool preventClipping {false};
    bool preferAlbum {false};
    double preampDb {0.0};
    double withoutInfoPreampDb {0.0};

    static PreviewReplayGain capture(const SettingsManager* settings);
    [[nodiscard]] double linearGain(const Track& track) const;
};

} // namespace Fooyin::Headphones
