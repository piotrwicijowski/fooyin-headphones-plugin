// SPDX-License-Identifier: GPL-3.0-or-later
// This test-only plugin never instantiates a preview controller. All preview
// commands go through the actual loaded Headphones module's public host wiring.
#include <core/coresettings.h>
#include <core/engine/audioloader.h>
#include <core/engine/enginecontroller.h>
#include <core/player/playercontroller.h>
#include <core/playlist/playlisthandler.h>
#include <core/plugins/coreplugin.h>
#include <core/plugins/plugin.h>
#include <gui/fywidget.h>
#include <gui/guiconstants.h>
#include <gui/plugins/guiplugin.h>
#include <gui/trackselectioncontroller.h>
#include <gui/widgetprovider.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/command.h>
#include <utils/settings/settingsmanager.h>

#include <QApplication>
#include <QAudioSink>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QMediaDevices>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <atomic>

using namespace Qt::StringLiterals;
using namespace Fooyin;

namespace {
std::atomic<uint64_t> mainWrittenFrames {0};

// A real explicit-device test backend for fooyin's main engine, registered only
// when the driver is explicitly enabled. It does not alter production plugins.
class MainQtOutput final : public AudioOutput
{
public:
    explicit MainQtOutput(QAudioDevice device)
        : m_device {std::move(device)}
    {
    }
    bool init(const AudioFormat& format) override
    {
        QAudioFormat qt;
        qt.setSampleRate(format.sampleRate());
        qt.setChannelCount(format.channelCount());
        qt.setSampleFormat(QAudioFormat::Float);
        if(format.sampleFormat() != SampleFormat::F32 || !m_device.isFormatSupported(qt)) {
            return false;
        }
        m_format = format;
        m_sink   = std::make_unique<QAudioSink>(m_device, qt);
        m_sink->setBufferSize(static_cast<qsizetype>(format.bytesForDuration(100)));
        m_io = m_sink->start();
        return m_io != nullptr;
    }
    void uninit() override
    {
        if(m_sink) {
            m_sink->reset();
        }
        m_io = nullptr;
        m_sink.reset();
    }
    void reset() override
    {
        if(m_sink) {
            m_sink->reset();
            m_io = m_sink->start();
        }
    }
    void drain() override { }
    void start() override { }
    bool initialised() const override
    {
        return m_sink && m_io;
    }
    QString device() const override
    {
        return QString::fromLatin1(m_device.id().toBase64());
    }
    Fooyin::OutputState currentState() override
    {
        const auto free   = static_cast<int>(m_sink->bytesFree() / m_format.bytesPerFrame());
        const auto queued = std::max(0, bufferSize() - free);
        return {free, queued, static_cast<double>(queued) / m_format.sampleRate()};
    }
    int bufferSize() const override
    {
        return static_cast<int>(m_sink->bufferSize() / m_format.bytesPerFrame());
    }
    OutputDevices getAllDevices(bool) override
    {
        return {{device(), u"Explicit same-process validation main device"_s}};
    }
    int write(std::span<const std::byte> data, int frames) override
    {
        if(!m_io) {
            return -1;
        }
        const auto written
            = m_io->write(reinterpret_cast<const char*>(data.data()),
                          std::min<qint64>(data.size(), static_cast<qint64>(frames) * m_format.bytesPerFrame()));
        const auto accepted = written < 0 ? -1 : static_cast<int>(written / m_format.bytesPerFrame());
        if(accepted > 0) {
            mainWrittenFrames += static_cast<uint64_t>(accepted);
        }
        return accepted;
    }
    void setPaused(bool paused) override
    {
        paused ? m_sink->suspend() : m_sink->resume();
    }
    void setDevice(const QString&) override { }
    AudioFormat format() const override
    {
        return m_format;
    }
    bool supportsVolumeControl() const override
    {
        return false;
    }
    ~MainQtOutput() override
    {
        uninit();
    }

private:
    QAudioDevice m_device;
    AudioFormat m_format;
    std::unique_ptr<QAudioSink> m_sink;
    QIODevice* m_io {nullptr};
};
} // namespace

