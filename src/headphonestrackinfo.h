// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gui/fywidget.h>

namespace Fooyin {
class ElidedLabel;
namespace Headphones {
class PreviewController;
struct PreviewSnapshot;

class HeadphonesTrackInfo : public FyWidget
{
    Q_OBJECT
public:
    explicit HeadphonesTrackInfo(PreviewController& controller, QWidget* parent = nullptr);
    QString name() const override;
    QString layoutName() const override;

private:
    void refresh(const PreviewSnapshot& snapshot);
    ElidedLabel* m_title;
};
} // namespace Headphones
} // namespace Fooyin
