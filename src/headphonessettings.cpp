// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonessettings.h"

#include "headphonedevices.h"

#include <utils/settings/settingsmanager.h>

#include <QVariantMap>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {

HeadphonesSettings::HeadphonesSettings(SettingsManager* settings, QObject* parent)
    : QObject {parent}
    , m_settings {settings}
{
    if(m_settings) {
        m_settings->createSetting(u"Headphones/DeviceId"_s, QString {});
        m_settings->createSetting(u"Headphones/DeviceDescription"_s, QString {});
        m_settings->createSetting(u"Headphones/OutputRoute"_s, QVariantMap {});
        m_settings->createSetting(u"Headphones/Volume"_s, 0.25);
        m_settings->createSetting(u"Headphones/PreviewStartOffsetSeconds"_s, 0);
        m_lastDeviceId    = deviceId();
        m_lastChannelPair = outputChannelPair();
        m_settings->subscribe(u"Headphones/DeviceId", this, [this]() { refreshDestination(); });
        m_settings->subscribe(u"Headphones/OutputRoute", this, [this]() { refreshDestination(); });
        m_settings->subscribe(u"Headphones/Volume", this, [this]() { Q_EMIT volumeChanged(); });
        m_settings->subscribe(u"Headphones/PreviewStartOffsetSeconds", this,
                              [this]() { Q_EMIT previewStartOffsetChanged(); });
    }
}

QByteArray HeadphonesSettings::deviceId() const
{
    return m_settings ? decodeDeviceId(m_settings->value(u"Headphones/DeviceId").toString()) : QByteArray {};
}

QString HeadphonesSettings::deviceDescription() const
{
    return m_settings ? m_settings->value(u"Headphones/DeviceDescription").toString() : QString {};
}

int HeadphonesSettings::outputChannelPair() const
{
    if(!m_settings) {
        return 0;
    }
    const auto stored = m_settings->value(u"Headphones/OutputRoute");
    if(stored.metaType() != QMetaType::fromType<QVariantMap>()) {
        return -1;
    }
    const auto route = stored.toMap();
    if(route.isEmpty()) {
        return 0; // Existing installations retain automatic mono/stereo routing.
    }
    bool valid       = false;
    const auto first = route.value(u"FirstChannel"_s).toString().toInt(&valid);
    if(!valid || first <= 0 || first >= 8 || first % 2 != 1
       || route.value(u"DeviceId"_s).toString() != encodeDeviceId(deviceId()) || deviceId().isEmpty()) {
        return -1; // Never reinterpret an invalid or differently bound explicit route as automatic.
    }
    return first;
}

void HeadphonesSettings::refreshDestination()
{
    if(m_updatingDestination) {
        return;
    }
    const auto id   = deviceId();
    const auto pair = outputChannelPair();
    if(id != m_lastDeviceId || pair != m_lastChannelPair) {
        m_lastDeviceId    = id;
        m_lastChannelPair = pair;
        Q_EMIT deviceChanged();
    }
}

double HeadphonesSettings::volume() const
{
    bool valid {false};
    const auto value = m_settings ? m_settings->value(u"Headphones/Volume").toDouble(&valid) : 0.25;
    return valid && std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.25;
}

int HeadphonesSettings::previewStartOffsetSeconds() const
{
    // Parse as an integer string: QVariant's numeric conversion can round floats
    // or wrap oversized unsigned values before range validation.
    bool valid {false};
    const auto value
        = m_settings ? m_settings->value(u"Headphones/PreviewStartOffsetSeconds").toString().toLongLong(&valid) : 0;
    return valid && value >= 0 && value <= MaxPreviewStartOffsetSeconds ? static_cast<int>(value) : 0;
}

void HeadphonesSettings::setDevice(const QByteArray& id, const QString& description)
{
    setDestination(id, description, id == deviceId() ? outputChannelPair() : 0);
}

void HeadphonesSettings::setDestination(const QByteArray& id, const QString& description, int firstChannel)
{
    if(m_settings) {
        m_updatingDestination = true;
        m_settings->set(u"Headphones/DeviceDescription", description);
        const QVariantMap route
            = firstChannel == 0 ? QVariantMap {}
                                : QVariantMap {{u"DeviceId"_s, encodeDeviceId(id)}, {u"FirstChannel"_s, firstChannel}};
        m_settings->set(u"Headphones/OutputRoute", route);
        m_settings->set(u"Headphones/DeviceId", encodeDeviceId(id));
        m_updatingDestination = false;
        refreshDestination(); // Publish only the complete device + pair transaction.
    }
}

void HeadphonesSettings::setVolume(double uiVolume)
{
    if(m_settings && std::isfinite(uiVolume)) {
        m_settings->set(u"Headphones/Volume", std::clamp(uiVolume, 0.0, 1.0));
    }
}

void HeadphonesSettings::setPreviewStartOffsetSeconds(int seconds)
{
    if(m_settings) {
        m_settings->set(u"Headphones/PreviewStartOffsetSeconds", seconds >= 0 ? seconds : 0);
    }
}

} // namespace Fooyin::Headphones
