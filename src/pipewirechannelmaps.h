// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QStringList>

#include <memory>
#include <optional>

namespace Fooyin::Headphones {

// Read-only native metadata: Qt's public channelConfiguration can be a guess
// for Pro Audio devices. No streams, links, profiles or settings are changed.
class PipewireChannelMaps : public QObject
{
    Q_OBJECT
public:
    explicit PipewireChannelMaps(QObject* parent = nullptr);
    ~PipewireChannelMaps() override;
    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] std::optional<QStringList> positions(const QByteArray& deviceId) const;

Q_SIGNALS:
    void mapsChanged();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace Fooyin::Headphones