class HeadphonesInProcessDriver : public QObject,
                                  public Plugin,
                                  public CorePlugin,
                                  public GuiPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID FOOYIN_PLUGIN_IID FILE "inprocessdriver.json")
    Q_INTERFACES(Fooyin::Plugin Fooyin::CorePlugin Fooyin::GuiPlugin)
public:
    HeadphonesInProcessDriver()
    {
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() { shutdown(); });
    }
    void initialise(const CorePluginContext& context) override
    {
        m_enabled = qEnvironmentVariable("HEADPHONES_INPROCESS_TEST") == u"1";
        if(!m_enabled) {
            return;
        }
        m_player    = context.playerController;
        m_engine    = context.engine;
        m_loader    = context.audioLoader;
        m_playlists = context.playlistHandler;
        m_settings  = context.settingsManager;
    }
    void initialise(const GuiPluginContext& context) override
    {
        if(!m_enabled) {
            return;
        }
        m_actions   = context.actionManager;
        m_selection = context.trackSelection;
        m_widgets   = context.widgetProvider;
        connect(&m_timer, &QTimer::timeout, this, &HeadphonesInProcessDriver::tick);
        m_timer.start(50);
        m_elapsed.start();
    }
    void shutdown() override
    {
        if(m_closed) {
            return;
        }
        m_closed = true;
        m_timer.stop();
        delete m_panel.data();
        m_panel = nullptr;
        if(m_selectionContext) {
            m_actions->removeContextObject(m_selectionContext.get());
        }
        m_selectionContext.reset();
        m_selectionView.reset();
        m_loader.reset();
    }

