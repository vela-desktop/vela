// SPDX-FileCopyrightText: 2026 Brando Giuffrida
// SPDX-License-Identifier: GPL-3.0-or-later

// vela-color (common/color) against Material Color Utilities' own results:
// HCT both ways, the Tonal Spot scheme of a few source colors in both modes,
// and a source color from synthetic images.
//
//   ctest --test-dir build -R '^color$'      (or build/common/vela-color-test)
//
// The schemes' and images' expected values come from the upstream
// TypeScript implementation, npm's @material/material-color-utilities 0.4.0
// (spec 2021, standard contrast). To regenerate them, outside the
// repository:
//
//   npm install @material/material-color-utilities@0.4.0
//   node -e "import('@material/material-color-utilities').then(m => {
//       const s = new m.SchemeTonalSpot(m.Hct.fromInt(0xff5b8cff), false, 0);
//       console.log(s.primary.toString(16)); })"
//
// for each source and mode, printing the roles in Roles' order; an image's
// source color is m.Score.score(m.QuantizerCelebi.quantize(pixels, 128),
// { fallbackColorARGB })[0]. (0.4.0's ES modules import two files without
// their .js extension: add it in node_modules to run them under Node.)

#include "hct.h"
#include "quantize.h"
#include "scheme.h"

#include <QTest>

#include <vector>

using vela::color::Argb;

