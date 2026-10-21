// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonesdestinationchoice.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QSignalBlocker>

#include <algorithm>

namespace Fooyin::Headphones {
namespace {
QString text(const char* source)
{
    return QCoreApplication::translate("HeadphonesDestinationChoice", source);
}
} // namespace

void populateDestinationChoice(QComboBox& combo, const std::vector<HeadphoneDevice>& devices,
                               const QByteArray& selectedId, const QString& selectedDescription, int selectedPair)
{
    const QSignalBlocker blocker {&combo};
    combo.clear();
    int selected   = -1;
    const auto add = [&](const QString& label, const QByteArray& id, const QString& description, int pair) {
        combo.addItem(label, id);
        const int index = combo.count() - 1;
        combo.setItemData(index, description, DestinationDescriptionRole);
        combo.setItemData(index, pair, DestinationPairRole);
        combo.setItemData(index, label, Qt::ToolTipRole);
        if(id == selectedId && pair == selectedPair) {
            selected = index;
        }
    };
    add(text("Not configured"), {}, {}, 0);
    bool previousGroup = false;
    for(const auto& device : devices) {
        if(device.id.isEmpty()) {
            continue;
        }
        const auto duplicates = std::ranges::count(devices, device.description, &HeadphoneDevice::description);
        const auto name
            = duplicates > 1 ? text("%1 [%2]").arg(device.description, encodeDeviceId(device.id)) : device.description;
        const auto pairs = previewChannelPairs(device);
        if((previousGroup || !pairs.empty()) && combo.count() > 1) {
            combo.insertSeparator(combo.count());
        }
        add(pairs.empty() ? name : text("%1 — Automatic").arg(name), device.id, device.description, 0);
        for(const auto pair : pairs) {
            add(text("%1 — Channels %2/%3").arg(name).arg(pair).arg(pair + 1), device.id, device.description, pair);
        }
        previousGroup = !pairs.empty();
    }
    if(selected < 0) {
        auto label = text("Unavailable — %1 [%2]").arg(selectedDescription, encodeDeviceId(selectedId));
        if(selectedPair > 0) {
            label = text("%1 — Channels %2/%3").arg(label).arg(selectedPair).arg(selectedPair + 1);
        }
        else if(selectedPair < 0) {
            label = text("%1 — Invalid channel selection").arg(label);
        }
        add(label, selectedId, selectedDescription, selectedPair);
    }
    combo.setCurrentIndex(selected);
    combo.setToolTip(text("%1\nChanging output stops preview. Press Play to restart.").arg(combo.currentText()));
}

} // namespace Fooyin::Headphones
