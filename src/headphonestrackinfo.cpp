// SPDX-License-Identifier: GPL-3.0-or-later
#include "headphonestrackinfo.h"

#include "previewcontroller.h"

#include <gui/widgets/elidedlabel.h>

#include <QHBoxLayout>

using namespace Qt::StringLiterals;

namespace Fooyin::Headphones {
HeadphonesTrackInfo::HeadphonesTrackInfo(PreviewController& controller, QWidget* parent)
    : FyWidget {parent}
    , m_title {new ElidedLabel(this)}
{
    setObjectName(layoutName());
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins({});
    layout->addWidget(m_title);
    m_title->setObjectName(u"Headphones.Title"_s);
    m_title->setTextFormat(Qt::PlainText);
    m_title->setAccessibleName(tr("Headphone preview track"));
    connect(&controller, &PreviewController::snapshotChanged, this, &HeadphonesTrackInfo::refresh);
    refresh(controller.snapshot());
}

QString HeadphonesTrackInfo::name() const
{
    return tr("Headphones Track Info");
}

QString HeadphonesTrackInfo::layoutName() const
{
    return u"HeadphonesTrackInfo"_s;
}

void HeadphonesTrackInfo::refresh(const PreviewSnapshot& snapshot)
{
    const auto title  = snapshot.track.effectiveTitle();
    const auto artist = snapshot.track.primaryArtist();
    const auto text   = !snapshot.track.isValid() ? tr("No headphone preview")
                      : artist.isEmpty()        ? title
                                                : tr("%1 - %2").arg(artist, title);
    if(m_title->text() != text) {
        m_title->setText(text);
    }
}
} // namespace Fooyin::Headphones
