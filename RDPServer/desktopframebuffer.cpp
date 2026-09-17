#include "desktopframebuffer_p.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace {
constexpr std::uint32_t comparisonTileSize = 16;

struct DirtyRectangle
{
    std::uint32_t left = 0;
    std::uint32_t top = 0;
    std::uint32_t right = 0;
    std::uint32_t bottom = 0;

    bool isEmpty() const
    {
        return left >= right || top >= bottom;
    }
};

DesktopCaptureResult errorResult(const QString &message)
{
    DesktopCaptureResult result;
    result.status = DesktopCaptureStatus::Error;
    result.errorMessage = message;
    return result;
}

bool bufferLayoutIsValid(const DesktopSize &size,
                         std::uint32_t stride,
                         const std::vector<std::uint8_t> &pixels)
{
    if (!size.isValid()) {
        return false;
    }

    const std::uint64_t minimumStride = static_cast<std::uint64_t>(size.width)
                                        * desktopCaptureBytesPerPixel;
    const std::uint64_t requiredBytes = static_cast<std::uint64_t>(stride) * size.height;
    return minimumStride <= (std::numeric_limits<std::uint32_t>::max)()
           && stride >= minimumStride && requiredBytes <= pixels.size();
}

bool tileDiffers(const std::vector<std::uint8_t> &currentPixels,
                 std::uint32_t currentStride,
                 const std::vector<std::uint8_t> &previousPixels,
                 std::uint32_t previousStride,
                 std::uint32_t left,
                 std::uint32_t top,
                 std::uint32_t width,
                 std::uint32_t height)
{
    const std::size_t rowBytes = static_cast<std::size_t>(width)
                                 * desktopCaptureBytesPerPixel;
    for (std::uint32_t row = 0; row < height; ++row) {
        const std::size_t currentOffset = static_cast<std::size_t>(top + row) * currentStride
                                          + static_cast<std::size_t>(left)
                                                * desktopCaptureBytesPerPixel;
        const std::size_t previousOffset = static_cast<std::size_t>(top + row) * previousStride
                                           + static_cast<std::size_t>(left)
                                                 * desktopCaptureBytesPerPixel;
        if (std::memcmp(currentPixels.data() + currentOffset,
                        previousPixels.data() + previousOffset,
                        rowBytes)
            != 0) {
            return true;
        }
    }
    return false;
}

DirtyRectangle changedRegion(const DesktopSize &size,
                             std::uint32_t currentStride,
                             const std::vector<std::uint8_t> &currentPixels,
                             std::uint32_t previousStride,
                             const std::vector<std::uint8_t> &previousPixels)
{
    DirtyRectangle dirty;
    dirty.left = size.width;
    dirty.top = size.height;

    for (std::uint32_t top = 0; top < size.height; top += comparisonTileSize) {
        const std::uint32_t tileHeight = std::min(comparisonTileSize, size.height - top);
        for (std::uint32_t left = 0; left < size.width; left += comparisonTileSize) {
            const std::uint32_t tileWidth = std::min(comparisonTileSize, size.width - left);
            if (!tileDiffers(currentPixels,
                             currentStride,
                             previousPixels,
                             previousStride,
                             left,
                             top,
                             tileWidth,
                             tileHeight)) {
                continue;
            }

            dirty.left = std::min(dirty.left, left);
            dirty.top = std::min(dirty.top, top);
            dirty.right = std::max(dirty.right, left + tileWidth);
            dirty.bottom = std::max(dirty.bottom, top + tileHeight);
        }
    }

    return dirty;
}

