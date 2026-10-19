// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphoneswidget.h"

#include "headphonedevices.h"
#include "headphonesactions.h"
#include "headphonessettings.h"
#include "headphonessettingsdialog.h"
#include "previewcontroller.h"

#include <gui/guiconstants.h>

#include <QAction>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {
namespace {
constexpr int SeekScale = 10000;
uint64_t sliderTime(uint64_t duration, int value)
{
    duration = std::min<uint64_t>(duration, std::numeric_limits<int64_t>::max());
    // Even when long double is only 64-bit, rounding up to 2^63 still fits
    // uint64_t. Clamp before handing the result to the signed seek interface.
    const auto scaled
        = static_cast<uint64_t>(static_cast<long double>(duration) * std::clamp(value, 0, SeekScale) / SeekScale);
    return std::min(scaled, duration);
}
QString timeText(uint64_t ms)
{
    const auto seconds = ms / 1000;
    return u"%1:%2"_s.arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char {'0'});
}
} // namespace

HeadphonesWidget::HeadphonesWidget(PreviewController& controller, HeadphonesActions& actions,
                                   HeadphonesSettings& settings, HeadphoneDeviceCatalogue& devices,
                                   std::weak_ptr<HeadphonesSettingsProvider> provider, QWidget* parent)
    : FyWidget {parent}
    , m_controller {&controller}
    , m_actions {&actions}
    , m_settings {&settings}
    , m_devices {&devices}
    , m_title {new QLabel(this)}
    , m_device {new QLabel(this)}
    , m_status {new QLabel(this)}
    , m_position {new QLabel(this)}
    , m_volumeValue {new QLabel(this)}
    , m_preview {new QPushButton(tr("Preview selected"), this)}
    , m_playPause {new QPushButton(this)}
    , m_seek {new QSlider(Qt::Horizontal, this)}
    , m_volume {new QSlider(Qt::Horizontal, this)}
{
    setObjectName(u"HeadphonesPreview"_s);
    auto* layout = new QVBoxLayout(this);
    for(auto* label : {m_title, m_device, m_status, m_position}) {
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
    }
    m_title->setObjectName(u"Headphones.Title"_s);
    m_device->setObjectName(u"Headphones.Device"_s);
    m_status->setObjectName(u"Headphones.Status"_s);
    m_position->setObjectName(u"Headphones.Position"_s);
    m_seek->setObjectName(u"Headphones.Seek"_s);
    m_volume->setObjectName(u"Headphones.Volume"_s);
    m_preview->setObjectName(u"Headphones.Preview"_s);
    m_volumeValue->setObjectName(u"Headphones.VolumeValue"_s);
    m_playPause->setObjectName(u"Headphones.PlayPause"_s);
    m_seek->setAccessibleName(tr("Headphone preview position"));
    m_volume->setAccessibleName(tr("Headphone volume"));
    m_seek->setTracking(false);
    m_volume->setRange(0, 100);
    layout->addWidget(m_title);
    layout->addWidget(m_device);
    layout->addWidget(m_status);
    auto* controls = new QHBoxLayout;
    controls->addWidget(m_preview);
    controls->addWidget(m_playPause);
    auto* stop = new QPushButton(tr("Stop"), this);
    stop->setObjectName(u"Headphones.Stop"_s);
    stop->setIcon(QIcon::fromTheme(QString::fromLatin1(Constants::Icons::Stop)));
    controls->addWidget(stop);
    connect(m_playPause, &QPushButton::clicked, actions.playPauseAction(), &QAction::trigger);
    connect(stop, &QPushButton::clicked, actions.stopAction(), &QAction::trigger);
    const auto updateButtons = [this, stop]() {
        if(m_actions) {
            m_playPause->setEnabled(m_actions->playPauseAction()->isEnabled());
            stop->setEnabled(m_actions->stopAction()->isEnabled());
        }
    };
    connect(actions.playPauseAction(), &QAction::changed, this, updateButtons);
    connect(actions.stopAction(), &QAction::changed, this, updateButtons);
    updateButtons();
    auto* configure = new QPushButton(tr("Device…"), this);
    configure->setObjectName(u"Headphones.Configure"_s);
    controls->addWidget(configure);
    layout->addLayout(controls);
    layout->addWidget(m_seek);
    layout->addWidget(m_position);
    auto* volumeRow = new QHBoxLayout;
    volumeRow->addWidget(new QLabel(tr("Headphone volume"), this));
    volumeRow->addWidget(m_volume);
    volumeRow->addWidget(m_volumeValue);
    layout->addLayout(volumeRow);
    connect(configure, &QPushButton::clicked, this, [this, provider]() {
        if(auto owner = provider.lock()) {
            owner->showSettings(window());
        }
    });
    connect(m_preview, &QPushButton::clicked, &actions, &HeadphonesActions::previewDisplaySelection);
    connect(&actions, &HeadphonesActions::availabilityChanged, this, [this]() {
        if(m_actions) {
            const auto reason = m_actions->displayPreviewReason();
            m_preview->setEnabled(reason.isEmpty());
            m_preview->setToolTip(reason.isEmpty() ? tr("Preview the first selected track") : reason);
        }
    });
    connect(&controller, &PreviewController::snapshotChanged, this, &HeadphonesWidget::refresh);
    connect(&devices, &HeadphoneDeviceCatalogue::devicesChanged, this, &HeadphonesWidget::refreshDevice);
    connect(&settings, &HeadphonesSettings::deviceChanged, this, &HeadphonesWidget::refreshDevice);
    connect(m_volume, &QSlider::valueChanged, this, [this](int value) {
        if(!m_updating && m_controller) {
            m_controller->setVolume(value / 100.0);
        }
    });
    connect(m_seek, &QSlider::sliderPressed, this, [this]() { m_scrubbing = true; });
    connect(m_seek, &QSlider::sliderMoved, this, &HeadphonesWidget::showTentative);
    connect(m_seek, &QSlider::sliderReleased, this, [this]() {
        m_scrubbing = false;
        requestSeek(m_seek->sliderPosition());
    });
    connect(m_seek, &QSlider::valueChanged, this, [this](int value) {
        if(!m_updating && !m_scrubbing) {
            requestSeek(value);
        }
    });
    refresh(controller.snapshot());
    refreshDevice();
    m_preview->setEnabled(actions.displayPreviewReason().isEmpty());
    m_preview->setToolTip(actions.displayPreviewReason());
}

