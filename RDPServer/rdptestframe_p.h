#ifndef RDPTESTFRAME_P_H
#define RDPTESTFRAME_P_H

#include <cstdint>
#include <vector>

struct RdpTestFrame
{
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint32_t stride = 0;
    std::vector<std::uint8_t> pixels;

    bool isValid() const;
};

class RdpTestFrameGenerator final
{
public:
    static RdpTestFrame fullFrame(std::uint16_t desktopWidth,
                                  std::uint16_t desktopHeight,
                                  std::uint64_t frameNumber);
    static RdpTestFrame counterFrame(std::uint16_t desktopWidth,
                                     std::uint16_t desktopHeight,
                                     std::uint64_t frameNumber);
};

#endif // RDPTESTFRAME_P_H
