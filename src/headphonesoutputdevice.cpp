// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonesoutputdevice.h"

#include "headphonedevices.h"
#include "headphonessettings.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QSignalBlocker>

#include <algorithm>

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
    m_combo->setAccessibleName(tr("Headphone output device"));
    m_combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_combo->setMinimumContentsLength(12);
    connect(m_combo, &QComboBox::activated, this, [this](int index) {
        if(m_settings && index >= 0) {
            m_settings->setDevice(m_combo->itemData(index).toByteArray(),
                                  m_combo->itemData(index, Qt::UserRole + 1).toString());
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
    const QSignalBlocker blocker {m_combo};
    const auto selected = m_settings->deviceId();
    const auto devices  = m_devices->devices();
    m_combo->clear();
    m_combo->addItem(tr("Not configured"), QByteArray {});
    bool found = selected.isEmpty();
    for(const auto& device : devices) {
        if(device.id.isEmpty()) {
            continue;
        }
        const auto duplicates = std::count_if(
            devices.begin(), devices.end(), [&](const auto& other) { return other.description == device.description; });
        const auto name
            = duplicates > 1 ? tr("%1 [%2]").arg(device.description, encodeDeviceId(device.id)) : device.description;
        m_combo->addItem(name, device.id);
        m_combo->setItemData(m_combo->count() - 1, device.description, Qt::UserRole + 1);
        found = found || device.id == selected;
    }
    if(!found) {
        m_combo->addItem(tr("Unavailable — %1 [%2]").arg(m_settings->deviceDescription(), encodeDeviceId(selected)),
                         selected);
        m_combo->setItemData(m_combo->count() - 1, m_settings->deviceDescription(), Qt::UserRole + 1);
    }
    m_combo->setCurrentIndex(m_combo->findData(selected));
    m_combo->setToolTip(m_combo->currentText());
}
} // namespace Fooyin::Headphones
