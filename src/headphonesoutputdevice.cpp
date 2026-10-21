// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonesoutputdevice.h"

#include "headphonedevices.h"
#include "headphonesdestinationchoice.h"
#include "headphonessettings.h"

#include <QComboBox>
#include <QHBoxLayout>

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {
HeadphonesOutputDevice::HeadphonesOutputDevice(HeadphonesSettings& settings, HeadphoneDeviceCatalogue& devices,
                                               QWidget* parent)
    : FyWidget {parent}
    , m_settings {&settings}
    , m_devices {&devices}
    , m_combo {new QComboBox(this)}
{
    setObjectName(layoutName());
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins({});
    layout->addWidget(m_combo);
    m_combo->setObjectName(u"Headphones.DeviceChoice"_s);
    m_combo->setAccessibleName(tr("Headphone output"));
    m_combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_combo->setMinimumContentsLength(12);
    connect(m_combo, &QComboBox::activated, this, [this](int index) {
        if(m_settings && index >= 0) {
            m_settings->setDestination(m_combo->itemData(index).toByteArray(),
                                       m_combo->itemData(index, DestinationDescriptionRole).toString(),
                                       m_combo->itemData(index, DestinationPairRole).toInt());
        }
    });
    connect(&settings, &HeadphonesSettings::deviceChanged, this, &HeadphonesOutputDevice::refreshDevices);
    connect(&devices, &HeadphoneDeviceCatalogue::devicesChanged, this, &HeadphonesOutputDevice::refreshDevices);
    refreshDevices();
}

QString HeadphonesOutputDevice::name() const
{
    return tr("Headphones Output Device");
}

QString HeadphonesOutputDevice::layoutName() const
{
    return u"HeadphonesOutputDevice"_s;
}

void HeadphonesOutputDevice::refreshDevices()
{
    if(!m_settings || !m_devices) {
        return;
    }
    populateDestinationChoice(*m_combo, m_devices->devices(), m_settings->deviceId(), m_settings->deviceDescription(),
                              m_settings->outputChannelPair());
}
} // namespace Fooyin::Headphones
