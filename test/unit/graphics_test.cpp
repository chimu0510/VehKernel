// SPDX-License-Identifier: GPL-3.0-or-later
// The local display's drawing functions, fonts and icons (spec 0016,
// ADR-0022): analytic checks of what each function lights, and reference
// images, compared bit for bit, of every function, clipped at every edge, of
// both fonts and of every icon. Run with VK_UPDATE_GOLDEN=1 to rewrite the
// images after an intended change, then review them.
#include "reference_image.hpp"

#include <vk/drivers/ssd1306.hpp>
#include <vk/local_display/font.hpp>
#include <vk/local_display/graphics.hpp>
#include <vk/local_display/icons.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>

namespace {

using vk::drivers::MonoFrame;
using vk::local_display::Bitmap;
using vk::local_display::Canvas;
using vk::local_display::Icon;

constexpr int W = static_cast<int>(MonoFrame::width);
constexpr int H = static_cast<int>(MonoFrame::height);

::testing::AssertionResult matches(const MonoFrame& frame, std::string_view name)
{
    return vk::test::matches_reference(frame, VK_GRAPHICS_GOLDEN_DIR, name);
}

std::size_t lit(const MonoFrame& frame)
{
    std::size_t count = 0;
    for (std::size_t y = 0; y < MonoFrame::height; ++y) {
        for (std::size_t x = 0; x < MonoFrame::width; ++x) {
            count += frame.get(x, y) ? 1U : 0U;
        }
    }
    return count;
}

// ---- Reference image support ----

TEST(ReferenceImage, PbmRoundTripsAndTheDiffShowsBothDirections)
{
    MonoFrame a;
    Canvas(a).rect(3, 4, 20, 9);
    MonoFrame back;
    ASSERT_TRUE(vk::test::frame_of(vk::test::pbm_of(a), back));
    EXPECT_EQ(a, back);

    MonoFrame b = a;
    b.set(3, 4, false);  // lit only in the reference
    b.set(50, 50, true);  // lit only in the actual image
    const std::string diff = vk::test::diff_text(b, a);
    EXPECT_EQ(diff.at(4 * (MonoFrame::width + 1) + 3), '-');
    EXPECT_EQ(diff.at((50 * (MonoFrame::width + 1)) + 50), '+');
    EXPECT_EQ(diff.at(4 * (MonoFrame::width + 1) + 4), '#');
    EXPECT_EQ(diff.at(0), '.');
}

TEST(ReferenceImage, AMismatchFailsWithADiffAndAMissingImageFails)
{
    // In a directory of its own, and with the update switch off, so that the
    // checked-in images are never touched.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vk_reference_image_test";
    std::filesystem::create_directories(dir);
    MonoFrame frame;
    Canvas(frame).rect(0, 0, 10, 10);
    {
        std::ofstream(dir / "ten.pbm", std::ios::binary) << vk::test::pbm_of(frame);
    }
    const char* saved = std::getenv("VK_UPDATE_GOLDEN");
    const std::string saved_value = saved == nullptr ? "" : saved;
    unsetenv("VK_UPDATE_GOLDEN");

    EXPECT_TRUE(vk::test::matches_reference(frame, dir.string(), "ten"));
    frame.set(100, 60, true);  // not in the reference
    frame.set(0, 0, false);    // only in the reference
    const auto result = vk::test::matches_reference(frame, dir.string(), "ten");
    ASSERT_FALSE(result);
    const std::string message = result.failure_message();
    EXPECT_NE(message.find('+'), std::string::npos);
    EXPECT_NE(message.find('-'), std::string::npos);
    EXPECT_FALSE(vk::test::matches_reference(frame, dir.string(), "absent"));

    if (saved != nullptr) {
        setenv("VK_UPDATE_GOLDEN", saved_value.c_str(), 1);
    }
    std::filesystem::remove_all(dir);
}

// ---- Analytic checks ----

TEST(Graphics, PixelLightsAndDarkensAndClips)
{
    MonoFrame frame;
    Canvas canvas(frame);
    canvas.pixel(5, 6);
    EXPECT_TRUE(canvas.get(5, 6));
    canvas.pixel(5, 6, false);
    EXPECT_FALSE(canvas.get(5, 6));
    canvas.pixel(-1, 0);
    canvas.pixel(0, -1);
    canvas.pixel(W, 0);
    canvas.pixel(0, H);
    canvas.pixel(-100000, 5);
    EXPECT_EQ(lit(frame), 0U);
}

TEST(Graphics, LinesIncludeBothEnds)
{
    MonoFrame frame;
    Canvas canvas(frame);
    canvas.hline(10, 10, 5);
    EXPECT_EQ(lit(frame), 5U);
    EXPECT_TRUE(canvas.get(10, 10));
    EXPECT_TRUE(canvas.get(14, 10));
    EXPECT_FALSE(canvas.get(15, 10));
    canvas.clear();
    canvas.vline(10, 10, 5);
    EXPECT_TRUE(canvas.get(10, 14));
    EXPECT_EQ(lit(frame), 5U);
    canvas.clear();
    canvas.line(2, 3, 12, 3);
    EXPECT_EQ(lit(frame), 11U);
    canvas.clear();
    canvas.line(0, 0, 9, 9);
    EXPECT_EQ(lit(frame), 10U);
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(canvas.get(i, i));
    }
    canvas.clear();
    canvas.line(9, 9, 0, 0);  // the same line the other way
    EXPECT_EQ(lit(frame), 10U);
    canvas.clear();
    canvas.line(4, 4, 4, 4);  // a point
    EXPECT_EQ(lit(frame), 1U);
    canvas.hline(0, 0, 0);
    canvas.hline(0, 0, -3);
    canvas.vline(0, 0, 0);
    EXPECT_EQ(lit(frame), 1U);
}

