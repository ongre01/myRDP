#include "rdptestframe_p.h"

#include <algorithm>
#include <array>
#include <limits>

namespace {
constexpr std::uint32_t bytesPerPixel = 4;
constexpr std::uint16_t preferredCounterWidth = 256;
constexpr std::uint16_t preferredCounterHeight = 72;
constexpr std::uint16_t counterInset = 16;
constexpr std::uint64_t displayedCounterLimit = 1000000;

struct Color
{
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
};

struct Rect
{
    std::uint16_t x;
    std::uint16_t y;
    std::uint16_t width;
    std::uint16_t height;
};

constexpr std::array<Color, 6> colorBars = {{
    {231, 76, 60},
    {243, 156, 18},
    {241, 196, 15},
    {46, 204, 113},
    {52, 152, 219},
    {155, 89, 182},
}};

constexpr std::array<std::uint8_t, 10> digitSegments = {
    0x3F, // 0: a b c d e f
    0x06, // 1: b c
    0x5B, // 2: a b d e g
    0x4F, // 3: a b c d g
    0x66, // 4: b c f g
    0x6D, // 5: a c d f g
    0x7D, // 6: a c d e f g
    0x07, // 7: a b c
    0x7F, // 8: a b c d e f g
    0x6F, // 9: a b c d f g
};

Rect counterRect(std::uint16_t desktopWidth, std::uint16_t desktopHeight)
{
    const std::uint16_t width = std::min(desktopWidth, preferredCounterWidth);
    const std::uint16_t height = std::min(desktopHeight, preferredCounterHeight);
    const std::uint16_t x = desktopWidth > width + 2 * counterInset ? counterInset : 0;
    const std::uint16_t y = desktopHeight > height + 2 * counterInset ? counterInset : 0;
    return {x, y, width, height};
}

bool allocatePixels(RdpTestFrame &frame)
{
    if (frame.width == 0 || frame.height == 0) {
        return false;
    }

    const std::uint64_t stride = static_cast<std::uint64_t>(frame.width) * bytesPerPixel;
    const std::uint64_t size = stride * frame.height;
    if (stride > (std::numeric_limits<std::uint32_t>::max)()
        || size > (std::numeric_limits<std::size_t>::max)()) {
        return false;
    }

    frame.stride = static_cast<std::uint32_t>(stride);
    try {
        frame.pixels.resize(static_cast<std::size_t>(size));
    } catch (...) {
        frame.stride = 0;
        return false;
    }
    return true;
}

void setPixel(RdpTestFrame &frame,
              std::uint16_t x,
              std::uint16_t y,
              const Color &color)
{
    if (x >= frame.width || y >= frame.height) {
        return;
    }

    const std::size_t offset = static_cast<std::size_t>(y) * frame.stride
                               + static_cast<std::size_t>(x) * bytesPerPixel;
    frame.pixels[offset] = color.blue;
    frame.pixels[offset + 1] = color.green;
    frame.pixels[offset + 2] = color.red;
    frame.pixels[offset + 3] = 0xFF;
}

void fillRect(RdpTestFrame &frame,
              std::uint16_t left,
              std::uint16_t top,
              std::uint16_t width,
              std::uint16_t height,
              const Color &color)
{
    const std::uint32_t right = std::min<std::uint32_t>(frame.width,
                                                        static_cast<std::uint32_t>(left) + width);
    const std::uint32_t bottom = std::min<std::uint32_t>(frame.height,
                                                         static_cast<std::uint32_t>(top) + height);
    for (std::uint32_t y = top; y < bottom; ++y) {
        for (std::uint32_t x = left; x < right; ++x) {
            setPixel(frame,
                     static_cast<std::uint16_t>(x),
                     static_cast<std::uint16_t>(y),
                     color);
        }
    }
}

void drawDigit(RdpTestFrame &frame,
               std::uint16_t left,
               std::uint16_t top,
               std::uint16_t width,
               std::uint16_t height,
               unsigned digit)
{
    if (digit >= digitSegments.size() || width < 5 || height < 9) {
        return;
    }

    const std::uint16_t thickness = std::max<std::uint16_t>(2, width / 6);
    const std::uint16_t horizontalWidth = width > 2 * thickness ? width - 2 * thickness : 1;
    const std::uint16_t halfHeight = height / 2;
    const std::uint16_t verticalHeight = halfHeight > thickness ? halfHeight - thickness : 1;
    const Color active = {126, 249, 180};
    const std::uint8_t segments = digitSegments[digit];

    if (segments & 0x01) {
        fillRect(frame, left + thickness, top, horizontalWidth, thickness, active);
    }
    if (segments & 0x02) {
        fillRect(frame, left + width - thickness, top + thickness, thickness, verticalHeight, active);
    }
    if (segments & 0x04) {
        fillRect(frame,
                 left + width - thickness,
                 top + halfHeight,
                 thickness,
                 verticalHeight,
                 active);
    }
    if (segments & 0x08) {
        fillRect(frame,
                 left + thickness,
                 top + height - thickness,
                 horizontalWidth,
                 thickness,
                 active);
    }
    if (segments & 0x10) {
        fillRect(frame, left, top + halfHeight, thickness, verticalHeight, active);
    }
    if (segments & 0x20) {
        fillRect(frame, left, top + thickness, thickness, verticalHeight, active);
    }
    if (segments & 0x40) {
        fillRect(frame,
                 left + thickness,
                 top + halfHeight - thickness / 2,
                 horizontalWidth,
                 thickness,
                 active);
    }
}

void drawCounter(RdpTestFrame &frame, std::uint64_t frameNumber)
{
    const Color panel = {20, 27, 38};
    const Color border = {210, 220, 230};
    fillRect(frame, 0, 0, frame.width, frame.height, panel);

    if (frame.width > 3 && frame.height > 3) {
        fillRect(frame, 0, 0, frame.width, 2, border);
        fillRect(frame, 0, frame.height - 2, frame.width, 2, border);
        fillRect(frame, 0, 0, 2, frame.height, border);
        fillRect(frame, frame.width - 2, 0, 2, frame.height, border);
    }

    constexpr std::uint16_t digitCount = 6;
    const std::uint16_t margin = frame.width >= 48 && frame.height >= 24 ? 6 : 2;
    const std::uint16_t gap = frame.width >= 96 ? 4 : 1;
    const std::uint16_t availableWidth = frame.width > 2 * margin + (digitCount - 1) * gap
                                             ? frame.width - 2 * margin - (digitCount - 1) * gap
                                             : 0;
    const std::uint16_t digitWidth = availableWidth / digitCount;
    const std::uint16_t digitHeight = frame.height > 2 * margin ? frame.height - 2 * margin : 0;
    if (digitWidth < 5 || digitHeight < 9) {
        return;
    }

    std::uint64_t value = frameNumber % displayedCounterLimit;
    std::array<unsigned, digitCount> digits = {};
    for (auto iterator = digits.rbegin(); iterator != digits.rend(); ++iterator) {
        *iterator = static_cast<unsigned>(value % 10);
        value /= 10;
    }

    std::uint16_t x = margin;
    for (const unsigned digit : digits) {
        drawDigit(frame, x, margin, digitWidth, digitHeight, digit);
        x = static_cast<std::uint16_t>(x + digitWidth + gap);
    }
}
} // namespace

