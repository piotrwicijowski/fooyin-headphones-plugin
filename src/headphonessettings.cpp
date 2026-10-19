// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonessettings.h"

#include "headphonedevices.h"

#include <utils/settings/settingsmanager.h>

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
        m_settings->createSetting(u"Headphones/Volume"_s, 0.25);
        m_settings->subscribe(u"Headphones/DeviceId", this, [this]() { Q_EMIT deviceChanged(); });
        m_settings->subscribe(u"Headphones/Volume", this, [this]() { Q_EMIT volumeChanged(); });
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

double HeadphonesSettings::volume() const
{
    bool valid {false};
    const auto value = m_settings ? m_settings->value(u"Headphones/Volume").toDouble(&valid) : 0.25;
    return valid && std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.25;
}

void HeadphonesSettings::setDevice(const QByteArray& id, const QString& description)
{
    if(m_settings) {
        m_settings->set(u"Headphones/DeviceDescription", description);
        m_settings->set(u"Headphones/DeviceId", encodeDeviceId(id));
    }
}

void HeadphonesSettings::setVolume(double uiVolume)
{
    if(m_settings && std::isfinite(uiVolume)) {
        m_settings->set(u"Headphones/Volume", std::clamp(uiVolume, 0.0, 1.0));
    }
}

} // namespace Fooyin::Headphones