namespace {

struct Vector {
    Argb source;
    bool dark;
    vela::color::Roles roles; // in Roles' (and roleNames') order
};

// Vela's default accent, Google Blue, Material's error red, pure green, a
// yellow (the hues where sRGB's chroma runs out first) and Material's
// baseline purple.
const Vector schemes[] = {
    { 0xff5b8cff, false,
        0xff485d92, 0xffffffff, 0xffdae2ff, 0xff304578, 0xff585e71, 0xffffffff,
        0xffdce2f9, 0xff404659, 0xff735471, 0xffffffff, 0xfffed7f9, 0xff5a3d59,
        0xffba1a1a, 0xffffffff, 0xffffdad6, 0xff93000a, 0xfffaf8ff, 0xffdad9e0,
        0xfffaf8ff, 0xffffffff, 0xfff4f3fa, 0xffeeedf4, 0xffe8e7ef, 0xffe2e2e9,
        0xff1a1b21, 0xff45464f, 0xff757780, 0xffc5c6d0, 0xff2f3036, 0xfff1f0f7,
        0xffb2c5ff,
    },
    { 0xff5b8cff, true,
        0xffb2c5ff, 0xff172e60, 0xff304578, 0xffdae2ff, 0xffc0c6dd, 0xff2a3042,
        0xff404659, 0xffdce2f9, 0xffe1bbdd, 0xff412741, 0xff5a3d59, 0xfffed7f9,
        0xffffb4ab, 0xff690005, 0xff93000a, 0xffffdad6, 0xff121318, 0xff121318,
        0xff38393f, 0xff0d0e13, 0xff1a1b21, 0xff1e1f25, 0xff282a2f, 0xff33343a,
        0xffe2e2e9, 0xffc5c6d0, 0xff8f909a, 0xff45464f, 0xffe2e2e9, 0xff2f3036,
        0xff485d92,
    },
    { 0xff4285f4, false,
        0xff445e91, 0xffffffff, 0xffd8e2ff, 0xff2b4678, 0xff575e71, 0xffffffff,
        0xffdbe2f9, 0xff3f4759, 0xff715573, 0xffffffff, 0xfffbd7fc, 0xff583e5b,
        0xffba1a1a, 0xffffffff, 0xffffdad6, 0xff93000a, 0xfff9f9ff, 0xffd9d9e0,
        0xfff9f9ff, 0xffffffff, 0xfff3f3fa, 0xffededf4, 0xffe8e7ee, 0xffe2e2e9,
        0xff1a1b20, 0xff44474f, 0xff74777f, 0xffc4c6d0, 0xff2f3036, 0xfff0f0f7,
        0xffadc6ff,
    },
    { 0xff4285f4, true,
        0xffadc6ff, 0xff102f60, 0xff2b4678, 0xffd8e2ff, 0xffbfc6dc, 0xff293041,
        0xff3f4759, 0xffdbe2f9, 0xffdebcdf, 0xff402843, 0xff583e5b, 0xfffbd7fc,
        0xffffb4ab, 0xff690005, 0xff93000a, 0xffffdad6, 0xff111318, 0xff111318,
        0xff37393e, 0xff0c0e13, 0xff1a1b20, 0xff1e1f25, 0xff282a2f, 0xff33353a,
        0xffe2e2e9, 0xffc4c6d0, 0xff8e9099, 0xff44474f, 0xffe2e2e9, 0xff2f3036,
        0xff445e91,
    },
    { 0xffb3261e, false,
        0xff904a42, 0xffffffff, 0xffffdad5, 0xff73342c, 0xff775652, 0xffffffff,
        0xffffdad5, 0xff5d3f3b, 0xff705c2e, 0xffffffff, 0xfffcdfa6, 0xff574419,
        0xffba1a1a, 0xffffffff, 0xffffdad6, 0xff93000a, 0xfffff8f7, 0xffe8d6d3,
        0xfffff8f7, 0xffffffff, 0xfffff0ee, 0xfffceae7, 0xfff7e4e1, 0xfff1dedc,
        0xff231918, 0xff534341, 0xff857370, 0xffd8c2be, 0xff392e2c, 0xffffedea,
        0xffffb4aa,
    },
    { 0xffb3261e, true,
        0xffffb4aa, 0xff561e18, 0xff73342c, 0xffffdad5, 0xffe7bdb7, 0xff442926,
        0xff5d3f3b, 0xffffdad5, 0xffdfc38c, 0xff3e2e04, 0xff574419, 0xfffcdfa6,
        0xffffb4ab, 0xff690005, 0xff93000a, 0xffffdad6, 0xff1a1110, 0xff1a1110,
        0xff423735, 0xff140c0b, 0xff231918, 0xff271d1c, 0xff322826, 0xff3d3231,
        0xfff1dedc, 0xffd8c2be, 0xffa08c89, 0xff534341, 0xfff1dedc, 0xff392e2c,
        0xff904a42,
    },
    { 0xff00ff00, false,
        0xff406836, 0xffffffff, 0xffc0efb0, 0xff285020, 0xff54634d, 0xffffffff,
        0xffd7e8cd, 0xff3c4b37, 0xff386568, 0xffffffff, 0xffbcebee, 0xff1e4d50,
        0xffba1a1a, 0xffffffff, 0xffffdad6, 0xff93000a, 0xfff8fbf1, 0xffd8dbd2,
        0xfff8fbf1, 0xffffffff, 0xfff2f5eb, 0xffecefe5, 0xffe6e9e0, 0xffe1e4da,
        0xff191d17, 0xff43483f, 0xff73796e, 0xffc3c8bc, 0xff2e322b, 0xffeff2e8,
        0xffa5d395,
    },
    { 0xff00ff00, true,
        0xffa5d395, 0xff11380b, 0xff285020, 0xffc0efb0, 0xffbbcbb2, 0xff263422,
        0xff3c4b37, 0xffd7e8cd, 0xffa0cfd2, 0xff003739, 0xff1e4d50, 0xffbcebee,
        0xffffb4ab, 0xff690005, 0xff93000a, 0xffffdad6, 0xff11140f, 0xff11140f,
        0xff363a34, 0xff0c0f0a, 0xff191d17, 0xff1d211b, 0xff272b25, 0xff32362f,
        0xffe1e4da, 0xffc3c8bc, 0xff8d9387, 0xff43483f, 0xffe1e4da, 0xff2e322b,
        0xff406836,
    },
    { 0xffffeb3b, false,
        0xff685f12, 0xffffffff, 0xfff1e48a, 0xff4f4800, 0xff645f41, 0xffffffff,
        0xffebe3bd, 0xff4b472b, 0xff406652, 0xffffffff, 0xffc2ecd3, 0xff294e3b,
        0xffba1a1a, 0xffffffff, 0xffffdad6, 0xff93000a, 0xfffff9eb, 0xffdfdacc,
        0xfffff9eb, 0xffffffff, 0xfff9f3e5, 0xfff3eee0, 0xffede8da, 0xffe7e2d5,
        0xff1d1c13, 0xff4a473a, 0xff7b7768, 0xffcbc6b5, 0xff323027, 0xfff6f0e3,
        0xffd4c871,
    },
    { 0xffffeb3b, true,
        0xffd4c871, 0xff363100, 0xff4f4800, 0xfff1e48a, 0xffcec7a3, 0xff343117,
        0xff4b472b, 0xffebe3bd, 0xffa7d0b7, 0xff103726, 0xff294e3b, 0xffc2ecd3,
        0xffffb4ab, 0xff690005, 0xff93000a, 0xffffdad6, 0xff15130c, 0xff15130c,
        0xff3b3930, 0xff0f0e07, 0xff1d1c13, 0xff212017, 0xff2c2a21, 0xff37352c,
        0xffe7e2d5, 0xffcbc6b5, 0xff959181, 0xff4a473a, 0xffe7e2d5, 0xff323027,
        0xff685f12,
    },
    { 0xff6750a4, false,
        0xff65558f, 0xffffffff, 0xffe9ddff, 0xff4d3d75, 0xff625b71, 0xffffffff,
        0xffe8def8, 0xff4a4458, 0xff7e5260, 0xffffffff, 0xffffd9e3, 0xff633b48,
        0xffba1a1a, 0xffffffff, 0xffffdad6, 0xff93000a, 0xfffdf7ff, 0xffded8e0,
        0xfffdf7ff, 0xffffffff, 0xfff8f2fa, 0xfff2ecf4, 0xffece6ee, 0xffe6e0e9,
        0xff1d1b20, 0xff49454e, 0xff7a757f, 0xffcac4cf, 0xff322f35, 0xfff5eff7,
        0xffcfbdfe,
    },
    { 0xff6750a4, true,
        0xffcfbdfe, 0xff36275d, 0xff4d3d75, 0xffe9ddff, 0xffcbc2db, 0xff332d41,
        0xff4a4458, 0xffe8def8, 0xffefb8c8, 0xff4a2532, 0xff633b48, 0xffffd9e3,
        0xffffb4ab, 0xff690005, 0xff93000a, 0xffffdad6, 0xff141218, 0xff141218,
        0xff3b383e, 0xff0f0d13, 0xff1d1b20, 0xff211f24, 0xff2b292f, 0xff36343a,
        0xffe6e0e9, 0xffcac4cf, 0xff948f99, 0xff49454e, 0xffe6e0e9, 0xff322f35,
        0xff65558f,
    },
};

QByteArray hex(Argb argb)
{
    return "0x" + QByteArray::number(argb, 16).rightJustified(8, '0');
}

} // namespace