TEST(Graphics, ALineIsTheSameWhicheverEndItStartsAt)
{
    MonoFrame forward;
    MonoFrame backward;
    Canvas(forward).line(3, 5, 100, 40);
    Canvas(backward).line(100, 40, 3, 5);
    // Bresenham may break a tie differently from the other end by at most one
    // pixel per column; the line must still touch both ends and every column.
    for (int x = 3; x <= 100; ++x) {
        bool a = false;
        bool b = false;
        for (int y = 5; y <= 40; ++y) {
            a = a || forward.get(static_cast<std::size_t>(x), static_cast<std::size_t>(y));
            b = b || backward.get(static_cast<std::size_t>(x), static_cast<std::size_t>(y));
        }
        EXPECT_TRUE(a && b) << x;
    }
}

TEST(Graphics, RectanglesHaveTheGivenSize)
{
    MonoFrame frame;
    Canvas canvas(frame);
    canvas.rect(10, 10, 6, 4);
    EXPECT_EQ(lit(frame), 2U * (6 + 4 - 2));
    EXPECT_TRUE(canvas.get(10, 10));
    EXPECT_TRUE(canvas.get(15, 13));
    EXPECT_FALSE(canvas.get(11, 11));
    EXPECT_FALSE(canvas.get(16, 13));
    canvas.clear();
    canvas.fill_rect(10, 10, 6, 4);
    EXPECT_EQ(lit(frame), 24U);
    canvas.fill_rect(11, 11, 2, 2, false);
    EXPECT_EQ(lit(frame), 20U);
    canvas.clear();
    canvas.rect(5, 5, 1, 1);
    EXPECT_EQ(lit(frame), 1U);
    canvas.rect(5, 5, 0, 3);
    canvas.rect(5, 5, 3, -1);
    canvas.fill_rect(5, 5, 0, 3);
    canvas.fill_rect(5, 5, 3, 0);
    EXPECT_EQ(lit(frame), 1U);
}

TEST(Graphics, CirclesAreSymmetricAndHaveTheGivenRadius)
{
    MonoFrame frame;
    Canvas canvas(frame);
    canvas.circle(30, 30, 12);
    EXPECT_TRUE(canvas.get(42, 30));
    EXPECT_TRUE(canvas.get(18, 30));
    EXPECT_TRUE(canvas.get(30, 42));
    EXPECT_TRUE(canvas.get(30, 18));
    EXPECT_FALSE(canvas.get(43, 30));
    EXPECT_FALSE(canvas.get(30, 30));
    for (int dx = -13; dx <= 13; ++dx) {
        for (int dy = -13; dy <= 13; ++dy) {
            const auto at = [&](int sx, int sy) { return canvas.get(30 + (sx * dx), 30 + (sy * dy)); };
            EXPECT_EQ(at(1, 1), at(-1, 1));
            EXPECT_EQ(at(1, 1), at(1, -1));
            EXPECT_EQ(canvas.get(30 + dx, 30 + dy), canvas.get(30 + dy, 30 + dx));
        }
    }
    canvas.clear();
    canvas.circle(30, 30, 0);
    EXPECT_EQ(lit(frame), 1U);
    canvas.circle(30, 30, -1);
    EXPECT_EQ(lit(frame), 1U);
    canvas.clear();
    canvas.fill_circle(30, 30, 1);
    EXPECT_EQ(lit(frame), 5U);  // a plus
    canvas.clear();
    canvas.fill_circle(30, 30, 0);
    EXPECT_EQ(lit(frame), 1U);
    canvas.fill_circle(30, 30, -2);
    EXPECT_EQ(lit(frame), 1U);
}