private:
    bool check(bool condition, const QString& message)
    {
        if(!condition) {
            qCritical() << "HEADPHONES SAME-PROCESS FAIL:" << message << "stage" << m_stage;
            m_timer.stop();
            QCoreApplication::exit(20);
        }
        return condition;
    }
    QString status() const
    {
        return m_panel->findChild<QLabel*>(u"Headphones.Status"_s)->text();
    }
    void trigger(const char* id)
    {
        auto* command = m_actions->command(id);
        if(check(command && command->action()->isEnabled(), u"Unavailable action %1"_s.arg(QString::fromLatin1(id)))) {
            command->action()->trigger();
        }
    }
    bool mainUnchanged()
    {
        for(const auto& [id, tracks] : m_playlistSnapshot) {
            const auto* playlist = m_playlists->playlistById(id);
            if(!check(playlist && playlist->tracks() == tracks, u"Main playlist contents changed"_s)) {
                return false;
            }
        }
        return check(m_player->playState() == Player::PlayState::Playing
                         && m_player->currentTrack().filepath() == m_main.filepath(),
                     u"Main playback/track changed"_s)
            && check(m_player->playbackQueue().items() == m_queue.items
                         && m_player->playbackQueue().currentIndex() == m_queue.currentIndex,
                     u"Main queue changed"_s)
            && check(m_settings->value<Settings::Core::OutputVolume>() == m_mainVolume
                         && m_settings->value<Settings::Core::AudioOutput>() == m_mainOutput,
                     u"Main output/volume settings changed"_s)
            && check(m_transport->isEmpty() && m_pauses->isEmpty() && m_stops->isEmpty() && m_tracks->isEmpty()
                         && m_outputs->isEmpty() && m_devices->isEmpty() && m_seeks->isEmpty()
                         && m_playStates->isEmpty() && m_trackPlayed->isEmpty() && m_volumeEvents == 0
                         && m_outputSettingEvents == 0,
                     u"Preview emitted main transport/track/output events"_s);
    }
    void previewMenu(const Track& track)
    {
        auto menu = std::make_unique<QMenu>();
        TrackSelection selection;
        selection.tracks = {track, m_other};
        m_selection->addTrackContextMenu(menu.get(), selection);
        auto* action = menu->findChild<QAction*>(u"Headphones.PreviewSelected.Context"_s);
        if(!check(action && action->isEnabled(), u"Actual plugin context action missing/disabled"_s)) {
            return;
        }
        // Different focus and ambient selection after menu construction must not
        // change the immutable track captured by the actual plugin action.
        QWidget otherView;
        WidgetContext context {&otherView};
        TrackSelection changed;
        changed.tracks = {m_other};
        m_selection->changeSelectedTracks(&context, changed);
        otherView.setFocus();
        action->trigger();
    }
    void tick()
    {
        if(!check(m_elapsed.elapsed() < 45000, u"Validation timeout"_s)) {
            return;
        }
        if(m_stage == 0) {
            if(!m_widgets->widgetExists(u"HeadphonesPreview"_s)) {
                return;
            }
            const auto mainId = QByteArray::fromBase64(qgetenv("HEADPHONES_TEST_MAIN_ID"));
            m_headId          = QByteArray::fromBase64(qgetenv("HEADPHONES_TEST_ID"));
            if(!check(!mainId.isEmpty() && !m_headId.isEmpty() && mainId != m_headId,
                      u"Explicit distinct Qt device IDs are required"_s)) {
                return;
            }
            const auto devices = QMediaDevices::audioOutputs();
            const auto found   = std::find_if(devices.begin(), devices.end(),
                                              [&](const auto& device) { return device.id() == mainId; });
            if(!check(found != devices.end(), u"Main test device unavailable"_s)) {
                return;
            }
            const auto mainDevice = *found;
            m_engine->addOutput(u"Headphones validation Qt main"_s,
                                [mainDevice]() { return std::make_unique<MainQtOutput>(mainDevice); });
            Engine::OutputProfileRequest request;
            request.output   = u"Headphones validation Qt main"_s;
            request.device   = QString::fromLatin1(mainId.toBase64());
            request.bitDepth = SampleFormat::F32;
            m_engine->applyOutputProfile(request);
            m_main    = Track {qEnvironmentVariable("HEADPHONES_TEST_MAIN_FILE")};
            m_preview = Track {qEnvironmentVariable("HEADPHONES_TEST_PREVIEW_FILE")};
            m_other   = Track {qEnvironmentVariable("HEADPHONES_TEST_OTHER_FILE")};
            if(!check(m_loader->readTrackMetadata(m_main) && m_loader->readTrackMetadata(m_preview)
                          && m_loader->readTrackMetadata(m_other),
                      u"Fixture metadata could not be read"_s)) {
                return;
            }
            m_main.setTitle(u"Main in-process"_s);
            m_preview.setTitle(u"Preview A"_s);
            m_other.setTitle(u"Preview B"_s);
            m_settings->set(u"Headphones/DeviceDescription", u"Explicit validation headphones"_s);
            m_settings->set(u"Headphones/DeviceId", QString::fromLatin1(m_headId.toBase64()));
            m_panel = m_widgets->createWidget(u"HeadphonesPreview"_s);
            if(!check(m_panel, u"Actual plugin panel was not created"_s)) {
                return;
            }
            m_panel->show();
            if(qEnvironmentVariable("HEADPHONES_TEST_EXPECT_VIM") == u"1"
               && !check(m_widgets->widgetExists(u"VimModeIndicator"_s), u"Vim Motions was not loaded"_s)) {
                return;
            }
            m_selection->startPlayback({m_main});
            m_player->queueTrack(m_other);
            m_stage = 1;
        }
        else if(m_stage == 1) {
            if(m_player->playState() != Player::PlayState::Playing || m_player->currentPosition() < 100
               || mainWrittenFrames < 48000) {
                return;
            }
            m_queue = m_player->playbackQueue().snapshot();
            for(const auto* playlist : m_playlists->playlists()) {
                m_playlistSnapshot.emplace_back(playlist->id(), playlist->tracks());
            }
            m_mainVolume  = m_settings->value<Settings::Core::OutputVolume>();
            m_mainOutput  = m_settings->value<Settings::Core::AudioOutput>();
            m_transport   = std::make_unique<QSignalSpy>(m_player, &PlayerController::transportPlayRequested);
            m_pauses      = std::make_unique<QSignalSpy>(m_player, &PlayerController::transportPauseRequested);
            m_stops       = std::make_unique<QSignalSpy>(m_player, &PlayerController::transportStopRequested);
            m_tracks      = std::make_unique<QSignalSpy>(m_player, &PlayerController::currentTrackChanged);
            m_outputs     = std::make_unique<QSignalSpy>(m_engine, &EngineController::outputChanged);
            m_devices     = std::make_unique<QSignalSpy>(m_engine, &EngineController::deviceChanged);
            m_seeks       = std::make_unique<QSignalSpy>(m_player, &PlayerController::positionMoved);
            m_playStates  = std::make_unique<QSignalSpy>(m_player, &PlayerController::playStateChanged);
            m_trackPlayed = std::make_unique<QSignalSpy>(m_player, &PlayerController::trackPlayed);
            m_settings->subscribe<Settings::Core::OutputVolume>(this, [this]() { ++m_volumeEvents; });
            m_settings->subscribe<Settings::Core::AudioOutput>(this, [this]() { ++m_outputSettingEvents; });
            previewMenu(m_preview);
            m_stage = 2;
        }
        else if(m_stage == 2) {
            if(status() != u"Playing") {
                return;
            }
            if(!mainUnchanged()
               || !check(m_panel->findChild<QLabel*>(u"Headphones.Title"_s)->text().contains(u"Preview A"),
                         u"Context-menu snapshot target changed"_s)) {
                return;
            }
            auto* command = m_actions->command("Headphones.PlayPause");
            command->setShortcut({QKeySequence::fromString(u"Ctrl+Alt+Shift+F12"_s)});
            m_panel->activateWindow();
            m_panel->setFocus();
            m_stage = 20;
        }
        else if(m_stage == 20) {
            QTest::keyClick(m_panel, Qt::Key_F12, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
            m_pausePosition = m_player->currentPosition();
            m_pauseFrames   = mainWrittenFrames.load();
            m_pauseWait.start();
            m_stage = 3;
        }
        else if(m_stage == 3) {
            if(m_pauseWait.elapsed() < 300) {
                return;
            }
            if(!mainUnchanged()
               || !check(status() == u"Paused" && m_player->currentPosition() > m_pausePosition
                             && mainWrittenFrames > m_pauseFrames,
                         u"Shortcut did not pause preview or main did not keep producing audio"_s)) {
                return;
            }
            m_panel->findChild<QSlider*>(u"Headphones.Volume"_s)->setValue(61);
            m_panel->findChild<QSlider*>(u"Headphones.Seek"_s)->setValue(5000);
            m_stage = 4;
        }
        else if(m_stage == 4) {
            if(status() != u"Paused") {
                return;
            }
            if(!mainUnchanged()
               || !check(m_panel->findChild<QLabel*>(u"Headphones.Position"_s)->text().startsWith(u"0:10"),
                         u"Paused seek was not applied"_s)) {
                return;
            }
            trigger("Headphones.PlayPause");
            delete m_panel.data();
            m_panel = m_widgets->createWidget(u"HeadphonesPreview"_s);
            if(!check(m_panel, u"Panel could not be recreated"_s)) {
                return;
            }
            m_panel->show();
            m_stage = 5;
        }
        else if(m_stage == 5) {
            if(status() != u"Playing") {
                return;
            }
            if(!mainUnchanged()
               || !check(m_panel->findChild<QSlider*>(u"Headphones.Volume"_s)->value() == 61,
                         u"Recreated panel lost volume"_s)) {
                return;
            }
            m_panel->findChild<QPushButton*>(u"Headphones.Configure"_s)->click();
            QDialog* dialog {nullptr};
            for(auto* widget : QApplication::topLevelWidgets()) {
                if(widget->objectName() == u"Headphones.Settings") {
                    dialog = qobject_cast<QDialog*>(widget);
                }
            }
            if(!check(dialog, u"Actual plugin configuration did not open"_s)) {
                return;
            }
            auto* combo   = dialog->findChild<QComboBox*>(u"Headphones.DeviceChoice"_s);
            auto* buttons = dialog->findChild<QDialogButtonBox*>(u"Headphones.SettingsButtons"_s);
            buttons->button(QDialogButtonBox::Apply)->click();
            if(!check(status() == u"Playing", u"Applying the same device interrupted preview"_s)) {
                return;
            }
            combo->setCurrentIndex(combo->findData(QByteArray {}));
            buttons->button(QDialogButtonBox::Apply)->click();
            if(!check(status().startsWith(u"Stopped"), u"Changed device did not stop preview"_s)) {
                return;
            }
            combo->setCurrentIndex(combo->findData(m_headId));
            buttons->button(QDialogButtonBox::Apply)->click();
            if(!check(status() == u"Stopped", u"Restoring a device automatically resumed playback"_s)
               || !mainUnchanged()) {
                return;
            }
            buttons->button(QDialogButtonBox::Cancel)->click();
            m_selectionView = std::make_unique<QWidget>();
            m_selectionView->setFocusPolicy(Qt::StrongFocus);
            m_selectionContext
                = std::make_unique<WidgetContext>(m_selectionView.get(), Context {Constants::Context::TrackSelection});
            m_actions->addContextObject(m_selectionContext.get());
            TrackSelection selection;
            selection.tracks = {m_other, m_preview};
            m_selection->changeSelectedTracks(m_selectionContext.get(), selection);
            m_selectionView->show();
            m_selectionView->activateWindow();
            m_selectionView->setFocus();
            m_stage = 7;
        }
        else if(m_stage == 7) {
            auto* command = m_actions->command("Headphones.PreviewSelected");
            if(!command->action()->isEnabled()) {
                return;
            }
            command->setShortcut({QKeySequence::fromString(u"Ctrl+Alt+Shift+F11"_s)});
            QTest::keyClick(m_selectionView.get(), Qt::Key_F11,
                            Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
            m_stage = 8;
        }
        else if(m_stage == 8) {
            if(status() != u"Playing") {
                return;
            }
            if(!mainUnchanged()
               || !check(m_panel->findChild<QLabel*>(u"Headphones.Title"_s)->text().contains(u"Preview B"),
                         u"Focused preview shortcut targeted the wrong track"_s)) {
                return;
            }
            trigger("Headphones.Stop");
            m_panel->activateWindow();
            QTest::mouseClick(m_panel->findChild<QPushButton*>(u"Headphones.Preview"_s), Qt::LeftButton);
            m_stage = 9;
        }
        else if(m_stage == 9) {
            if(status() != u"Playing") {
                return;
            }
            if(!mainUnchanged()
               || !check(m_panel->findChild<QLabel*>(u"Headphones.Title"_s)->text().contains(u"Preview B"),
                         u"Panel lost the retained display selection"_s)) {
                return;
            }
            trigger("Headphones.Stop");
            qInfo() << "HEADPHONES SAME-PROCESS PASS: actual plugin menu/actions/keyboard/panel selection, "
                       "pause/seek/volume, panel recreation, settings; main track/playlist/queue/output/volume "
                       "unchanged, zero main transport events; main frames"
                    << mainWrittenFrames.load();
            m_stage = 6;
            m_player->stop(); // Outside the measured preview-only interval.
            m_finishWait.start();
        }
        else if(m_stage == 6 && m_finishWait.elapsed() >= 500) {
            m_timer.stop();
            QCoreApplication::quit();
        }
    }

    bool m_enabled {false};
    bool m_closed {false};
    int m_stage {0};
    PlayerController* m_player {nullptr};
    EngineController* m_engine {nullptr};
    PlaylistHandler* m_playlists {nullptr};
    SettingsManager* m_settings {nullptr};
    ActionManager* m_actions {nullptr};
    TrackSelectionController* m_selection {nullptr};
    WidgetProvider* m_widgets {nullptr};
    std::shared_ptr<AudioLoader> m_loader;
    QPointer<FyWidget> m_panel;
    std::unique_ptr<QWidget> m_selectionView;
    std::unique_ptr<WidgetContext> m_selectionContext;
    QTimer m_timer;
    QElapsedTimer m_elapsed, m_pauseWait, m_finishWait;
    Track m_main, m_preview, m_other;
    PlaybackQueueSnapshot m_queue;
    std::vector<std::pair<UId, TrackList>> m_playlistSnapshot;
    QByteArray m_headId;
    double m_mainVolume {0};
    QString m_mainOutput;
    uint64_t m_pausePosition {0};
    uint64_t m_pauseFrames {0};
    std::unique_ptr<QSignalSpy> m_transport, m_pauses, m_stops, m_tracks, m_outputs;
    std::unique_ptr<QSignalSpy> m_devices, m_seeks, m_playStates, m_trackPlayed;
    int m_volumeEvents {0}, m_outputSettingEvents {0};
};

#include "inprocessdriver.moc"
