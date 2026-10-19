// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <core/scripting/scriptdefs.h>

#include <QObject>

#include <atomic>
#include <memory>

namespace Fooyin::Headphones {

class PreviewController;
struct PreviewSnapshot;

// Publishes immutable state for script callbacks running on fooyin's workers.
class HeadphonesVariableProvider : public QObject
{
    Q_OBJECT

public:
    explicit HeadphonesVariableProvider(PreviewController& controller);
    [[nodiscard]] std::span<const ScriptVariableDescriptor> variables() const;

Q_SIGNALS:
    void variablesChanged();

private:
    struct Snapshot
    {
        Track track;
        Player::PlayState state {Player::PlayState::Stopped};
    };

    void updateSnapshot(const PreviewSnapshot& snapshot);
    template <auto Getter>
    ScriptResult metadataVariable() const;
    template <Player::PlayState State>
    ScriptResult stateVariable() const;
    ScriptResult iconVariable(const ScriptContext& context) const;

    std::atomic<std::shared_ptr<const Snapshot>> m_snapshot;
    std::vector<ScriptVariableDescriptor> m_variables;
};

} // namespace Fooyin::Headphones
