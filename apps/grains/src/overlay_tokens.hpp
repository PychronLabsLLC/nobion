#pragma once

#include <QColor>

// Every colour of the overlay. Paint code names one of these, never a literal.
namespace pychron::grains::tokens {

inline const QColor kCandidate{0x2e, 0xc2, 0x58};    // sanidine?
inline const QColor kNot{0xe0, 0x45, 0x45};          // not
inline const QColor kUnsure{0xf0, 0xa8, 0x20};       // unsure
inline const QColor kNoReference{0x9a, 0x9a, 0x9a};  // outlined, no label yet
inline const QColor kGreyed{0x70, 0x70, 0x78};       // held, lost, or picked
inline const QColor kReferenceRing{0x40, 0xa0, 0xff};
inline const QColor kSelected{0xff, 0xff, 0xff};
inline const QColor kBannerBackground{0x14, 0x14, 0x18, 0xd0};
inline const QColor kBannerText{0xf0, 0xf0, 0xf0};
inline const QColor kChrome{0x1c, 0x1c, 0x20};  // behind the picture
inline const QColor kOnChrome{0xd8, 0xd8, 0xd8};
inline constexpr int kVeilAlpha = 170;  // over the last picture of a lost camera

}  // namespace pychron::grains::tokens
