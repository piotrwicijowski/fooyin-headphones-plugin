// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonessettingsdialog.h"

#include "headphonedevices.h"
#include "headphonessettings.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {

HeadphonesSettingsDialog::HeadphonesSettingsDialog(HeadphonesSettings& settings, HeadphoneDeviceCatalogue& devices,
                                                   QWidget* parent)
    : QDialog {parent}
    , m_settings {&settings}
    , m_devices {&devices}
    , m_combo {new QComboBox(this)}
    , m_startOffset {new QSpinBox(this)}
    , m_pendingId {settings.deviceId()}
    , m_pendingDescription {settings.deviceDescription()}
{
    setObjectName(u"Headphones.Settings"_s);
    setWindowTitle(tr("Headphones Preview"));
    auto* layout = new QVBoxLayout(this);
    auto* label  = new QLabel(tr("Headphone output device:"), this);
    m_combo->setObjectName(u"Headphones.DeviceChoice"_s);
    m_combo->setAccessibleName(tr("Headphone output device"));
    label->setBuddy(m_combo);
    layout->addWidget(label);
    layout->addWidget(m_combo);
    auto* note = new QLabel(tr("Choose a separately addressable device different from fooyin’s main output. "
                               "OS audio policy can reroute streams on disconnect; verify your routing setup. "
                               "Preview volume is controlled in the Headphones Volume Controls widget."),
                            this);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* offsetLabel = new QLabel(tr("Preview start offset (seconds):"), this);
    m_startOffset->setObjectName(u"Headphones.PreviewStartOffsetSeconds"_s);
    m_startOffset->setAccessibleName(tr("Preview start offset (seconds)"));
    m_startOffset->setRange(0, HeadphonesSettings::MaxPreviewStartOffsetSeconds);
    m_startOffset->setValue(settings.previewStartOffsetSeconds());
    m_startOffset->setToolTip(tr("Used for new previews and restarts. Paused previews resume at their current position. "
                                "Short or unseekable tracks start at the beginning."));
    offsetLabel->setBuddy(m_startOffset);
    layout->addWidget(offsetLabel);
    layout->addWidget(m_startOffset);
    auto* buttons
        = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(u"Headphones.SettingsButtons"_s);
    layout->addWidget(buttons);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &HeadphonesSettingsDialog::apply);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        apply();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_combo, &QComboBox::currentIndexChanged, this, [this]() {
        m_pendingId          = m_combo->currentData().toByteArray();
        m_pendingDescription = m_combo->currentData(Qt::UserRole + 1).toString();
    });
    connect(&devices, &HeadphoneDeviceCatalogue::devicesChanged, this, &HeadphonesSettingsDialog::refreshDevices);
    refreshDevices();
    resize(500, sizeHint().height());
}

void HeadphonesSettingsDialog::refreshDevices()
{
    const QSignalBlocker blocker {m_combo};
    m_combo->clear();
    m_combo->addItem(tr("Not configured"), QByteArray {});
    const auto devices = m_devices ? m_devices->devices() : std::vector<HeadphoneDevice> {};
    bool found         = m_pendingId.isEmpty();
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
        found = found || device.id == m_pendingId;
    }
    if(!found) {
        m_combo->addItem(tr("Unavailable — %1 [%2]").arg(m_pendingDescription, encodeDeviceId(m_pendingId)),
                         m_pendingId);
        m_combo->setItemData(m_combo->count() - 1, m_pendingDescription, Qt::UserRole + 1);
    }
    m_combo->setCurrentIndex(m_combo->findData(m_pendingId));
}

void HeadphonesSettingsDialog::resetPending()
{
    if(m_settings) {
        m_pendingId          = m_settings->deviceId();
        m_pendingDescription = m_settings->deviceDescription();
        m_startOffset->setValue(m_settings->previewStartOffsetSeconds());
        refreshDevices();
    }
}

void HeadphonesSettingsDialog::done(int result)
{
    resetPending();
    QDialog::done(result);
}

void HeadphonesSettingsDialog::showEvent(QShowEvent* event)
{
    resetPending();
    QDialog::showEvent(event);
}

void HeadphonesSettingsDialog::apply()
{
    if(m_settings) {
        const auto description = m_combo->currentData(Qt::UserRole + 1).toString();
        m_settings->setDevice(m_pendingId, description);
        m_settings->setPreviewStartOffsetSeconds(m_startOffset->value());
    }
}

HeadphonesSettingsProvider::HeadphonesSettingsProvider(HeadphonesSettings& settings, HeadphoneDeviceCatalogue& devices)
    : m_settings {&settings}
    , m_devices {&devices}
{
}

HeadphonesSettingsProvider::~HeadphonesSettingsProvider()
{
    delete m_dialog.data();
}

QDialog* HeadphonesSettingsProvider::createSettings(QWidget* parent)
{
    if(!m_settings || !m_devices) {
        return nullptr;
    }
    if(!m_dialog) {
        m_dialog = new HeadphonesSettingsDialog(*m_settings, *m_devices, parent);
    }
    return m_dialog;
}

namespace {
class SharedProviderAdapter final : public PluginSettingsProvider
{
public:
    explicit SharedProviderAdapter(std::weak_ptr<HeadphonesSettingsProvider> provider)
        : m_provider {std::move(provider)}
    {
    }
    QDialog* createSettings(QWidget* parent) override
    {
        const auto provider = m_provider.lock();
        return provider ? provider->createSettings(parent) : nullptr;
    }

private:
    std::weak_ptr<HeadphonesSettingsProvider> m_provider;
};
} // namespace

std::unique_ptr<PluginSettingsProvider> settingsProviderAdapter(std::weak_ptr<HeadphonesSettingsProvider> provider)
{
    return std::make_unique<SharedProviderAdapter>(std::move(provider));
}

} // namespace Fooyin::Headphones
