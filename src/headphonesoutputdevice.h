// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gui/fywidget.h>

#include <QPointer>

class QComboBox;
namespace Fooyin::Headphones {
class HeadphonesSettings;
class HeadphoneDeviceCatalogue;

class HeadphonesOutputDevice : public FyWidget
{
    Q_OBJECT
public:
    HeadphonesOutputDevice(HeadphonesSettings& settings, HeadphoneDeviceCatalogue& devices, QWidget* parent = nullptr);
    QString name() const override;
    QString layoutName() const override;

private:
    void refreshDevices();
    QPointer<HeadphonesSettings> m_settings;
    QPointer<HeadphoneDeviceCatalogue> m_devices;
    QComboBox* m_combo;
};
} // namespace Fooyin::Headphones