QString HeadphonesWidget::name() const
{
    return tr("Headphones Preview");
}
QString HeadphonesWidget::layoutName() const
{
    return u"HeadphonesPreview"_s;
}

void HeadphonesWidget::refreshDevice()
{
    if(!m_settings || !m_devices) {
        return;
    }
    const auto id     = m_settings->deviceId();
    const auto device = m_devices->resolve(id);
    m_device->setText(id.isEmpty() ? tr("Headphones: not configured")
                      : device     ? tr("Headphones: %1").arg(device->description)
                                   : tr("Headphones: %1 (unavailable)").arg(m_settings->deviceDescription()));
}

void HeadphonesWidget::refresh(const PreviewSnapshot& snapshot)
{
    const bool trackChanged = !snapshot.track.sameIdentityAs(m_snapshot.track);
    m_snapshot              = snapshot;
    if(trackChanged || snapshot.state == PreviewState::Stopped || snapshot.state == PreviewState::Error) {
        m_scrubbing   = false;
        m_seekPending = false;
    }
    if(snapshot.state != PreviewState::Loading) {
        m_seekPending = false;
    }
    const auto title  = snapshot.track.effectiveTitle();
    const auto artist = snapshot.track.primaryArtist();
    m_title->setText(!snapshot.track.isValid() ? tr("No headphone preview")
                     : artist.isEmpty()        ? title
                                               : tr("%1 — %2").arg(artist, title));
    QString status;
    switch(snapshot.state) {
        case PreviewState::Stopped:
            status = tr("Stopped");
            break;
        case PreviewState::Loading:
            status = snapshot.pauseRequested ? tr("Loading (pause requested)") : tr("Loading…");
            break;
        case PreviewState::Buffering:
            status = tr("Buffering…");
            break;
        case PreviewState::Playing:
            status = tr("Playing");
            break;
        case PreviewState::Paused:
            status = tr("Paused");
            break;
        case PreviewState::Draining:
            status = tr("Finishing…");
            break;
        case PreviewState::Error:
            status = snapshot.error;
            break;
    }
    if(snapshot.state == PreviewState::Stopped && !snapshot.deviceAvailable) {
        status += tr(" — configure an available headphone output");
    }
    m_status->setText(status);
    const bool canPlay
        = snapshot.pauseRequested || snapshot.state == PreviewState::Stopped || snapshot.state == PreviewState::Error;
    m_playPause->setText(canPlay ? tr("Play") : tr("Pause"));
    m_playPause->setIcon(
        QIcon::fromTheme(QString::fromLatin1(canPlay ? Constants::Icons::Play : Constants::Icons::Pause)));
    m_playPause->setToolTip(canPlay ? tr("Play headphone preview") : tr("Pause headphone preview"));
    const bool active = snapshot.state != PreviewState::Stopped && snapshot.state != PreviewState::Error;
    m_updating        = true;
    m_seek->setRange(0, snapshot.durationMs == 0 ? 0 : SeekScale);
    m_seek->setEnabled(active && snapshot.seekable && snapshot.durationMs != 0);
    if(!m_scrubbing && !m_seekPending) {
        const auto ratio
            = snapshot.durationMs == 0 ? 0.0L : static_cast<long double>(snapshot.positionMs) / snapshot.durationMs;
        m_seek->setValue(static_cast<int>(std::clamp(ratio, 0.0L, 1.0L) * SeekScale));
        m_position->setText(tr("%1 / %2").arg(timeText(snapshot.positionMs),
                                              snapshot.durationMs ? timeText(snapshot.durationMs) : tr("unknown")));
    }
    m_volume->setValue(static_cast<int>(std::lround(snapshot.volume * 100)));
    m_volume->setToolTip(tr("Headphone volume: %1%").arg(m_volume->value()));
    m_volumeValue->setText(tr("%1%").arg(m_volume->value()));
    m_volume->setAccessibleDescription(m_volume->toolTip());
    m_updating = false;
}

void HeadphonesWidget::showTentative(int value)
{
    const auto ms = sliderTime(m_snapshot.durationMs, value);
    m_position->setText(tr("%1 / %2").arg(timeText(ms), timeText(m_snapshot.durationMs)));
}

void HeadphonesWidget::requestSeek(int value)
{
    if(!m_controller || !m_seek->isEnabled()) {
        return;
    }
    const auto target = sliderTime(m_snapshot.durationMs, value);
    if(m_seekPending && target == m_target) {
        return;
    }
    m_target      = target;
    m_seekPending = true;
    showTentative(value);
    if(!m_controller->seek(static_cast<int64_t>(std::min<uint64_t>(target, std::numeric_limits<int64_t>::max())))) {
        m_seekPending = false;
        refresh(m_controller->snapshot());
    }
}

} // namespace Fooyin::Headphones