TEST(Graphics, AFilledCircleFillsTheOutlineAndNothingOutsideIt)
{
    MonoFrame outline;
    MonoFrame filled;
    Canvas(outline).circle(40, 30, 11);
    Canvas(filled).fill_circle(40, 30, 11);
    for (int y = 15; y < 45; ++y) {
        for (int x = 25; x < 55; ++x) {
            const auto ux = static_cast<std::size_t>(x);
            const auto uy = static_cast<std::size_t>(y);
            if (outline.get(ux, uy)) {
                EXPECT_TRUE(filled.get(ux, uy)) << x << "," << y;
            }
            const int dx = x - 40;
            const int dy = y - 30;
            if ((dx * dx) + (dy * dy) <= 9 * 9) {
                EXPECT_TRUE(filled.get(ux, uy)) << x << "," << y;  // well inside
            }
            if ((dx * dx) + (dy * dy) > 12 * 12) {
                EXPECT_FALSE(filled.get(ux, uy)) << x << "," << y;  // well outside
            }
        }
    }
}

TEST(Graphics, TheClipRectangleLimitsEveryFunction)
{
    MonoFrame frame;
    Canvas canvas(frame);
    canvas.set_clip(20, 10, 30, 20);
    canvas.fill_rect(0, 0, W, H);
    EXPECT_EQ(lit(frame), 30U * 20U);
    EXPECT_TRUE(canvas.get(20, 10));
    EXPECT_TRUE(canvas.get(49, 29));
    EXPECT_FALSE(canvas.get(50, 29));
    canvas.clear_clip();
    EXPECT_FALSE(canvas.get(19, 10));
    EXPECT_TRUE(canvas.get(49, 29));

    canvas.clear();
    canvas.set_clip(20, 10, 30, 20);
    canvas.line(0, 0, W - 1, H - 1);
    canvas.circle(35, 20, 40);
    canvas.text(0, 12, "WIDE TEXT THAT CROSSES THE CLIP");
    canvas.clear_clip();
    for (std::size_t y = 0; y < MonoFrame::height; ++y) {
        for (std::size_t x = 0; x < MonoFrame::width; ++x) {
            if (frame.get(x, y)) {
                EXPECT_TRUE(x >= 20 && x < 50 && y >= 10 && y < 30) << x << "," << y;
            }
        }
    }
    // A clip partly or wholly outside the image, or empty, is cut to the image.
    canvas.clear();
    canvas.set_clip(-10, -10, 20, 20);
    canvas.fill_rect(0, 0, W, H);
    EXPECT_EQ(lit(frame), 100U);
    canvas.clear();
    canvas.set_clip(W - 5, H - 5, 100, 100);
    canvas.fill_rect(0, 0, W, H);
    EXPECT_EQ(lit(frame), 25U);
    canvas.clear();
    canvas.set_clip(200, 200, 10, 10);
    canvas.fill_rect(0, 0, W, H);
    canvas.set_clip(5, 5, 0, 10);
    canvas.fill_rect(0, 0, W, H);
    canvas.set_clip(5, 5, -3, 10);
    canvas.fill_rect(0, 0, W, H);
    EXPECT_EQ(lit(frame), 0U);
}

TEST(Graphics, TextAdvancesBySixPixelsAndBigTextByTwelve)
{
    MonoFrame frame;
    Canvas canvas(frame);
    EXPECT_EQ(canvas.text(4, 4, "AB"), 4 + 12);
    EXPECT_EQ(canvas.text(4, 20, ""), 4);
    EXPECT_EQ(canvas.big_text(4, 30, "12"), 4 + 24);
    EXPECT_EQ(vk::local_display::text_width(21), 126);
    EXPECT_EQ(vk::local_display::big_text_width(2), 24);
}

