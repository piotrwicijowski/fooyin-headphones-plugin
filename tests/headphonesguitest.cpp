// SPDX-License-Identifier: GPL-3.0-or-later
#include "fakes/audiosinkfakes.h"
#include "fakes/previewfakes.h"
#include "headphonesactions.h"
#include "headphonesoutputdevice.h"
#include "headphonessettings.h"
#include "headphonessettingsdialog.h"
#include "headphonesplayercontrol.h"
#include "headphonesseekbar.h"
#include "headphonestrackinfo.h"
#include "headphonesvolumecontrol.h"

#include <gui/guisettings.h>
#include <gui/trackselectioncontroller.h>
#include <gui/widgets/elidedlabel.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/command.h>
#include <utils/settings/settingsmanager.h>
#include <utils/stringutils.h>

#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QLabel>
#include <QBoxLayout>
#include <QJsonObject>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include <limits>

using namespace Qt::StringLiterals;
using namespace Fooyin;
using namespace Fooyin::Headphones;
using namespace Fooyin::Headphones::Tests;

class HeadphonesGuiTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void additionalCommandsRegisterAndShareVolumeWithoutOutput()
    {
        QTemporaryDir dir;
        SettingsManager settings {dir.filePath(u"settings.ini"_s)};
        QMainWindow window;
        ActionManager manager {&settings};
        manager.setMainWindow(&window);
        PreviewHarness h;
        h.create();
        h.controller->setOutput(nullptr);
        const auto generation = h.controller->snapshot().generation;
        const QStringList ids {u"Headphones.Volume.Up"_s, u"Headphones.Volume.Down"_s,
                               u"Headphones.Volume.Mute"_s, u"Headphones.Volume.Unmute"_s,
                               u"Headphones.Volume.ToggleMute"_s, u"Headphones.Configure"_s,
                               u"Headphones.Tracks.FocusPreviewed"_s};
        {
            HeadphonesActions actions {*h.controller, &manager};
            for(const auto& id : ids) {
                auto* command = manager.command(Id {id});
                QVERIFY(command);
                QVERIFY(command->defaultShortcuts().isEmpty());
                QVERIFY(!command->description().isEmpty());
                QStringList categories {u"Headphones Preview"_s};
                if(id.startsWith(u"Headphones.Volume.")) {
                    categories.append(u"Headphone volume"_s);
                }
                else if(id == u"Headphones.Tracks.FocusPreviewed") {
                    categories.append(u"Headphone tracks"_s);
                }
                QCOMPARE(command->categories(), categories);
            }
            QVERIFY(!manager.command("Headphones.Tracks.FocusPreviewed")->action()->isEnabled());
            const auto invoke = [&manager](const char* id) { manager.command(id)->action()->trigger(); };
            h.controller->setVolume(0.0);
            invoke("Headphones.Volume.Unmute");
            QCOMPARE(h.controller->snapshot().volume, 0.25); // Existing default restoration policy.
            for(int i = 0; i < 5; ++i) {
                invoke("Headphones.Volume.Down");
            }
            QCOMPARE(h.controller->snapshot().volume, 0.0);
            invoke("Headphones.Volume.Down");
            QCOMPARE(h.controller->snapshot().volume, 0.0);
            invoke("Headphones.Volume.Up");
            QCOMPARE(h.controller->snapshot().volume, 0.05);
            for(int i = 0; i < 25; ++i) {
                invoke("Headphones.Volume.Up");
            }
            QCOMPARE(h.controller->snapshot().volume, 1.0);
            h.controller->setVolume(0.37);
            invoke("Headphones.Volume.Unmute");
            QCOMPARE(h.controller->snapshot().volume, 0.37);
            HeadphonesVolumeControl first {*h.controller, nullptr};
            HeadphonesVolumeControl second {*h.controller, nullptr};
            invoke("Headphones.Volume.Mute");
            invoke("Headphones.Volume.Mute");
            QCOMPARE(first.findChild<QSlider*>(u"Headphones.Volume"_s)->value(), 0);
            QCOMPARE(second.findChild<QSlider*>(u"Headphones.Volume"_s)->value(), 0);
            second.findChild<QToolButton*>(u"Headphones.Mute"_s)->click();
            QCOMPARE(h.controller->snapshot().volume, 0.37);
            invoke("Headphones.Volume.ToggleMute");
            invoke("Headphones.Volume.Unmute");
            invoke("Headphones.Volume.Unmute");
            QCOMPARE(first.findChild<QSlider*>(u"Headphones.Volume"_s)->value(), 37);
            invoke("Headphones.Volume.ToggleMute");
            invoke("Headphones.Volume.ToggleMute");
            QCOMPARE(h.controller->snapshot().volume, 0.37);
            QCOMPARE(h.controller->snapshot().state, PreviewState::Stopped);
            QCOMPARE(h.controller->snapshot().generation, generation);
        }
        for(const auto& id : ids) {
            QVERIFY(!manager.command(Id {id}));
        }
    }

    void configureCommandSharesDialogAndGuardsProviderLifetime()
    {
        QTemporaryDir dir;
        SettingsManager manager {dir.filePath(u"settings.ini"_s)};
        HeadphonesSettings settings {&manager};
        FakeDeviceCatalogue catalogue;
        auto provider = std::make_shared<HeadphonesSettingsProvider>(settings, catalogue);
        auto host = settingsProviderAdapter(provider);
        PreviewHarness h;
        h.create();
        h.controller->setOutput(nullptr);
        HeadphonesActions actions {*h.controller, nullptr, nullptr, provider};
        auto* configure = actions.findChild<QAction*>(u"Headphones.Configure"_s);
        QVERIFY(configure && configure->isEnabled());
        configure->trigger(); // No widgets or output required.
        QPointer<QDialog> dialog {provider->createSettings(nullptr)};
        QVERIFY(dialog && dialog->isVisible());
        dialog->hide();
        host->showSettings(nullptr);
        QCOMPARE(provider->createSettings(nullptr), dialog.data());
        dialog->hide();
        configure->trigger();
        QVERIFY(dialog->isVisible());
        provider.reset();
        QVERIFY(dialog.isNull());
        configure->trigger(); // Expired provider cannot reopen settings or access deleted services.
        host->showSettings(nullptr);
        QCOMPARE(h.controller->snapshot().state, PreviewState::Stopped);
    }

    void unavailablePlayerControlsStillOpenConfiguration()
    {
        PreviewHarness h;
        h.create();
        h.controller->setOutput(nullptr);
        HeadphonesActions actions {*h.controller, nullptr};
        HeadphonesPlayerControl controls {*h.controller, actions, nullptr};
        controls.loadLayoutData({{u"ShowPause"_s, true}, {u"ShowPlay"_s, true}});
        controls.show();
        for(const auto& name : {u"Headphones.Stop"_s, u"Headphones.Pause"_s, u"Headphones.Play"_s,
                                u"Headphones.PlayPause"_s, u"Headphones.Preview"_s}) {
            auto* button = controls.findChild<QToolButton*>(name);
            QVERIFY(button);
            QVERIFY(!button->isEnabled());
            const auto pos = button->rect().center();
            QContextMenuEvent event {QContextMenuEvent::Mouse, pos, button->mapToGlobal(pos)};
            QApplication::sendEvent(button, &event);
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            QVERIFY(menu);
            QVERIFY(menu->isVisible());
            QCOMPARE(menu->actions().front()->text(), u"Show Stop"_s);
            menu->close();
        }
        QCOMPARE(h.controller->snapshot().state, PreviewState::Stopped);
    }

    void idleSeekbarLabelsResetToZero_data()
    {
        QTest::addColumn<bool>("remaining");
        QTest::newRow("total") << false;
        QTest::newRow("remaining") << true;
    }

    void idleSeekbarLabelsResetToZero()
    {
        QFETCH(bool, remaining);
        PreviewHarness h;
        h.add(u"preview.wav"_s);
        h.add(u"unknown.wav"_s);
        h.create();
        HeadphonesSeekBar bar {*h.controller, nullptr};
        bar.loadLayoutData({{u"ShowRemainingTime"_s, remaining}});
        auto* elapsed = bar.findChild<QLabel*>(u"Headphones.Elapsed"_s);
        auto* total = bar.findChild<QLabel*>(u"Headphones.Total"_s);
        QVERIFY(elapsed && total);
        QCOMPARE(elapsed->text(), Utils::msToString(0));
        QCOMPARE(total->text(), Utils::msToString(0));
        Track track {u"preview.wav"_s};
        track.setDuration(10000);
        h.controller->previewTrack(track);
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
        h.controller->playPause();
        QCOMPARE(total->text(), (remaining ? u"-"_s : QString {}) + Utils::msToString(10000));
        h.controller->stop();
        QCOMPARE(elapsed->text(), Utils::msToString(0));
        QCOMPARE(total->text(), Utils::msToString(0));
        h.controller->setOutput(nullptr);
        h.controller->previewTrack(track);
        QCOMPARE(h.controller->snapshot().state, PreviewState::Error);
        QCOMPARE(elapsed->text(), Utils::msToString(0));
        QCOMPARE(total->text(), Utils::msToString(0));
        h.controller->setOutput(std::make_unique<FakeOutput>(h.output));
        h.controller->previewTrack(Track {u"unknown.wav"_s});
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
        QCOMPARE(total->text(), u"unknown"_s);
        h.controller->stop();
        QCOMPARE(total->text(), Utils::msToString(0));
    }

    void separateControlsUsePreviewActionsAndPersistConfiguration()
    {
        PreviewHarness h;
        h.add(u"preview.wav"_s);
        h.create();
        HeadphonesActions actions {*h.controller, nullptr};
        HeadphonesPlayerControl controls {*h.controller, actions, nullptr};
        controls.loadLayoutData({{u"Orientation"_s, Qt::Vertical}, {u"ShowStop"_s, false}});
        controls.show();
        auto* stop = controls.findChild<QToolButton*>(u"Headphones.Stop"_s);
        auto* playPause = controls.findChild<QToolButton*>(u"Headphones.PlayPause"_s);
        QVERIFY(stop && playPause);
        QVERIFY(stop->isHidden());
        QCOMPARE(stop->defaultAction(), actions.stopAction());
        QCOMPARE(playPause->defaultAction(), actions.playPauseAction());
        QCOMPARE(static_cast<QBoxLayout*>(controls.layout())->direction(), QBoxLayout::TopToBottom);
        QJsonObject saved;
        controls.saveLayoutData(saved);
        HeadphonesPlayerControl restored {*h.controller, actions, nullptr};
        restored.loadLayoutData(saved);
        QJsonObject roundtrip;
        restored.saveLayoutData(roundtrip);
        QCOMPARE(roundtrip, saved);
        QMenu menu;
        controls.populateContextMenu(&menu);
        menu.actions().front()->trigger();
        QVERIFY(!stop->isHidden());
        QVERIFY(!playPause->isEnabled());
        h.controller->previewTrack(Track {u"preview.wav"_s});
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
        QVERIFY(playPause->isEnabled());
        playPause->click();
        QCOMPARE(h.controller->snapshot().state, PreviewState::Paused);
        playPause->click();
        QCOMPARE(h.controller->snapshot().state, PreviewState::Playing);
        stop->click();
        QCOMPARE(h.controller->snapshot().state, PreviewState::Stopped);
    }

    void separatePlayAndPauseAreOptionalAndHaveDistinctActions()
    {
        PreviewHarness h;
        auto source = h.add(u"preview.wav"_s);
        source->openGate = std::make_shared<Gate>();
        h.create();
        HeadphonesActions actions {*h.controller, nullptr};
        HeadphonesPlayerControl controls {*h.controller, actions, nullptr};
        auto* play = controls.findChild<QToolButton*>(u"Headphones.Play"_s);
        auto* pause = controls.findChild<QToolButton*>(u"Headphones.Pause"_s);
        QVERIFY(play && pause);
        QVERIFY(play->isHidden());
        QVERIFY(pause->isHidden());
        // Existing layouts without the new keys retain the default visibility.
        controls.loadLayoutData({{u"ShowPlayPause"_s, true}});
        QVERIFY(play->isHidden());
        QVERIFY(pause->isHidden());
        QMenu menu;
        controls.populateContextMenu(&menu);
        for(auto* action : menu.actions()) {
            if(action->text() == u"Show Play" || action->text() == u"Show Pause") {
                action->trigger();
            }
        }
        QVERIFY(!play->isHidden());
        QVERIFY(!pause->isHidden());
        QJsonObject saved;
        controls.saveLayoutData(saved);
        QVERIFY(saved.value(u"ShowPlay"_s).toBool());
        QVERIFY(saved.value(u"ShowPause"_s).toBool());
        HeadphonesPlayerControl restored {*h.controller, actions, nullptr};
        restored.loadLayoutData(saved);
        QVERIFY(!restored.findChild<QToolButton*>(u"Headphones.Play"_s)->isHidden());
        QVERIFY(!restored.findChild<QToolButton*>(u"Headphones.Pause"_s)->isHidden());
        QVERIFY(!play->isEnabled());
        QVERIFY(!pause->isEnabled());
        h.controller->previewTrack(Track {u"preview.wav"_s});
        QCOMPARE(h.controller->snapshot().state, PreviewState::Loading);
        pause->click();
        pause->click();
        QVERIFY(h.controller->snapshot().pauseRequested);
        play->click();
        play->click();
        QVERIFY(!h.controller->snapshot().pauseRequested);
        pause->click();
        source->openGate->release();
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);
        const auto generation = h.controller->snapshot().generation;
        play->click();
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
        QCOMPARE(h.controller->snapshot().generation, generation);
        play->click(); // Like the main Play button, restart an already playing track.
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
        QCOMPARE(h.controller->snapshot().generation, generation + 1);
        pause->click();
        pause->click();
        QCOMPARE(h.controller->snapshot().state, PreviewState::Paused);
        h.controller->stop();
        QVERIFY(!pause->isEnabled());
        QVERIFY(play->isEnabled());
        play->click();
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
    }

    void playerControlsPreviewDisplaySelectionAndSharePluginSettings()
    {
        QTemporaryDir dir;
        SettingsManager manager {dir.filePath(u"settings.ini"_s)};
        HeadphonesSettings settings {&manager};
        FakeDeviceCatalogue catalogue;
        auto provider = std::make_shared<HeadphonesSettingsProvider>(settings, catalogue);
        PreviewHarness h;
        auto source = h.add(u"preview.wav"_s);
        h.add(u"focused.wav"_s);
        h.create();
        HeadphonesActions actions {*h.controller, nullptr};
        HeadphonesPlayerControl controls {*h.controller, actions, nullptr, provider};
        auto* preview = controls.findChild<QToolButton*>(u"Headphones.Preview"_s);
        auto* configure = controls.findChild<QToolButton*>(u"Headphones.Configure"_s);
        QVERIFY(preview && configure);
        QVERIFY(!preview->isHidden());
        QVERIFY(!preview->isEnabled());
        QVERIFY(configure->isHidden());
        // Old layouts also get the requested default visibility.
        controls.loadLayoutData({});
        QVERIFY(!preview->isHidden());
        QVERIFY(configure->isHidden());
        actions.setSelection({Track {u"focused.wav"_s}}, {Track {u"preview.wav"_s}});
        QVERIFY(preview->isEnabled());
        preview->click();
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
        QCOMPARE(h.controller->snapshot().track.filepath(), u"preview.wav"_s);
        actions.setSelection({}, {Track {u"preview.wav"_s}});
        QVERIFY(preview->isEnabled());
        preview->click();
        QTRY_COMPARE(source->opens.load(), 2);
        actions.setSelection({}, {Track {u"https://example.com/live"_s}, Track {u"preview.wav"_s}});
        QVERIFY(!preview->isEnabled());
        QVERIFY(!preview->toolTip().isEmpty());
        actions.setSelection({}, {});
        QVERIFY(!preview->isEnabled());
        QMenu menu;
        controls.populateContextMenu(&menu);
        for(auto* action : menu.actions()) {
            if(action->text() == u"Show Preview Selected" || action->text() == u"Show Configure") {
                action->trigger();
            }
        }
        QVERIFY(preview->isHidden());
        QVERIFY(!configure->isHidden());
        QJsonObject saved;
        controls.saveLayoutData(saved);
        HeadphonesPlayerControl restored {*h.controller, actions, nullptr, provider};
        restored.loadLayoutData(saved);
        QVERIFY(restored.findChild<QToolButton*>(u"Headphones.Preview"_s)->isHidden());
        QVERIFY(!restored.findChild<QToolButton*>(u"Headphones.Configure"_s)->isHidden());
        h.controller->stop();
        auto* dialog = provider->createSettings(nullptr);
        auto host = settingsProviderAdapter(provider);
        configure->click();
        QVERIFY(dialog->isVisible());
        QCOMPARE(provider->createSettings(nullptr), dialog);
        dialog->hide();
        host->showSettings(nullptr);
        QCOMPARE(provider->createSettings(nullptr), dialog);
        QVERIFY(dialog->isVisible());
        QCOMPARE(h.controller->snapshot().state, PreviewState::Stopped);
        h.controller->setOutput(nullptr);
        actions.setSelection({}, {Track {u"preview.wav"_s}});
        QVERIFY(!preview->isEnabled());
    }

    void outputDeviceWidgetsSynchroniseAndRetainMissingDevices()
    {
        QTemporaryDir dir;
        SettingsManager manager {dir.filePath(u"settings.ini"_s)};
        HeadphonesSettings settings {&manager};
        settings.setDevice("missing", u"Saved headphones"_s);
        FakeDeviceCatalogue catalogue;
        catalogue.entries = {fakeDevice("a"), fakeDevice("b")};
        QSignalSpy changed {&settings, &HeadphonesSettings::deviceChanged};
        HeadphonesOutputDevice widget {settings, catalogue};
        HeadphonesOutputDevice other {settings, catalogue};
        auto* combo = widget.findChild<QComboBox*>(u"Headphones.DeviceChoice"_s);
        auto* otherCombo = other.findChild<QComboBox*>(u"Headphones.DeviceChoice"_s);
        QVERIFY(combo && otherCombo);
        QCOMPARE(combo->currentData().toByteArray(), QByteArray {"missing"});
        QVERIFY(combo->currentText().contains(u"Unavailable"));
        QVERIFY(combo->itemText(1) != combo->itemText(2)); // Duplicate names have distinct IDs.
        QCOMPARE(changed.count(), 0);
        const auto activate = [combo](const QByteArray& id) {
            const auto index = combo->findData(id);
            combo->setCurrentIndex(index);
            combo->activated(index);
        };
        activate("b");
        QCOMPARE(settings.deviceId(), QByteArray {"b"});
        QCOMPARE(settings.deviceDescription(), catalogue.entries[1].description);
        QCOMPARE(otherCombo->currentData().toByteArray(), QByteArray {"b"});
        QCOMPARE(changed.count(), 1);
        catalogue.change({fakeDevice("a")});
        QCOMPARE(combo->currentData().toByteArray(), QByteArray {"b"});
        QVERIFY(combo->currentText().contains(u"Unavailable"));
        QCOMPARE(settings.deviceId(), QByteArray {"b"});
        catalogue.change({fakeDevice("b"), fakeDevice("a")});
        QCOMPARE(combo->currentData().toByteArray(), QByteArray {"b"});
        QVERIFY(!combo->currentText().contains(u"Unavailable"));
        QCOMPARE(changed.count(), 1); // Refresh/hotplug never applies another output.
        settings.setDevice("a", catalogue.entries[1].description);
        QCOMPARE(combo->currentData().toByteArray(), QByteArray {"a"});
        QCOMPARE(otherCombo->currentData().toByteArray(), QByteArray {"a"});
        activate({});
        QVERIFY(settings.deviceId().isEmpty());
        QVERIFY(otherCombo->currentData().toByteArray().isEmpty());
        QCOMPARE(combo->currentText(), u"Not configured"_s);
    }

    void trackInfoFollowsPreviewAndSurvivesRecreation()
    {
        PreviewHarness h;
        h.add(u"preview.wav"_s);
        h.add(u"other.wav"_s);
        h.create();
        HeadphonesActions actions {*h.controller, nullptr};
        HeadphonesTrackInfo info {*h.controller};
        auto* title = info.findChild<ElidedLabel*>(u"Headphones.Title"_s);
        QVERIFY(title);
        QCOMPARE(title->text(), u"No headphone preview"_s);
        Track track {u"preview.wav"_s};
        track.setTitle(u"<b>Title</b>"_s);
        track.setArtists({u"Artist"_s});
        actions.setSelection({}, {track});
        QCOMPARE(title->text(), u"No headphone preview"_s);
        actions.previewDisplaySelection();
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
        QCOMPARE(title->text(), u"Artist - <b>Title</b>"_s);
        QCOMPARE(title->textFormat(), Qt::PlainText);
        actions.setSelection({}, {Track {u"other.wav"_s}});
        QCOMPARE(title->text(), u"Artist - <b>Title</b>"_s);
        h.controller->stop();
        const auto generation = h.controller->snapshot().generation;
        HeadphonesTrackInfo recreated {*h.controller};
        QCOMPARE(recreated.findChild<ElidedLabel*>(u"Headphones.Title"_s)->text(), title->text());
        QCOMPARE(h.controller->snapshot().generation, generation);
        actions.previewDisplaySelection();
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
        QCOMPARE(title->text(), Track {u"other.wav"_s}.effectiveTitle());
    }

    void separateSeekbarUsesAbsolutePositionAndCancelsDrag_data()
    {
        QTest::addColumn<int>("orientation");
        QTest::newRow("horizontal") << int(Qt::Horizontal);
        QTest::newRow("vertical") << int(Qt::Vertical);
    }

    void separateSeekbarUsesAbsolutePositionAndCancelsDrag()
    {
        QFETCH(int, orientation);
        PreviewHarness h;
        h.add(u"preview.wav"_s);
        h.create();
        HeadphonesSeekBar bar {*h.controller, nullptr};
        bar.loadLayoutData({{u"Orientation"_s, orientation}, {u"ShowRemainingTime"_s, true}});
        bar.resize(orientation == Qt::Horizontal ? QSize {500, 40} : QSize {90, 500});
        bar.show();
        auto* slider = bar.findChild<QSlider*>(u"Headphones.Seek"_s);
        QVERIFY(slider);
        QVERIFY(!slider->isEnabled());
        Track track {u"preview.wav"_s};
        track.setDuration(10000);
        h.controller->previewTrack(track);
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
        h.controller->playPause();
        QVERIFY(slider->isEnabled());
        QCOMPARE(int(slider->orientation()), orientation);
        QTest::qWait(1); // Activate layout before choosing a physical click position.
        const auto pointAt = [slider, orientation](double ratio) {
            return orientation == Qt::Horizontal ? QPoint {int(slider->width() * ratio), slider->height() / 2}
                                                 : QPoint {slider->width() / 2, int(slider->height() * ratio)};
        };
        auto generation = h.controller->snapshot().generation;
        QTest::mouseClick(slider, Qt::LeftButton, Qt::NoModifier, pointAt(0.75));
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);
        QCOMPARE(h.controller->snapshot().generation, generation + 1);
        QVERIFY(h.controller->snapshot().positionMs > 7000);
        QVERIFY(h.controller->snapshot().positionMs < 8000);
        QVERIFY(bar.findChild<QLabel*>(u"Headphones.Total"_s)->text().startsWith(u'-'));
        generation = h.controller->snapshot().generation;
        QTest::mousePress(slider, Qt::LeftButton, Qt::NoModifier, pointAt(0.25));
        QVERIFY(slider->isSliderDown());
        h.controller->setVolume(0.42); // A snapshot during a drag must not seek or reset the tentative value.
        QCOMPARE(h.controller->snapshot().generation, generation);
        QVERIFY(slider->value() < slider->maximum() / 2);
        QTest::mouseClick(slider, Qt::RightButton, Qt::NoModifier, pointAt(0.25));
        QTest::mouseRelease(slider, Qt::LeftButton, Qt::NoModifier, pointAt(0.25));
        QVERIFY(!slider->isSliderDown());
        QCOMPARE(h.controller->snapshot().generation, generation);
        QVERIFY(slider->value() > slider->maximum() / 2);
        QTest::keyClick(slider, Qt::Key_Left);
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);
        QVERIFY(h.controller->snapshot().positionMs > 2000);
        QVERIFY(h.controller->snapshot().positionMs < 3000);
        QJsonObject saved;
        bar.saveLayoutData(saved);
        QVERIFY(!saved.contains(u"ShowPlayedThreshold"_s));
        saved[u"ShowLabels"_s] = false;
        HeadphonesSeekBar restored {*h.controller, nullptr};
        restored.loadLayoutData(saved);
        QVERIFY(restored.findChild<QLabel*>(u"Headphones.Elapsed"_s)->isHidden());
        QJsonObject roundtrip;
        restored.saveLayoutData(roundtrip);
        QCOMPARE(roundtrip, saved);
        h.controller->stop();
        QVERIFY(!slider->isEnabled());
    }

    void separateVolumeSetsClickedValueAndSharesMute()
    {
        PreviewHarness h;
        h.create();
        h.controller->setVolume(0.2);
        HeadphonesVolumeControl volume {*h.controller, nullptr};
        HeadphonesVolumeControl other {*h.controller, nullptr};
        volume.loadLayoutData({{u"Orientation"_s, Qt::Horizontal}});
        volume.resize(500, 40);
        volume.show();
        QTest::qWait(1);
        auto* slider = volume.findChild<QSlider*>(u"Headphones.Volume"_s);
        auto* otherSlider = other.findChild<QSlider*>(u"Headphones.Volume"_s);
        auto* mute = volume.findChild<QToolButton*>(u"Headphones.Mute"_s);
        QVERIFY(slider && otherSlider && mute);
        QTest::mousePress(slider, Qt::LeftButton, Qt::NoModifier,
                          QPoint {slider->width() * 3 / 4, slider->height() / 2});
        QVERIFY(h.controller->snapshot().volume > 0.7);
        QVERIFY(h.controller->snapshot().volume < 0.8);
        QTest::mouseRelease(slider, Qt::LeftButton, Qt::NoModifier,
                            QPoint {slider->width() * 3 / 4, slider->height() / 2});
        const auto previous = h.controller->snapshot().volume;
        QCOMPARE(otherSlider->value(), slider->value());
        mute->click();
        QCOMPARE(h.controller->snapshot().volume, 0.0);
        QCOMPARE(otherSlider->value(), 0);
        QVERIFY(mute->isChecked());
        // Mute restoration belongs to the session, even for a newly created widget.
        HeadphonesVolumeControl recreated {*h.controller, nullptr};
        recreated.findChild<QToolButton*>(u"Headphones.Mute"_s)->click();
        QCOMPARE(h.controller->snapshot().volume, previous);
        QVERIFY(!mute->isChecked());
        volume.loadLayoutData({{u"Orientation"_s, Qt::Vertical}, {u"ShowSlider"_s, false}});
        QCOMPARE(slider->orientation(), Qt::Vertical);
        QVERIFY(slider->window() != volume.window());
        QJsonObject saved;
        volume.saveLayoutData(saved);
        recreated.loadLayoutData(saved);
        QJsonObject roundtrip;
        recreated.saveLayoutData(roundtrip);
        QCOMPARE(roundtrip, saved);
        // Repeated mode switches must keep the same working slider alive.
        volume.loadLayoutData({{u"Orientation"_s, Qt::Vertical}, {u"ShowIcon"_s, false}});
        QVERIFY(mute->isHidden());
        QCOMPARE(slider->window(), volume.window());
        volume.resize(50, 500);
        QTest::qWait(1);
        QTest::mouseClick(slider, Qt::LeftButton, Qt::NoModifier,
                          QPoint {slider->width() / 2, slider->height() / 4});
        QVERIFY(h.controller->snapshot().volume > 0.7);
        QVERIFY(h.controller->snapshot().volume < 0.8);
        QMenu menu;
        volume.populateContextMenu(&menu);
        menu.actions()[1]->trigger(); // Cannot disable the last visible control.
        QVERIFY(menu.actions()[1]->isChecked());
        QVERIFY(!slider->isHidden());
    }

    void actionsUseMenuSnapshotAndFirstTrack()
    {
        QTemporaryDir dir;
        SettingsManager settings {dir.filePath(u"settings.ini"_s)};
        QMainWindow mainWindow;
        ActionManager manager {&settings};
        manager.setMainWindow(&mainWindow);
        PreviewHarness h;
        auto first  = h.add(u"first.wav"_s);
        auto second = h.add(u"second.wav"_s);
        h.create();
        {
            HeadphonesActions actions {*h.controller, &manager};
            auto* command = manager.command("Headphones.Tracks.PreviewSelected");
            QVERIFY(command);
            QVERIFY(command->defaultShortcuts().isEmpty());
            QCOMPARE(command->categories(), (QStringList {u"Headphones Preview"_s, u"Headphone tracks"_s}));
            TrackSelection selected;
            selected.tracks = {Track {u"first.wav"_s}, Track {u"second.wav"_s}};
            QMenu menu;
            actions.renderMenu(&menu, selected);
            actions.setSelection({Track {u"second.wav"_s}}, selected.tracks);
            menu.actions().front()->trigger();
            QTRY_COMPARE(first->opens.load(), 1);
            QCOMPARE(second->opens.load(), 0);
            QCOMPARE(h.controller->snapshot().track.filepath(), u"first.wav"_s);
            auto* focus = actions.findChild<QAction*>(u"Headphones.Tracks.FocusPreviewed"_s);
            QVERIFY(focus && !focus->isEnabled()); // A retained track without playlist GUI services is safe.
            const auto generation = h.controller->snapshot().generation;
            focus->trigger();
            QCOMPARE(h.controller->snapshot().generation, generation);
            h.controller->stop();
            // Player controls keep the last display selection after focus moves off the view.
            actions.setSelection({}, {Track {u"second.wav"_s}});
            auto* shortcut = actions.findChild<QAction*>(u"Headphones.Tracks.PreviewSelected"_s);
            QVERIFY(!shortcut->isEnabled());
            actions.previewDisplaySelection();
            QTRY_COMPARE(second->opens.load(), 1);
            actions.setSelection({}, {});
            QVERIFY(!actions.displayPreviewReason().isEmpty());
            actions.setSelection({Track {u"https://example.com/live"_s}}, {});
            QVERIFY(!shortcut->isEnabled());
            QCOMPARE(h.controller->snapshot().track.filepath(), u"second.wav"_s);
        }
        QVERIFY(!manager.command("Headphones.Tracks.PreviewSelected"));
    }

    void seekActionsFollowPlaybackSettings_data()
    {
        QTest::addColumn<QString>("actionId");
        QTest::addColumn<bool>("forward");
        QTest::addColumn<bool>("largeStep");
        QTest::newRow("small-backward") << u"Headphones.Seek.Backward.Small"_s << false << false;
        QTest::newRow("small-forward") << u"Headphones.Seek.Forward.Small"_s << true << false;
        QTest::newRow("large-backward") << u"Headphones.Seek.Backward.Large"_s << false << true;
        QTest::newRow("large-forward") << u"Headphones.Seek.Forward.Large"_s << true << true;
    }

    void seekActionsFollowPlaybackSettings()
    {
        QFETCH(QString, actionId);
        QFETCH(bool, forward);
        QFETCH(bool, largeStep);
        QTemporaryDir dir;
        SettingsManager settings {dir.filePath(u"settings.ini"_s)};
        settings.createSetting<Settings::Gui::SeekStepSmall>(700, u"Interface/SeekIncrement"_s);
        settings.createSetting<Settings::Gui::SeekStepLarge>(4300, u"Interface/SeekIncrementLarge"_s);
        QMainWindow mainWindow;
        ActionManager manager {&settings};
        manager.setMainWindow(&mainWindow);
        PreviewHarness h;
        h.add(u"preview.wav"_s);
        auto unseekable = h.add(u"unseekable.wav"_s);
        unseekable->seekable = false;
        h.create();
        {
            HeadphonesActions actions {*h.controller, &manager, &settings};
            auto* command = manager.command(Id {actionId});
            QVERIFY(command);
            QVERIFY(command->defaultShortcuts().isEmpty());
            QCOMPARE(command->categories(), (QStringList {u"Headphones Preview"_s, u"Headphone seek"_s}));
            auto* action = actions.findChild<QAction*>(actionId);
            QVERIFY(action);
            QVERIFY(!action->isEnabled());
            Track track {u"preview.wav"_s};
            track.setDuration(30000);
            h.controller->previewTrack(track);
            QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
            h.controller->playPause();
            QVERIFY(h.controller->seek(15000));
            QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);
            QVERIFY(action->isEnabled());
            action->trigger();
            const int step = largeStep ? 4300 : 700;
            const int64_t expected = 15000 + (forward ? step : -step);
            QTRY_COMPARE(h.controller->snapshot().positionMs, expected);
            QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);

            // Read the shared settings at trigger time, without recreating the actions.
            settings.set<Settings::Gui::SeekStepSmall>(1100);
            settings.set<Settings::Gui::SeekStepLarge>(6100);
            action->trigger();
            const int updatedStep = largeStep ? 6100 : 1100;
            QTRY_COMPARE(h.controller->snapshot().positionMs, expected + (forward ? updatedStep : -updatedStep));
            QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);

            QVERIFY(h.controller->seek(forward ? 29900 : 100));
            QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);
            action->trigger();
            QTRY_COMPARE(h.controller->snapshot().positionMs, forward ? uint64_t {30000} : uint64_t {0});
            QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);
            h.controller->stop();
            QVERIFY(!action->isEnabled());
            h.controller->previewTrack(Track {u"unseekable.wav"_s});
            QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
            QVERIFY(!action->isEnabled());
        }
        QVERIFY(!manager.command(Id {actionId}));
    }

    void settingsStageApplyCancelAndRetainMissingId()
    {
        QTemporaryDir dir;
        SettingsManager manager {dir.filePath(u"settings.ini"_s)};
        HeadphonesSettings settings {&manager};
        settings.setDevice("missing", u"Old headphones"_s);
        FakeDeviceCatalogue catalogue;
        catalogue.entries = {fakeDevice("a"), fakeDevice("b")};
        HeadphonesSettingsDialog dialog {settings, catalogue};
        auto* combo   = dialog.findChild<QComboBox*>(u"Headphones.DeviceChoice"_s);
        auto* buttons = dialog.findChild<QDialogButtonBox*>(u"Headphones.SettingsButtons"_s);
        QVERIFY(combo && buttons);
        QCOMPARE(combo->currentData().toByteArray(), QByteArray {"missing"});
        QVERIFY(combo->currentText().contains(u"Unavailable"));
        QVERIFY(combo->itemText(1) != combo->itemText(2));
        combo->setCurrentIndex(combo->findData(QByteArray {"b"}));
        QCOMPARE(settings.deviceId(), QByteArray {"missing"});
        catalogue.change({fakeDevice("a")});
        QCOMPARE(combo->currentData().toByteArray(), QByteArray {"b"});
        QVERIFY(combo->currentText().contains(u"Unavailable"));
        catalogue.change({fakeDevice("b"), fakeDevice("a")});
        QCOMPARE(combo->currentData().toByteArray(), QByteArray {"b"});
        QTest::mouseClick(buttons->button(QDialogButtonBox::Apply), Qt::LeftButton);
        QCOMPARE(settings.deviceId(), QByteArray {"b"});
        settings.setVolume(0.7);
        combo->setCurrentIndex(combo->findData(QByteArray {"a"}));
        QTest::mouseClick(buttons->button(QDialogButtonBox::Cancel), Qt::LeftButton);
        QCOMPARE(settings.deviceId(), QByteArray {"b"});
        QCOMPARE(settings.volume(), 0.7); // Dialog cannot overwrite newer live volume.
    }

    void settingsProviderIsSharedAndDoesNotStartAudio()
    {
        QTemporaryDir dir;
        SettingsManager manager {dir.filePath(u"settings.ini"_s)};
        HeadphonesSettings settings {&manager};
        FakeDeviceCatalogue catalogue;
        auto provider = std::make_shared<HeadphonesSettingsProvider>(settings, catalogue);
        auto host     = settingsProviderAdapter(provider);
        host->showSettings(nullptr);
        auto* first = provider->createSettings(nullptr);
        provider->showSettings(nullptr);
        QCOMPARE(first, provider->createSettings(nullptr));
        QVERIFY(first->isVisible());
        provider.reset();
        // Host adapter has no ownership that can outlive the plugin services.
        host->showSettings(nullptr);
        QVERIFY(settings.deviceId().isEmpty());
    }

    void widgetsPreservePausedSeekAndSessionAcrossRecreation()
    {
        PreviewHarness h;
        auto plan = h.add(u"preview.wav"_s);
        h.create();
        HeadphonesActions actions {*h.controller, nullptr};
        Track track {u"preview.wav"_s};
        track.setTitle(u"<b>Preview title</b>"_s);
        track.setDuration(10000);
        actions.setSelection({track}, {track});
        auto controls = std::make_unique<HeadphonesPlayerControl>(*h.controller, actions, nullptr);
        auto seekbar = std::make_unique<HeadphonesSeekBar>(*h.controller, nullptr);
        auto volumeControl = std::make_unique<HeadphonesVolumeControl>(*h.controller, nullptr);
        controls->findChild<QToolButton*>(u"Headphones.Preview"_s)->click();
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
        controls->findChild<QToolButton*>(u"Headphones.PlayPause"_s)->click();
        auto* seek = seekbar->findChild<QSlider*>(u"Headphones.Seek"_s);
        QVERIFY(seek->isEnabled());
        const auto generation = h.controller->snapshot().generation;
        seek->setValue(seek->maximum() / 2);
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);
        QCOMPARE(h.controller->snapshot().generation, generation + 1);
        QCOMPARE(h.controller->snapshot().positionMs, uint64_t {5000});
        auto* volume = volumeControl->findChild<QSlider*>(u"Headphones.Volume"_s);
        volume->setValue(42);
        QCOMPARE(h.controller->snapshot().volume, 0.42);
        QVERIFY(volume->toolTip().contains(u"42%"));
        const auto opens = plan->opens.load();
        controls.reset();
        seekbar.reset();
        volumeControl.reset();
        QCOMPARE(h.controller->snapshot().state, PreviewState::Paused);
        controls = std::make_unique<HeadphonesPlayerControl>(*h.controller, actions, nullptr);
        seekbar = std::make_unique<HeadphonesSeekBar>(*h.controller, nullptr);
        volumeControl = std::make_unique<HeadphonesVolumeControl>(*h.controller, nullptr);
        QCOMPARE(plan->opens.load(), opens);
        QCOMPARE(volumeControl->findChild<QSlider*>(u"Headphones.Volume"_s)->value(), 42);
        QCOMPARE(seekbar->findChild<QLabel*>(u"Headphones.Elapsed"_s)->text(), Utils::msToString(5000));
        QCOMPARE(h.controller->snapshot().state, PreviewState::Paused);
        QCOMPARE(h.controller->snapshot().generation, generation + 1);
    }

    void oversizedMetadataDurationUsesSafeSeekRange()
    {
        PreviewHarness h;
        auto source = h.add(u"long.wav"_s);
        h.create();
        HeadphonesSeekBar widget {*h.controller, nullptr};
        Track track {u"long.wav"_s};
        track.setDuration(std::numeric_limits<uint64_t>::max());
        h.controller->previewTrack(track);
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
        h.controller->playPause();
        auto* slider = widget.findChild<QSlider*>(u"Headphones.Seek"_s);
        slider->setValue(slider->maximum());
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);
        QCOMPARE(source->lastSeek.load(), static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
    }

    void unknownDurationAndUnavailableDeviceAreExplicit()
    {
        QTemporaryDir dir;
        SettingsManager manager {dir.filePath(u"settings.ini"_s)};
        HeadphonesSettings settings {&manager};
        settings.setDevice("gone", u"Saved device"_s);
        FakeDeviceCatalogue catalogue;
        PreviewHarness h;
        h.add(u"unknown.wav"_s);
        h.create();
        HeadphonesSeekBar seekbar {*h.controller, nullptr};
        HeadphonesOutputDevice output {settings, catalogue};
        h.controller->previewTrack(Track {u"unknown.wav"_s});
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
        QVERIFY(!seekbar.findChild<QSlider*>(u"Headphones.Seek"_s)->isEnabled());
        QVERIFY(output.findChild<QComboBox*>(u"Headphones.DeviceChoice"_s)->currentText().contains(u"Unavailable"));
        QCOMPARE(seekbar.findChild<QLabel*>(u"Headphones.Total"_s)->text(), u"unknown"_s);
    }
};

QTEST_MAIN(HeadphonesGuiTest)
#include "headphonesguitest.moc"
