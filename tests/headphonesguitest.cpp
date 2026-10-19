// SPDX-License-Identifier: GPL-3.0-or-later
#include "fakes/audiosinkfakes.h"
#include "fakes/previewfakes.h"
#include "headphonesactions.h"
#include "headphonessettings.h"
#include "headphonessettingsdialog.h"
#include "headphoneswidget.h"

#include <gui/trackselectioncontroller.h>
#include <utils/actions/actionmanager.h>
#include <utils/actions/command.h>
#include <utils/settings/settingsmanager.h>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QTest>

#include <limits>

using namespace Qt::StringLiterals;
using namespace Fooyin;
using namespace Fooyin::Headphones;
using namespace Fooyin::Headphones::Tests;

class HeadphonesGuiTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
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
            auto* command = manager.command("Headphones.PreviewSelected");
            QVERIFY(command);
            QVERIFY(command->defaultShortcuts().isEmpty());
            QCOMPARE(command->categories(), QStringList {u"Headphones Preview"_s});
            TrackSelection selected;
            selected.tracks = {Track {u"first.wav"_s}, Track {u"second.wav"_s}};
            QMenu menu;
            actions.renderMenu(&menu, selected);
            actions.setSelection({Track {u"second.wav"_s}}, selected.tracks);
            menu.actions().front()->trigger();
            QTRY_COMPARE(first->opens.load(), 1);
            QCOMPARE(second->opens.load(), 0);
            QCOMPARE(h.controller->snapshot().track.filepath(), u"first.wav"_s);
            h.controller->stop();
            // A panel keeps the last display selection after focus moves off the view.
            actions.setSelection({}, {Track {u"second.wav"_s}});
            auto* shortcut = actions.findChild<QAction*>(u"Headphones.PreviewSelected"_s);
            QVERIFY(!shortcut->isEnabled());
            actions.previewDisplaySelection();
            QTRY_COMPARE(second->opens.load(), 1);
            actions.setSelection({}, {});
            QVERIFY(!actions.displayPreviewReason().isEmpty());
            actions.setSelection({Track {u"https://example.com/live"_s}}, {});
            QVERIFY(!shortcut->isEnabled());
            QCOMPARE(h.controller->snapshot().track.filepath(), u"second.wav"_s);
        }
        QVERIFY(!manager.command("Headphones.PreviewSelected"));
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

    void widgetPreservesPausedSeekAndSessionAcrossRecreation()
    {
        QTemporaryDir dir;
        SettingsManager manager {dir.filePath(u"settings.ini"_s)};
        HeadphonesSettings settings {&manager};
        settings.setDevice("selected", u"Headphones"_s);
        FakeDeviceCatalogue catalogue;
        catalogue.entries = {fakeDevice("selected")};
        PreviewHarness h;
        auto plan = h.add(u"preview.wav"_s);
        h.create();
        HeadphonesActions actions {*h.controller, nullptr};
        Track track {u"preview.wav"_s};
        track.setTitle(u"<b>Preview title</b>"_s);
        track.setDuration(10000);
        actions.setSelection({track}, {track});
        auto provider = std::make_shared<HeadphonesSettingsProvider>(settings, catalogue);
        auto widget   = std::make_unique<HeadphonesWidget>(*h.controller, actions, settings, catalogue, provider);
        widget->show();
        auto* preview = widget->findChild<QPushButton*>(u"Headphones.Preview"_s);
        QTest::mouseClick(preview, Qt::LeftButton);
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
        auto* title = widget->findChild<QLabel*>(u"Headphones.Title"_s);
        QCOMPARE(title->text(), u"<b>Preview title</b>"_s);
        QCOMPARE(title->textFormat(), Qt::PlainText);
        widget->findChild<QPushButton*>(u"Headphones.PlayPause"_s)->click();
        auto* seek = widget->findChild<QSlider*>(u"Headphones.Seek"_s);
        QVERIFY(seek->isEnabled());
        const auto generation = h.controller->snapshot().generation;
        // Simulate dragging: no seek until release, and exactly one new generation.
        seek->setSliderDown(true);
        seek->setSliderPosition(5000);
        QCOMPARE(h.controller->snapshot().generation, generation);
        seek->setSliderDown(false);
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Paused);
        QCOMPARE(h.controller->snapshot().generation, generation + 1);
        QCOMPARE(h.controller->snapshot().positionMs, uint64_t {5000});
        auto* volume = widget->findChild<QSlider*>(u"Headphones.Volume"_s);
        volume->setValue(42);
        QCOMPARE(h.controller->snapshot().volume, 0.42);
        QCOMPARE(widget->findChild<QLabel*>(u"Headphones.VolumeValue"_s)->text(), u"42%"_s);
        const auto opens = plan->opens.load();
        widget.reset();
        QCOMPARE(h.controller->snapshot().state, PreviewState::Paused);
        widget = std::make_unique<HeadphonesWidget>(*h.controller, actions, settings, catalogue, provider);
        QCOMPARE(plan->opens.load(), opens);
        QCOMPARE(widget->findChild<QSlider*>(u"Headphones.Volume"_s)->value(), 42);
        QCOMPARE(widget->findChild<QLabel*>(u"Headphones.Status"_s)->text(), u"Paused"_s);
    }

    void oversizedMetadataDurationUsesSafeSeekRange()
    {
        QTemporaryDir dir;
        SettingsManager manager {dir.filePath(u"settings.ini"_s)};
        HeadphonesSettings settings {&manager};
        FakeDeviceCatalogue catalogue;
        PreviewHarness h;
        auto source = h.add(u"long.wav"_s);
        h.create();
        HeadphonesActions actions {*h.controller, nullptr};
        HeadphonesWidget widget {*h.controller, actions, settings, catalogue, {}};
        Track track {u"long.wav"_s};
        track.setDuration(std::numeric_limits<uint64_t>::max());
        h.controller->previewTrack(track);
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
        h.controller->playPause();
        widget.findChild<QSlider*>(u"Headphones.Seek"_s)->setValue(10000);
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
        HeadphonesActions actions {*h.controller, nullptr};
        HeadphonesWidget widget {*h.controller, actions, settings, catalogue, {}};
        h.controller->previewTrack(Track {u"unknown.wav"_s});
        QTRY_COMPARE(h.controller->snapshot().state, PreviewState::Playing);
        QVERIFY(!widget.findChild<QSlider*>(u"Headphones.Seek"_s)->isEnabled());
        QVERIFY(widget.findChild<QLabel*>(u"Headphones.Device"_s)->text().contains(u"unavailable"));
        QVERIFY(widget.findChild<QLabel*>(u"Headphones.Position"_s)->text().contains(u"unknown"));
    }
};

QTEST_MAIN(HeadphonesGuiTest)
#include "headphonesguitest.moc"
