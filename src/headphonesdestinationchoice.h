// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "headphonedevices.h"

class QComboBox;

namespace Fooyin::Headphones {

inline constexpr int DestinationDescriptionRole = Qt::UserRole + 1;
inline constexpr int DestinationPairRole        = Qt::UserRole + 2;

// Shared presentation for the immediate widget and staged settings dialog.
void populateDestinationChoice(QComboBox& combo, const std::vector<HeadphoneDevice>& devices,
                               const QByteArray& selectedId, const QString& selectedDescription, int selectedPair);

} // namespace Fooyin::Headphones