TEST(Graphics, TextDrawsTheSameGlyphsAsTheTextScreenColumns)
{
    MonoFrame frame;
    Canvas canvas(frame);
    canvas.text(0, 0, "A");
    // The font's columns have bit n = row n, as a frame byte's bits do.
    const auto& g = vk::local_display::glyph('A');
    for (std::size_t column = 0; column < vk::local_display::font_width; ++column) {
        EXPECT_EQ(frame.bytes.at(0).at(column), g.at(column)) << column;
    }
    EXPECT_EQ(frame.bytes.at(0).at(vk::local_display::font_width), 0U);  // the cell's gap
}

TEST(Graphics, ABitmapDrawsItsLitBitsAndOnlyDarkensWhenOpaque)
{
    constexpr std::array<std::uint8_t, 4> data{0x01, 0x02, 0x04, 0x08};  // a diagonal, 4 x 4
    const Bitmap bitmap{.width = 4, .height = 4, .data = data};
    MonoFrame frame;
    Canvas canvas(frame);
    canvas.fill_rect(0, 0, 8, 8);
    canvas.bitmap(2, 2, bitmap, false);  // dark bits stay as they are
    EXPECT_EQ(lit(frame), 64U - 4U);
    EXPECT_FALSE(canvas.get(2, 2));
    EXPECT_FALSE(canvas.get(5, 5));
    EXPECT_TRUE(canvas.get(3, 2));
    canvas.clear();
    canvas.fill_rect(0, 0, 8, 8);
    canvas.bitmap(2, 2, bitmap, false, true);  // opaque: the dark bits light the pixel
    EXPECT_EQ(lit(frame), 64U - 16U + 12U);
    // Empty and short bitmaps draw nothing, or what the data covers.
    canvas.clear();
    canvas.bitmap(0, 0, Bitmap{});
    canvas.bitmap(0, 0, Bitmap{.width = 4, .height = 4, .data = std::span<const std::uint8_t>(data).first(2)});
    EXPECT_EQ(lit(frame), 2U);
}

// ---- Reference images ----

// A shape crossing each edge and each corner, and one entirely inside, so a
// clipping error at any edge shows in the image.
template <typename Draw>
MonoFrame edges(Draw draw, int size)
{
    MonoFrame frame;
    Canvas canvas(frame);
    const int half = size / 2;
    const int cx = W / 2;
    const int cy = H / 2;
    draw(canvas, cx, cy);                // inside
    draw(canvas, -half + 2, cy);         // left edge
    draw(canvas, W + half - 3, cy);      // right edge
    draw(canvas, cx, -half + 2);         // top edge
    draw(canvas, cx, H + half - 3);      // bottom edge
    draw(canvas, -half + 2, -half + 2);  // corners
    draw(canvas, W + half - 3, -half + 2);
    draw(canvas, -half + 2, H + half - 3);
    draw(canvas, W + half - 3, H + half - 3);
    return frame;
}

TEST(GraphicsImages, RectangleOfTen)
{
    MonoFrame frame;
    Canvas(frame).rect(0, 0, 10, 10);
    EXPECT_TRUE(matches(frame, "rect_ten"));
}

TEST(GraphicsImages, LinesInAllDirections)
{
    MonoFrame frame;
    Canvas canvas(frame);
    for (std::size_t i = 0; i < 8; ++i) {  // a star of eight spokes plus slopes
        constexpr std::array<int, 8> dx = {20, 20, 0, -20, -20, -20, 0, 20};
        constexpr std::array<int, 8> dy = {0, 14, 20, 14, 0, -14, -20, -14};
        canvas.line(32, 32, 32 + dx.at(i), 32 + dy.at(i));
    }
    canvas.line(70, 5, 120, 12);
    canvas.line(70, 20, 75, 60);
    canvas.hline(80, 30, 40);
    canvas.vline(125, 35, 25);
    canvas.line(70, 62, 120, 62, true);
    canvas.line(100, 40, 110, 40, false);
    EXPECT_TRUE(matches(frame, "lines"));
}