bool RdpTestFrame::isValid() const
{
    return width > 0 && height > 0 && stride == static_cast<std::uint32_t>(width) * bytesPerPixel
           && pixels.size() == static_cast<std::size_t>(stride) * height;
}

RdpTestFrame RdpTestFrameGenerator::fullFrame(std::uint16_t desktopWidth,
                                              std::uint16_t desktopHeight,
                                              std::uint64_t frameNumber)
{
    RdpTestFrame frame;
    frame.width = desktopWidth;
    frame.height = desktopHeight;
    if (!allocatePixels(frame)) {
        return {};
    }

    for (std::uint32_t y = 0; y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            const std::size_t bar = std::min<std::size_t>(
                colorBars.size() - 1,
                static_cast<std::size_t>(x) * colorBars.size() / frame.width);
            Color color = colorBars[bar];
            if (((x / 64) + (y / 64)) % 2 != 0) {
                color.red = static_cast<std::uint8_t>(color.red * 3 / 4);
                color.green = static_cast<std::uint8_t>(color.green * 3 / 4);
                color.blue = static_cast<std::uint8_t>(color.blue * 3 / 4);
            }
            if (x % 64 == 0 || y % 64 == 0) {
                color = {235, 240, 245};
            }
            setPixel(frame,
                     static_cast<std::uint16_t>(x),
                     static_cast<std::uint16_t>(y),
                     color);
        }
    }

    const Rect rect = counterRect(desktopWidth, desktopHeight);
    RdpTestFrame counter = counterFrame(desktopWidth, desktopHeight, frameNumber);
    if (counter.isValid()) {
        for (std::uint32_t row = 0; row < counter.height; ++row) {
            const std::size_t sourceOffset = static_cast<std::size_t>(row) * counter.stride;
            const std::size_t destinationOffset = static_cast<std::size_t>(rect.y + row) * frame.stride
                                                  + static_cast<std::size_t>(rect.x) * bytesPerPixel;
            std::copy_n(counter.pixels.begin() + static_cast<std::ptrdiff_t>(sourceOffset),
                        counter.stride,
                        frame.pixels.begin() + static_cast<std::ptrdiff_t>(destinationOffset));
        }
    }
    return frame;
}

RdpTestFrame RdpTestFrameGenerator::counterFrame(std::uint16_t desktopWidth,
                                                 std::uint16_t desktopHeight,
                                                 std::uint64_t frameNumber)
{
    const Rect rect = counterRect(desktopWidth, desktopHeight);
    RdpTestFrame frame;
    frame.x = rect.x;
    frame.y = rect.y;
    frame.width = rect.width;
    frame.height = rect.height;
    if (!allocatePixels(frame)) {
        return {};
    }

    drawCounter(frame, frameNumber);
    return frame;
}
