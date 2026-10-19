// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonesvariableprovider.h"

#include "previewcontroller.h"

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {

namespace {
Player::PlayState scriptState(const PreviewSnapshot& snapshot)
{
    if(snapshot.state == PreviewState::Stopped || snapshot.state == PreviewState::Error) {
        return Player::PlayState::Stopped;
    }
    return snapshot.pauseRequested ? Player::PlayState::Paused : Player::PlayState::Playing;
}

QString albumArtist(const Track& track)
{
    return track.effectiveAlbumArtist();
}
} // namespace

HeadphonesVariableProvider::HeadphonesVariableProvider(PreviewController& controller)
    : m_snapshot {std::make_shared<const Snapshot>()}
    , m_variables {
          makeScriptVariableDescriptor<&HeadphonesVariableProvider::metadataVariable<albumArtist>>(
              this, VariableKind::Generic, u"HEADPHONES_ALBUMARTIST"_s, u"Headphones Preview"_s,
              u"Album artist of the retained headphone preview track"_s),
          makeScriptVariableDescriptor<&HeadphonesVariableProvider::metadataVariable<&Track::primaryArtist>>(
              this, VariableKind::Generic, u"HEADPHONES_ARTIST"_s, u"Headphones Preview"_s,
              u"Artist of the retained headphone preview track"_s),
          makeScriptVariableDescriptor<&HeadphonesVariableProvider::metadataVariable<&Track::album>>(
              this, VariableKind::Generic, u"HEADPHONES_ALBUM"_s, u"Headphones Preview"_s,
              u"Album of the retained headphone preview track"_s),
          makeScriptVariableDescriptor<&HeadphonesVariableProvider::metadataVariable<&Track::effectiveTitle>>(
              this, VariableKind::Generic, u"HEADPHONES_TITLE"_s, u"Headphones Preview"_s,
              u"Title of the retained headphone preview track"_s),
          makeScriptVariableDescriptor<&HeadphonesVariableProvider::metadataVariable<&Track::discNumber>>(
              this, VariableKind::Generic, u"HEADPHONES_DISK"_s, u"Headphones Preview"_s,
              u"Disc number of the retained headphone preview track"_s, {u"HEADPHONES_DISC"_s}),
          makeScriptVariableDescriptor<&HeadphonesVariableProvider::metadataVariable<&Track::trackNumber>>(
              this, VariableKind::Generic, u"HEADPHONES_TRACK"_s, u"Headphones Preview"_s,
              u"Track number of the retained headphone preview track"_s, {u"HEADPHONES_TRACKNUMBER"_s}),
          makeScriptVariableDescriptor<&HeadphonesVariableProvider::iconVariable>(
              this, VariableKind::Generic, u"HEADPHONESICON"_s, u"Headphones Preview"_s,
              u"Headphone symbol for the track currently being previewed, including while paused"_s),
          makeScriptVariableDescriptor<&HeadphonesVariableProvider::stateVariable<Player::PlayState::Playing>>(
              this, VariableKind::Generic, u"HEADPHONES_ISPLAYING"_s, u"Headphones Preview"_s,
              u"Returns 1 while headphone preview is active and not paused"_s),
          makeScriptVariableDescriptor<&HeadphonesVariableProvider::stateVariable<Player::PlayState::Stopped>>(
              this, VariableKind::Generic, u"HEADPHONES_ISSTOPPED"_s, u"Headphones Preview"_s,
              u"Returns 1 while headphone preview is stopped or has failed"_s),
          makeScriptVariableDescriptor<&HeadphonesVariableProvider::stateVariable<Player::PlayState::Paused>>(
              this, VariableKind::Generic, u"HEADPHONES_ISPAUSED"_s, u"Headphones Preview"_s,
              u"Returns 1 while headphone preview is paused, including a pending pause during loading"_s),
      }
{
    updateSnapshot(controller.snapshot());
    connect(&controller, &PreviewController::snapshotChanged, this, &HeadphonesVariableProvider::updateSnapshot);
}

std::span<const ScriptVariableDescriptor> HeadphonesVariableProvider::variables() const
{
    return m_variables;
}

void HeadphonesVariableProvider::updateSnapshot(const PreviewSnapshot& snapshot)
{
    const auto previous = m_snapshot.load();
    const auto state    = scriptState(snapshot);
    if(previous->state == state && previous->track.sameDataAs(snapshot.track)) {
        return;
    }

    m_snapshot.store(std::make_shared<const Snapshot>(Snapshot {snapshot.track, state}));
    // Position, volume and internal buffering transitions do not change these variables.
    Q_EMIT variablesChanged();
}

template <auto Getter>
ScriptResult HeadphonesVariableProvider::metadataVariable() const
{
    const auto snapshot = m_snapshot.load();
    if(!snapshot->track.isValid()) {
        return {};
    }
    const QString value = std::invoke(Getter, snapshot->track);
    return {.value = value, .cond = !value.isEmpty()};
}

template <Player::PlayState State>
ScriptResult HeadphonesVariableProvider::stateVariable() const
{
    const bool matches = m_snapshot.load()->state == State;
    return {.value = matches ? u"1"_s : QString {}, .cond = matches};
}

ScriptResult HeadphonesVariableProvider::iconVariable(const ScriptContext& context) const
{
    const auto snapshot = m_snapshot.load();
    if(snapshot->state == Player::PlayState::Stopped || !snapshot->track.isValid() || !context.track
       || !snapshot->track.sameIdentityAs(*context.track)) {
        return {};
    }
    return {.value = u"\U0001F3A7"_s, .cond = true};
}

} // namespace Fooyin::Headphones
