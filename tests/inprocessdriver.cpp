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
#include <gui/widgets/elidedlabel.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/command.h>
#include <utils/settings/settingsmanager.h>
#include <utils/stringutils.h>

#include <QApplication>
#include <QAudioSink>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QLabel>
#include <QMediaDevices>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

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
        m_player        = context.playerController;
        m_engine        = context.engine;
        m_loader        = context.audioLoader;
        m_playlists     = context.playlistHandler;
        m_settings      = context.settingsManager;
        m_stressSeconds = qEnvironmentVariableIntValue("HEADPHONES_TEST_STRESS_SECONDS");
        // The opt-in driver uses a fresh isolated configuration. Suppress the
        // host's modal first-run wizard so it cannot intercept test shortcuts.
        m_settings->set<Settings::Core::FirstRun>(false);
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
        // Observe the individual widgets' public UI rather than relying on the
        // removed combined widget's status label. Require clock progress before
        // treating a new preview as playing, so loading is not mistaken for playback.
        if(!m_actions->command("Headphones.Playback.Pause")->action()->isEnabled()) {
            return m_actions->command("Headphones.Playback.Stop")->action()->isEnabled() ? u"Error"_s : u"Stopped"_s;
        }
        if(m_panel->findChild<QToolButton*>(u"Headphones.PlayPause"_s)->toolTip() == u"Play headphone preview") {
            return u"Paused"_s;
        }
        auto* seek = m_panel->findChild<QSlider*>(u"Headphones.Seek"_s);
        return seek->isEnabled() && seek->value() > 0 ? u"Playing"_s : u"Loading"_s;
    }
    QWidget* createPreviewWidgets()
    {
        // A test-only container exercises the actual separately registered widgets.
        auto panel = std::make_unique<QWidget>();
        auto* layout = new QVBoxLayout(panel.get());
        const QStringList ids {u"HeadphonesPlayerControls"_s, u"HeadphonesSeekBar"_s,
                               u"HeadphonesVolumeControls"_s, u"HeadphonesOutputDevice"_s, u"HeadphonesTrackInfo"_s};
        for(const auto& id : ids) {
            auto* widget = m_widgets->createWidget(id);
            if(!check(widget, u"Actual plugin widget was not created: %1"_s.arg(id))) {
                return nullptr;
            }
            widget->loadLayoutData({{u"Orientation"_s, Qt::Horizontal}, {u"ShowConfigure"_s, true}});
            layout->addWidget(widget);
        }
        return panel.release();
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
        // Extended playback legitimately reaches fooyin's main-track play-count
        // threshold. Only the main fixture may produce such an event; preview
        // tracks must never enter this host accounting path.
        for(const auto& arguments : *m_trackPlayed) {
            if(!check(qvariant_cast<Track>(arguments.front()).filepath() == m_main.filepath(),
                      u"Preview track emitted a main-player trackPlayed event"_s)) {
                return false;
            }
        }
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
                         && m_playStates->isEmpty() && m_volumeEvents == 0 && m_outputSettingEvents == 0,
                     u"Preview emitted main transport/track/output events"_s);
    }
    void previewMenu(const Track& track)
    {
        auto menu = std::make_unique<QMenu>();
        TrackSelection selection;
        selection.tracks = {track, m_other};
        m_selection->addTrackContextMenu(menu.get(), selection);
        auto* action = menu->findChild<QAction*>(u"Headphones.Tracks.PreviewSelected.Context"_s);
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
        if(!check(m_stressSeconds >= 0 && m_stressSeconds <= 3600, u"Stress duration must be 0..3600 seconds"_s)
           || !check(m_elapsed.elapsed() < 45000 + m_stressSeconds * 1000, u"Validation timeout"_s)) {
            return;
        }
        if(m_stage >= 30 && m_stage <= 34 && !mainUnchanged()) {
            return;
        }
        if(m_stage == 0) {
            if(!m_widgets->widgetExists(u"HeadphonesPlayerControls"_s)) {
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
            if(!check(m_main.duration() >= static_cast<uint64_t>(m_stressSeconds + 20) * 1000
                          && m_preview.duration() >= 20000 && m_other.duration() >= 20000,
                      u"Fixtures too short for validation duration"_s)) {
                return;
            }
            m_main.setTitle(u"Main in-process"_s);
            m_preview.setTitle(u"Preview A"_s);
            m_other.setTitle(u"Preview B"_s);
            m_settings->set(u"Headphones/DeviceDescription", u"Explicit validation headphones"_s);
            m_settings->set(u"Headphones/DeviceId", QString::fromLatin1(m_headId.toBase64()));
            m_panel = createPreviewWidgets();
            if(!m_panel) {
                return;
            }
            m_panel->show();
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
               || !check(m_panel->findChild<ElidedLabel*>(u"Headphones.Title"_s)->text().contains(u"Preview A"),
                         u"Context-menu snapshot target changed"_s)) {
                return;
            }
            auto* command = m_actions->command("Headphones.Playback.PlayPause");
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
            // A software sink can have a longer server-side startup prebuffer
            // than the requested QAudioSink buffer. Wait for actual main output
            // progress instead of assuming it must refill within 300 ms.
            if(m_player->currentPosition() <= m_pausePosition || mainWrittenFrames <= m_pauseFrames) {
                return;
            }
            if(!mainUnchanged()
               || !check(status() == u"Paused" && m_player->currentPosition() > m_pausePosition
                             && mainWrittenFrames > m_pauseFrames,
                         u"Shortcut/main progress: preview=%1 main position=%2->%3 frames=%4->%5"_s.arg(status())
                             .arg(m_pausePosition)
                             .arg(m_player->currentPosition())
                             .arg(m_pauseFrames)
                             .arg(mainWrittenFrames.load()))) {
                return;
            }
            m_panel->findChild<QSlider*>(u"Headphones.Volume"_s)->setValue(61);
            auto* seek = m_panel->findChild<QSlider*>(u"Headphones.Seek"_s);
            seek->setValue(seek->maximum() / 2);
            m_stage = 4;
        }
        else if(m_stage == 4) {
            if(status() != u"Paused") {
                return;
            }
            if(!mainUnchanged()
               || !check(m_panel->findChild<QLabel*>(u"Headphones.Elapsed"_s)->text() == Utils::msToString(10000),
                         u"Paused seek was not applied"_s)) {
                return;
            }
            trigger("Headphones.Playback.PlayPause");
            delete m_panel.data();
            m_panel = createPreviewWidgets();
            if(!m_panel) {
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
                          u"Recreated volume controls lost volume"_s)) {
                return;
            }
            m_panel->findChild<QToolButton*>(u"Headphones.Configure"_s)->click();
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
            auto* command = m_actions->command("Headphones.Tracks.PreviewSelected");
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
               || !check(m_panel->findChild<ElidedLabel*>(u"Headphones.Title"_s)->text().contains(u"Preview B"),
                         u"Focused preview shortcut targeted the wrong track"_s)) {
                return;
            }
            trigger("Headphones.Playback.Stop");
            m_panel->activateWindow();
            QTest::mouseClick(m_panel->findChild<QToolButton*>(u"Headphones.Preview"_s), Qt::LeftButton);
            m_stage = 9;
        }
        else if(m_stage == 9) {
            if(status() != u"Playing") {
                return;
            }
            if(!mainUnchanged()
               || !check(m_panel->findChild<ElidedLabel*>(u"Headphones.Title"_s)->text().contains(u"Preview B"),
                          u"Player controls lost the retained display selection"_s)) {
                return;
            }
            m_stressWait.start();
            m_stage = 30;
        }
        else if(m_stage == 30) {
            if(m_stressWait.elapsed() >= m_stressSeconds * 1000) {
                qInfo() << "HEADPHONES ISOLATION PASS: main track/playlist/queue/output/volume unchanged, "
                           "zero main transport events; main frames"
                        << mainWrittenFrames.load() << "stress cycles" << m_stressCycles << "max command-to-progress ms"
                        << m_maxStartMs << "normal main trackPlayed events" << m_trackPlayed->size();
                // Outside the measured preview-only interval, exercise independence
                // in the opposite direction and quit with preview still active.
                trigger("Headphones.Playback.PlayPause");
                if(status() == u"Paused") {
                    trigger("Headphones.Playback.PlayPause");
                }
                m_player->pause();
                m_stage = 40;
                m_finishWait.start();
                return;
            }
            previewMenu(m_stressCycles % 2 == 0 ? m_preview : m_other);
            m_actionWait.start();
            m_stage = 31;
        }
        else if(m_stage == 31) {
            if(status() != u"Playing") {
                return;
            }
            m_maxStartMs = std::max(m_maxStartMs, m_actionWait.elapsed());
            trigger("Headphones.Playback.PlayPause");
            trigger("Headphones.Seek.Forward.Small");
            trigger("Headphones.Seek.Backward.Small");
            trigger("Headphones.Seek.Forward.Small");
            m_stage = 32;
        }
        else if(m_stage == 32) {
            if(status() != u"Paused") {
                return;
            }
            m_panel->findChild<QSlider*>(u"Headphones.Volume"_s)->setValue(20 + m_stressCycles % 60);
            trigger("Headphones.Playback.PlayPause");
            m_stage = 33;
        }
        else if(m_stage == 33) {
            if(status() != u"Playing") {
                return;
            }
            // Allow real output to consume PCM between seek/replacement bursts.
            m_finishWait.start();
            m_stage = 34;
        }
        else if(m_stage == 34 && m_finishWait.elapsed() >= 200) {
            trigger("Headphones.Playback.Stop");
            if(!check(status() == u"Stopped", u"Stress stop was not synchronous"_s)) {
                return;
            }
            ++m_stressCycles;
            m_stage = 30;
        }
        else if(m_stage == 40 && m_finishWait.elapsed() >= 1200) {
            if(!check(m_player->playState() == Player::PlayState::Paused && status() == u"Playing",
                      u"Main pause interrupted preview"_s)) {
                return;
            }
            m_previewPosition = m_panel->findChild<QSlider*>(u"Headphones.Seek"_s)->value();
            m_player->stop();
            m_finishWait.start();
            m_stage = 41;
        }
        else if(m_stage == 41 && m_finishWait.elapsed() >= 1200) {
            if(!check(m_player->playState() == Player::PlayState::Stopped && status() == u"Playing",
                      u"Main stop interrupted preview"_s)) {
                return;
            }
            // Use the high-resolution timeline rather than whole-second labels;
            // allow the software sink's startup prebuffer to drain before progress.
            if(m_panel->findChild<QSlider*>(u"Headphones.Seek"_s)->value() <= m_previewPosition) {
                return;
            }
            qInfo() << "HEADPHONES SAME-PROCESS PASS: actual plugin workflow, main isolation, "
                       "preview continues with main paused/stopped; quitting with active preview";
            m_timer.stop();
            QCoreApplication::quit();
        }
    }

    bool m_enabled {false};
    bool m_closed {false};
    int m_stage {0};
    int m_stressSeconds {0}, m_stressCycles {0};
    qint64 m_maxStartMs {0};
    PlayerController* m_player {nullptr};
    EngineController* m_engine {nullptr};
    PlaylistHandler* m_playlists {nullptr};
    SettingsManager* m_settings {nullptr};
    ActionManager* m_actions {nullptr};
    TrackSelectionController* m_selection {nullptr};
    WidgetProvider* m_widgets {nullptr};
    std::shared_ptr<AudioLoader> m_loader;
    QPointer<QWidget> m_panel;
    std::unique_ptr<QWidget> m_selectionView;
    std::unique_ptr<WidgetContext> m_selectionContext;
    QTimer m_timer;
    QElapsedTimer m_elapsed, m_pauseWait, m_finishWait;
    QElapsedTimer m_stressWait, m_actionWait;
    int m_previewPosition {0};
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