class ColorTest : public QObject {
    Q_OBJECT

private slots:
    // Every sRGB color comes back unchanged from its HCT (on a grid of the
    // cube: each channel in steps of 15, with 0 and 255).
    void roundTrip()
    {
        for (int r = 0; r <= 255; r += 15) {
            for (int g = 0; g <= 255; g += 15) {
                for (int b = 0; b <= 255; b += 15) {
                    const Argb argb = 0xFF000000u | Argb(r) << 16 | Argb(g) << 8 | Argb(b);
                    const vela::color::Hct hct = vela::color::hctFromArgb(argb);
                    QCOMPARE(hex(vela::color::argbFromHct(hct)), hex(argb));
                }
            }
        }
    }

    // CAM16's hue and chroma and L*, as upstream's C++ Hct (built from
    // cpp/cam at the commit the port is from, printing get_hue(),
    // get_chroma() and get_tone()). Its TypeScript computes the default
    // viewing conditions where the C++ takes them to nine decimals, so
    // npm's Hct.fromInt() differs from these in the fifth decimal (the
    // fourth for near grays, whose hue barely exists), far below what a
    // channel's step changes: the schemes below are the same to the bit.
    void knownHct_data()
    {
        QTest::addColumn<uint>("argb");
        QTest::addColumn<double>("hue");
        QTest::addColumn<double>("chroma");
        QTest::addColumn<double>("tone");
        QTest::newRow("vela accent") << 0xff5b8cffu << 270.19624576808252 << 62.154522677488949 << 60.069279225900502;
        QTest::newRow("google blue") << 0xff4285f4u << 265.97940344687021 << 62.269110561915682 << 56.550348737355023;
        QTest::newRow("black") << 0xff000000u << 0.0 << 0.0 << 0.0;
        QTest::newRow("white") << 0xffffffffu << 209.49210705653425 << 2.8690149862470831 << 100.0;
        QTest::newRow("gray") << 0xff808080u << 209.49397315440305 << 1.8960082637639395 << 53.585013452169036;
    }