bool copyRegion(const std::vector<std::uint8_t> &source,
                std::uint32_t sourceStride,
                const DirtyRectangle &region,
                DesktopFrame *frame)
{
    frame->x = region.left;
    frame->y = region.top;
    frame->width = region.right - region.left;
    frame->height = region.bottom - region.top;

    const std::uint64_t stride = static_cast<std::uint64_t>(frame->width)
                                 * desktopCaptureBytesPerPixel;
    const std::uint64_t byteCount = stride * frame->height;
    if (stride > (std::numeric_limits<std::uint32_t>::max)()
        || byteCount > (std::numeric_limits<std::size_t>::max)()) {
        return false;
    }

    frame->stride = static_cast<std::uint32_t>(stride);
    try {
        frame->pixels.resize(static_cast<std::size_t>(byteCount));
    } catch (...) {
        return false;
    }

    for (std::uint32_t row = 0; row < frame->height; ++row) {
        const std::size_t sourceOffset = static_cast<std::size_t>(frame->y + row) * sourceStride
                                         + static_cast<std::size_t>(frame->x)
                                               * desktopCaptureBytesPerPixel;
        const std::size_t destinationOffset = static_cast<std::size_t>(row) * frame->stride;
        std::memcpy(frame->pixels.data() + destinationOffset,
                    source.data() + sourceOffset,
                    frame->stride);
    }
    return true;
}

void updatePreviousRegion(const std::vector<std::uint8_t> &source,
                          std::uint32_t sourceStride,
                          const DirtyRectangle &region,
                          std::uint32_t destinationStride,
                          std::vector<std::uint8_t> *destination)
{
    const std::size_t rowBytes = static_cast<std::size_t>(region.right - region.left)
                                 * desktopCaptureBytesPerPixel;
    for (std::uint32_t row = region.top; row < region.bottom; ++row) {
        const std::size_t sourceOffset = static_cast<std::size_t>(row) * sourceStride
                                         + static_cast<std::size_t>(region.left)
                                               * desktopCaptureBytesPerPixel;
        const std::size_t destinationOffset = static_cast<std::size_t>(row) * destinationStride
                                              + static_cast<std::size_t>(region.left)
                                                    * desktopCaptureBytesPerPixel;
        std::memcpy(destination->data() + destinationOffset,
                    source.data() + sourceOffset,
                    rowBytes);
    }
}
} // namespace

DesktopCaptureResult DesktopFrameBuffer::update(const DesktopSize &size,
                                                std::uint32_t stride,
                                                const std::vector<std::uint8_t> &pixels,
                                                bool forceFullFrame)
{
    if (!bufferLayoutIsValid(size, stride, pixels)) {
        return errorResult(QStringLiteral("The captured desktop buffer has an invalid layout."));
    }

    const bool hadPreviousFrame = previousSize.isValid();
    const bool sizeChanged = hadPreviousFrame && previousSize != size;
    const bool layoutChanged = previousSize != size || previousStride != stride
                               || previousPixels.empty();

    DirtyRectangle dirty;
    if (forceFullFrame || layoutChanged) {
        dirty = {0, 0, size.width, size.height};
    } else {
        dirty = changedRegion(size,
                              stride,
                              pixels,
                              previousStride,
                              previousPixels);
        if (dirty.isEmpty()) {
            DesktopCaptureResult result;
            result.status = DesktopCaptureStatus::NoChanges;
            return result;
        }
    }

    DesktopCaptureResult result;
    result.status = DesktopCaptureStatus::FrameReady;
    result.frame.desktopSize = size;
    result.frame.desktopSizeChanged = sizeChanged;
    if (!copyRegion(pixels, stride, dirty, &result.frame)) {
        return errorResult(QStringLiteral("Failed to allocate the captured desktop frame."));
    }

    try {
        if (layoutChanged) {
            previousPixels.assign(pixels.begin(),
                                  pixels.begin() + static_cast<std::size_t>(stride) * size.height);
            previousSize = size;
            previousStride = stride;
        } else {
            updatePreviousRegion(pixels,
                                 stride,
                                 dirty,
                                 previousStride,
                                 &previousPixels);
        }
    } catch (...) {
        return errorResult(QStringLiteral("Failed to retain the captured desktop frame."));
    }

    return result;
}
