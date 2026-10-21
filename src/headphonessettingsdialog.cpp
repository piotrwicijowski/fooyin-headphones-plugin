// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonessettingsdialog.h"

#include "headphonedevices.h"
#include "headphonesdestinationchoice.h"
#include "headphonessettings.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

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
    , m_pendingPair {settings.outputChannelPair()}
{
    setObjectName(u"Headphones.Settings"_s);
    setWindowTitle(tr("Headphones Preview"));
    auto* layout = new QVBoxLayout(this);
    auto* label  = new QLabel(tr("Headphone output:"), this);
    m_combo->setObjectName(u"Headphones.DeviceChoice"_s);
    m_combo->setAccessibleName(tr("Headphone output"));
    label->setBuddy(m_combo);
    layout->addWidget(label);
    layout->addWidget(m_combo);
    auto* note = new QLabel(tr("Choose the device or channel pair connected to your headphones. "
                               "Configure fooyin’s main output separately. "
                               "Automatic uses the device’s normal mono/stereo output. "
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
        m_pendingDescription = m_combo->currentData(DestinationDescriptionRole).toString();
        m_pendingPair        = m_combo->currentData(DestinationPairRole).toInt();
        m_combo->setToolTip(tr("%1\nChanging output stops preview. Press Play to restart.").arg(m_combo->currentText()));
    });
    connect(&devices, &HeadphoneDeviceCatalogue::devicesChanged, this, &HeadphonesSettingsDialog::refreshDevices);
    refreshDevices();
    resize(500, sizeHint().height());
}

void HeadphonesSettingsDialog::refreshDevices()
{
    const auto devices = m_devices ? m_devices->devices() : std::vector<HeadphoneDevice> {};
    populateDestinationChoice(*m_combo, devices, m_pendingId, m_pendingDescription, m_pendingPair);
}

void HeadphonesSettingsDialog::resetPending()
{
    if(m_settings) {
        m_pendingId          = m_settings->deviceId();
        m_pendingDescription = m_settings->deviceDescription();
        m_pendingPair        = m_settings->outputChannelPair();
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
        m_settings->setDestination(m_pendingId, m_pendingDescription, m_pendingPair);
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