    void knownHct()
    {
        QFETCH(uint, argb);
        QFETCH(double, hue);
        QFETCH(double, chroma);
        QFETCH(double, tone);
        const vela::color::Hct hct = vela::color::hctFromArgb(argb);
        QVERIFY2(std::abs(hct.hue - hue) < 1e-9, QByteArray::number(hct.hue, 'g', 17));
        QVERIFY2(std::abs(hct.chroma - chroma) < 1e-9, QByteArray::number(hct.chroma, 'g', 17));
        QVERIFY2(std::abs(hct.tone - tone) < 1e-9, QByteArray::number(hct.tone, 'g', 17));
    }

    // A color too saturated for its tone keeps hue and tone, loses chroma.
    void outOfGamutKeepsTone()
    {
        const Argb argb = vela::color::argbFromHct(120.0, 200.0, 50.0);
        const vela::color::Hct hct = vela::color::hctFromArgb(argb);
        QVERIFY(std::abs(hct.tone - 50.0) < 0.5);
        QVERIFY(std::abs(hct.hue - 120.0) < 2.0);
        QVERIFY(hct.chroma < 200.0);
    }

    void tonalSpot()
    {
        for (const Vector& expected : schemes) {
            const vela::color::Roles roles = vela::color::tonalSpot(expected.source, expected.dark);
            for (const vela::color::RoleName& role : vela::color::roleNames) {
                const QByteArray where = hex(expected.source) + (expected.dark ? " dark " : " light ") + role.name;
                QVERIFY2(roles.*role.member == expected.roles.*role.member,
                    where + ": " + hex(roles.*role.member) + ", expected " + hex(expected.roles.*role.member));
            }
        }
    }

    // A source's alpha doesn't change its scheme.
    void tonalSpotIgnoresAlpha()
    {
        QCOMPARE(hex(vela::color::tonalSpot(0x005b8cff, false).primary), hex(0xff485d92));
    }

    void ratioOfTones()
    {
        QCOMPARE(vela::color::ratioOfTones(0, 100), 21.0);
        QCOMPARE(vela::color::ratioOfTones(50, 50), 1.0);
        QVERIFY(vela::color::ratioOfTones(40, 100) > 4.5);
    }

    // One color: that color.
    void sourceOfASolidImage()
    {
        const std::vector<Argb> pixels(64 * 64, 0xff3366cc);
        QCOMPARE(hex(vela::color::sourceFromPixels(pixels, 0xff5b8cff)), hex(0xff3366cc));
    }

    // Two colors, 70% and 30%: the more used wins.
    void sourceOfTwoColors()
    {
        std::vector<Argb> pixels(70, 0xffd32f2f);
        pixels.insert(pixels.end(), 30, 0xff1976d2);
        QCOMPARE(hex(vela::color::sourceFromPixels(pixels, 0xff5b8cff)), hex(0xffd32f2f));
        const std::map<Argb, std::uint32_t> colors = vela::color::quantizeCelebi(pixels, 128);
        QCOMPARE(colors.size(), std::size_t(2));
        QCOMPARE(colors.at(0xffd32f2f), 70u);
        QCOMPARE(colors.at(0xff1976d2), 30u);
    }