TEST(GraphicsImages, LinesClippedAtEveryEdge)
{
    MonoFrame frame;
    Canvas canvas(frame);
    canvas.line(-20, 32, 150, 32);   // across left and right
    canvas.line(64, -20, 64, 100);   // across top and bottom
    canvas.line(-30, -20, 160, 90);  // corner to corner, outside both ends
    canvas.line(160, -20, -30, 90);
    canvas.line(-5, 10, 20, -5);     // cutting a corner
    canvas.line(100, 70, 140, 50);
    canvas.hline(-10, 5, 30);
    canvas.hline(110, 5, 30);
    canvas.vline(5, -10, 30);
    canvas.vline(5, 50, 30);
    canvas.hline(-100000, 60, 200000);  // far beyond the image
    EXPECT_TRUE(matches(frame, "lines_clipped"));
    EXPECT_EQ(frame.get(0, 32), true);
    EXPECT_EQ(frame.get(127, 32), true);
    EXPECT_EQ(frame.get(64, 0), true);
    EXPECT_EQ(frame.get(64, 63), true);
}

TEST(GraphicsImages, RectanglesOutlinedAndFilled)
{
    MonoFrame frame;
    Canvas canvas(frame);
    canvas.rect(2, 2, 30, 20);
    canvas.fill_rect(40, 2, 30, 20);
    canvas.fill_rect(44, 6, 22, 12, false);
    canvas.rect(80, 2, 1, 1);
    canvas.fill_rect(84, 2, 2, 2);
    canvas.rect(90, 2, 3, 3);
    canvas.rect(100, 2, 26, 3);
    canvas.fill_rect(2, 30, 60, 1);
    canvas.rect(2, 36, 124, 26);
    EXPECT_TRUE(matches(frame, "rects"));
}

TEST(GraphicsImages, RectanglesClippedAtEveryEdge)
{
    const MonoFrame outlined = edges([](Canvas& c, int x, int y) { c.rect(x - 10, y - 8, 20, 16); }, 20);
    EXPECT_TRUE(matches(outlined, "rect_clipped"));
    const MonoFrame filled = edges([](Canvas& c, int x, int y) { c.fill_rect(x - 10, y - 8, 20, 16); }, 20);
    EXPECT_TRUE(matches(filled, "fill_rect_clipped"));
    // A rectangle larger than the image, and with its origin beyond it.
    MonoFrame frame;
    Canvas canvas(frame);
    canvas.rect(-5, -5, 140, 80);
    canvas.rect(3, 3, 1000, 1000);
    canvas.fill_rect(-1000, 20, 2000, 4);
    EXPECT_TRUE(matches(frame, "rect_oversize"));
}

TEST(GraphicsImages, Circles)
{
    MonoFrame frame;
    Canvas canvas(frame);
    canvas.circle(14, 14, 12);
    canvas.fill_circle(42, 14, 12);
    canvas.circle(70, 14, 5);
    canvas.fill_circle(86, 14, 3);
    canvas.circle(98, 14, 2);
    canvas.fill_circle(106, 14, 1);
    canvas.circle(112, 14, 1);
    canvas.circle(118, 14, 0);
    canvas.circle(30, 48, 14);  // the 56 x 56 horizon's rim is of this order
    canvas.circle(30, 48, 9);
    canvas.fill_circle(30, 48, 3);
    canvas.fill_circle(80, 48, 14);
    canvas.fill_circle(80, 48, 8, false);
    canvas.circle(80, 48, 4);
    EXPECT_TRUE(matches(frame, "circles"));
}

TEST(GraphicsImages, CirclesClippedAtEveryEdge)
{
    const MonoFrame outlined = edges([](Canvas& c, int x, int y) { c.circle(x, y, 12); }, 24);
    EXPECT_TRUE(matches(outlined, "circle_clipped"));
    const MonoFrame filled = edges([](Canvas& c, int x, int y) { c.fill_circle(x, y, 12); }, 24);
    EXPECT_TRUE(matches(filled, "fill_circle_clipped"));
    MonoFrame frame;
    Canvas canvas(frame);
    canvas.circle(64, 32, 100);  // the image lies wholly inside
    canvas.fill_circle(64, 32, 40);
    canvas.circle(-3000, 32, 5);
    canvas.fill_circle(3000, 32, 5);
    EXPECT_TRUE(matches(frame, "circle_oversize"));
}

