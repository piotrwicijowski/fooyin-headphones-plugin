// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <core/track.h>

#include <QMetaType>

namespace Fooyin::Headphones {

enum class PreviewState
{
    Stopped,
    Loading,
    Buffering,
    Playing,
    Paused,
    Draining,
    Error,
};

struct PreviewSnapshot
{
    PreviewState state {PreviewState::Stopped};
    Track track;
    uint64_t generation {0};
    uint64_t positionMs {0};
    uint64_t durationMs {0}; // Zero means unknown.
    double volume {0.25}; // Linear stream gain; UI mapping belongs to Phase 3.
    bool deviceAvailable {false};
    bool seekable {false};
    bool pauseRequested {false};
    QString error;
};

} // namespace Fooyin::Headphones

Q_DECLARE_METATYPE(Fooyin::Headphones::PreviewSnapshot)