    // Nothing with enough chroma, or nothing opaque: the fallback.
    void sourceFallsBack()
    {
        std::vector<Argb> grays;
        for (Argb v = 0; v < 256; ++v) {
            grays.push_back(0xFF000000u | v << 16 | v << 8 | v);
        }
        QCOMPARE(hex(vela::color::sourceFromPixels(grays, 0xff5b8cff)), hex(0xff5b8cff));
        const std::vector<Argb> transparent(100, 0x80ff0000);
        QCOMPARE(hex(vela::color::sourceFromPixels(transparent, 0xff5b8cff)), hex(0xff5b8cff));
        QCOMPARE(hex(vela::color::sourceFromPixels({}, 0)), hex(0));
    }
    // A picture whose colors differ mostly in green, or in blue: the
    // quantizer cuts its boxes along that axis and the source keeps the
    // picture's hue (HCT hues: greens about 140, blues about 280).
    void sourceFollowsTheDominantAxis_data()
    {
        QTest::addColumn<int>("axis"); // 1 green, 2 blue
        QTest::addColumn<double>("low");
        QTest::addColumn<double>("high");
        QTest::newRow("green gradient") << 1 << 100.0 << 170.0;
        QTest::newRow("blue gradient") << 2 << 230.0 << 300.0;
    }
    void sourceFollowsTheDominantAxis()
    {
        QFETCH(int, axis);
        QFETCH(double, low);
        QFETCH(double, high);
        std::vector<Argb> pixels;
        for (Argb v = 0; v < 256; ++v) {
            for (int repeat = 0; repeat < 8; ++repeat) {
                pixels.push_back(axis == 1 ? 0xFF000000u | 40u << 16 | v << 8 | 50u : 0xFF000000u | 30u << 16 | 60u << 8 | v);
            }
        }
        const Argb source = vela::color::sourceFromPixels(pixels, 0);
        QVERIFY(source != 0);
        const double hue = vela::color::hctFromArgb(source).hue;
        QVERIFY2(hue >= low && hue <= high, QByteArray::number(hue) + " for " + hex(source));
    }

    // Whatever the source, Tonal Spot's pairs keep M3's minimum contrast of
    // 3:1 (color.md: "accessible pairings guaranteeing a minimum 3:1"), in
    // both modes: every hue at a high and a low chroma, black, white and
    // a fully saturated yellow.
    void pairsKeepTheirContrast()
    {
        std::vector<Argb> sources { 0xff000000, 0xffffffff, 0xffffff00, 0xff00ff00, 0xff0000ff };
        for (int hue = 0; hue < 360; hue += 15) {
            sources.push_back(vela::color::argbFromHct(hue, 80, 50));
            sources.push_back(vela::color::argbFromHct(hue, 8, 70));
        }
        using R = vela::color::Roles;
        const std::pair<Argb R::*, Argb R::*> pairs[] = {
            { &R::onPrimary, &R::primary }, { &R::onPrimaryContainer, &R::primaryContainer },
            { &R::onSecondary, &R::secondary }, { &R::onSecondaryContainer, &R::secondaryContainer },
            { &R::onTertiary, &R::tertiary }, { &R::onTertiaryContainer, &R::tertiaryContainer },
            { &R::onError, &R::error }, { &R::onErrorContainer, &R::errorContainer },
            { &R::onSurface, &R::surface }, { &R::onSurface, &R::surfaceContainerHighest },
            { &R::onSurfaceVariant, &R::surfaceContainerHighest }, { &R::inverseOnSurface, &R::inverseSurface },
            { &R::primary, &R::surface }, { &R::outline, &R::surface },
        };
        for (const Argb source : sources) {
            for (const bool dark : { false, true }) {
                const R roles = vela::color::tonalSpot(source, dark);
                for (const auto& [fore, back] : pairs) {
                    const double ratio = vela::color::ratioOfTones(vela::color::hctFromArgb(roles.*fore).tone,
                        vela::color::hctFromArgb(roles.*back).tone);
                    QVERIFY2(ratio >= 3.0 - 0.05, hex(source) + (dark ? " dark: " : " light: ") + QByteArray::number(ratio)
                        + " between " + hex(roles.*fore) + " and " + hex(roles.*back));
                }
            }
        }
    }
};

QTEST_APPLESS_MAIN(ColorTest)

#include "color_test.moc"