TEST(GraphicsImages, Bitmaps)
{
    // 12 x 16 and 5 x 8 bitmaps in the frame's layout, checked against the pixels.
    constexpr std::array<std::uint8_t, 8> tall{0xFF, 0x81, 0xBD, 0xA5, 0xFF, 0x81, 0xBD, 0xFF};  // 4 x 16
    const Bitmap bitmap{.width = 4, .height = 16, .data = tall};
    MonoFrame frame;
    Canvas canvas(frame);
    canvas.bitmap(10, 10, bitmap);
    EXPECT_EQ(frame.get(10, 10), true);    // band 0, column 0, row 0
    EXPECT_EQ(frame.get(10, 17), true);    // band 0, column 0, row 7
    EXPECT_EQ(frame.get(11, 11), false);   // band 0, column 1: 0x81 has rows 0 and 7
    EXPECT_EQ(frame.get(11, 10), true);
    EXPECT_EQ(frame.get(10, 18), true);    // band 1, column 0: 0xFF
    EXPECT_EQ(frame.get(11, 19), false);
    canvas.bitmap(0, 0, bitmap, false);
    canvas.bitmap(-2, -4, bitmap);
    canvas.bitmap(W - 2, H - 5, bitmap);
    canvas.bitmap(W - 2, -8, bitmap);
    canvas.bitmap(-3, H - 8, bitmap);
    canvas.bitmap(60, 30, bitmap, true, true);
    EXPECT_TRUE(matches(frame, "bitmaps"));
}

TEST(GraphicsImages, SmallFont)
{
    MonoFrame frame;
    Canvas canvas(frame);
    // Every printable ASCII character, 21 to a line as the text pages set them.
    std::string all;
    for (unsigned c = vk::local_display::first_glyph; c < vk::local_display::first_glyph + vk::local_display::glyph_count; ++c) {
        all.push_back(static_cast<char>(c));
    }
    for (std::size_t line = 0; line * 21 < all.size(); ++line) {
        canvas.text(1, static_cast<int>(line) * 8, std::string_view(all).substr(line * 21, 21));
    }
    canvas.text(1, 40, "\x01 non-ASCII is a ?\x7F");
    EXPECT_TRUE(matches(frame, "font_small"));
}

TEST(GraphicsImages, SmallFontClippedAtEveryEdge)
{
    MonoFrame frame;
    Canvas canvas(frame);
    canvas.text(-9, 28, "LEFT EDGE");
    canvas.text(W - 14, 28, "RIGHT");
    canvas.text(40, -3, "TOP EDGE");
    canvas.text(40, H - 4, "BOTTOM");
    canvas.text(-3, -3, "CORNER");
    canvas.text(W - 8, H - 4, "ZZ");
    canvas.text(-200, 10, "FAR LEFT");
    canvas.text(300, 10, "FAR RIGHT");
    canvas.text(20, 12, "INVERSE", false);
    EXPECT_TRUE(matches(frame, "font_small_clipped"));
}

TEST(GraphicsImages, BigFont)
{
    MonoFrame frame;
    Canvas canvas(frame);
    canvas.big_text(2, 0, "0123456789");   // ten digits, 120 pixels
    canvas.big_text(2, 20, "-12.5:30 -0.");
    canvas.big_text(2, 40, "9876543210");
    canvas.big_text(2, 20, "X", false);     // an unknown character is a blank
    EXPECT_TRUE(matches(frame, "font_big"));
    // Every digit is lit somewhere inside its 10 x 16 cell and its columns 0 and 9
    // stay within the cell.
    for (char c = '0'; c <= '9'; ++c) {
        const Bitmap g = vk::local_display::big_glyph(c);
        EXPECT_EQ(g.width, 10);
        EXPECT_EQ(g.height, 16);
        ASSERT_EQ(g.data.size(), 20U);
        int count = 0;
        for (auto byte : g.data) {
            for (int bit = 0; bit < 8; ++bit) {
                count += (byte >> bit) & 1;
            }
        }
        EXPECT_GT(count, 20) << c;
    }
    // No two digits look alike.
    for (char a = '0'; a <= '9'; ++a) {
        for (char b = static_cast<char>(a + 1); b <= '9'; ++b) {
            const auto da = vk::local_display::big_glyph(a).data;
            const auto db = vk::local_display::big_glyph(b).data;
            EXPECT_FALSE(std::equal(da.begin(), da.end(), db.begin())) << a << b;
        }
    }
}

