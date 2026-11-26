// SPDX-License-Identifier: GPL-3.0-or-later
// Reference image test support (spec 0016, ADR-0022): renders a MonoFrame as
// a PBM image and compares it bit for bit with a checked-in reference. On a
// mismatch the failure shows the pixels that differ as text.
//
// Run with VK_UPDATE_GOLDEN=1 to write the reference after an intended
// change, then review it: the failure's text form is the way to read it.
#pragma once

#include <vk/drivers/ssd1306.hpp>

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace vk::test {

// The image as a binary PBM (P4), the format of the simulated panel's image.
inline std::string pbm_of(const drivers::MonoFrame& frame)
{
    using drivers::MonoFrame;
    std::string out = "P4\n" + std::to_string(MonoFrame::width) + " " + std::to_string(MonoFrame::height) + "\n";
    for (std::size_t y = 0; y < MonoFrame::height; ++y) {
        for (std::size_t byte = 0; byte < MonoFrame::width / 8; ++byte) {
            unsigned bits = 0;
            for (std::size_t bit = 0; bit < 8; ++bit) {
                bits = (bits << 1U) | (frame.get((byte * 8) + bit, y) ? 1U : 0U);
            }
            out.push_back(static_cast<char>(bits));
        }
    }
    return out;
}

// The image back from the PBM written by pbm_of(); false if it is not one.
inline bool frame_of(std::string_view pbm, drivers::MonoFrame& frame)
{
    using drivers::MonoFrame;
    const std::string header = "P4\n" + std::to_string(MonoFrame::width) + " " + std::to_string(MonoFrame::height) + "\n";
    if (pbm.size() != header.size() + (MonoFrame::width / 8 * MonoFrame::height) || !pbm.starts_with(header)) {
        return false;
    }
    frame.clear();
    const std::string_view data = pbm.substr(header.size());
    for (std::size_t y = 0; y < MonoFrame::height; ++y) {
        for (std::size_t x = 0; x < MonoFrame::width; ++x) {
            const auto byte = static_cast<unsigned char>(data[(y * (MonoFrame::width / 8)) + (x / 8)]);
            frame.set(x, y, ((byte >> (7 - (x % 8))) & 1U) != 0);
        }
    }
    return true;
}

// The image as text, one line per row: '#' lit in both, '.' dark in both,
// '+' lit only in `actual`, '-' lit only in `expected`.
inline std::string diff_text(const drivers::MonoFrame& actual, const drivers::MonoFrame& expected)
{
    using drivers::MonoFrame;
    std::string out;
    for (std::size_t y = 0; y < MonoFrame::height; ++y) {
        for (std::size_t x = 0; x < MonoFrame::width; ++x) {
            const bool a = actual.get(x, y);
            const bool e = expected.get(x, y);
            out.push_back(a ? (e ? '#' : '+') : (e ? '-' : '.'));
        }
        out.push_back('\n');
    }
    return out;
}

// Whether `frame` equals the reference image `<dir>/<name>.pbm`.
inline ::testing::AssertionResult matches_reference(const drivers::MonoFrame& frame, std::string_view dir,
                                                    std::string_view name)
{
    const std::filesystem::path path = std::filesystem::path(dir) / (std::string(name) + ".pbm");
    const std::string image = pbm_of(frame);
    const char* update = std::getenv("VK_UPDATE_GOLDEN");
    if (update != nullptr && std::string_view(update) == "1") {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << image;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return ::testing::AssertionFailure()
               << "missing reference image " << path << "; run with VK_UPDATE_GOLDEN=1\n"
               << diff_text(frame, drivers::MonoFrame{});
    }
    const std::string expected((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (image == expected) {
        return ::testing::AssertionSuccess();
    }
    drivers::MonoFrame reference;
    if (!frame_of(expected, reference)) {
        return ::testing::AssertionFailure() << "reference image " << path << " is not a 128 x 64 binary PBM";
    }
    return ::testing::AssertionFailure() << "image differs from " << path << " ('+' only drawn, '-' only in the reference)\n"
                                         << diff_text(frame, reference);
}

}  // namespace vk::test