TEST(GraphicsImages, BigFontClippedAtEveryEdge)
{
    MonoFrame frame;
    Canvas canvas(frame);
    canvas.big_text(-8, 24, "88");
    canvas.big_text(W - 14, 24, "88");
    canvas.big_text(40, -8, "88");
    canvas.big_text(40, H - 8, "88");
    canvas.big_text(-5, -5, "8");
    canvas.big_text(W - 5, H - 5, "8");
    canvas.big_text(-500, 24, "8");
    canvas.big_text(500, 24, "8");
    EXPECT_TRUE(matches(frame, "font_big_clipped"));
}

constexpr std::array<Icon, vk::local_display::icon_count> all_icons{
    Icon::Armed, Icon::EmergencyStop, Icon::LinkUp,  Icon::LinkDown, Icon::Gamepad,
    Icon::Companion, Icon::Fault,     Icon::Warning, Icon::Check,    Icon::Cross,
};

TEST(GraphicsImages, Icons)
{
    MonoFrame frame;
    Canvas canvas(frame);
    // Row 1: each icon drawn lit, 12 pixels apart; row 2: dark on a lit bar,
    // as the status bar's inverse use; row 3: each icon four times the size by
    // drawing its pixels as 3 x 3 blocks, to read the picture.
    for (std::size_t i = 0; i < all_icons.size(); ++i) {
        const auto x = static_cast<int>(i) * 12 + 4;
        canvas.bitmap(x, 2, vk::local_display::icon(all_icons[i]));
    }
    canvas.fill_rect(0, 14, W, 10);
    for (std::size_t i = 0; i < all_icons.size(); ++i) {
        const auto x = static_cast<int>(i) * 12 + 4;
        canvas.bitmap(x, 15, vk::local_display::icon(all_icons[i]), false);
    }
    for (std::size_t i = 0; i < 5; ++i) {
        const Bitmap b = vk::local_display::icon(all_icons[i]);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                if (((b.data[static_cast<std::size_t>(x)] >> y) & 1U) != 0) {
                    canvas.fill_rect(static_cast<int>(i) * 25 + (x * 3), 28 + (y * 3) / 1, 3, 3);
                }
            }
        }
    }
    EXPECT_TRUE(matches(frame, "icons"));
}

TEST(GraphicsImages, EachIconOnItsOwn)
{
    // One reference image per icon, so a change to one icon is one diff.
    constexpr std::array<std::string_view, 10> names{"armed",   "emergency_stop", "link_up", "link_down", "gamepad",
                                                     "companion", "fault",        "warning", "check",     "cross"};
    for (std::size_t i = 0; i < all_icons.size(); ++i) {
        MonoFrame frame;
        Canvas canvas(frame);
        const Bitmap b = vk::local_display::icon(all_icons[i]);
        EXPECT_EQ(b.width, 8);
        EXPECT_EQ(b.height, 8);
        ASSERT_EQ(b.data.size(), 8U);
        canvas.bitmap(0, 0, b);                                       // at the corner
        canvas.bitmap(W - 8, H - 8, b);                               // at the opposite corner
        canvas.bitmap(-4, 20, b);                                     // clipped at the left edge
        canvas.bitmap(W - 4, 20, b);                                  // clipped at the right edge
        canvas.bitmap(40, -4, b);                                     // at the top
        canvas.bitmap(40, H - 4, b);                                  // at the bottom
        canvas.fill_rect(60, 20, 10, 10);
        canvas.bitmap(61, 21, b, false);                              // dark on lit
        EXPECT_TRUE(matches(frame, std::string("icon_") + std::string(names[i]))) << names[i];
    }
}

TEST(GraphicsImages, NoIconIsBlankAndNoTwoAreAlike)
{
    for (std::size_t i = 0; i < all_icons.size(); ++i) {
        const auto di = vk::local_display::icon(all_icons[i]).data;
        EXPECT_NE(std::count(di.begin(), di.end(), 0), 8) << i;
        for (std::size_t j = i + 1; j < all_icons.size(); ++j) {
            const auto dj = vk::local_display::icon(all_icons[j]).data;
            EXPECT_FALSE(std::equal(di.begin(), di.end(), dj.begin())) << i << " " << j;
        }
    }
}

}  // namespace
