#pragma once
#include <cstddef>
#include <cstdint>

namespace codex_buddy_assets {

struct GifFrame {
    const uint16_t* pixels;
    uint16_t delay_ms;
};

struct GifAnimation {
    const GifFrame* frames;
    uint8_t frame_count;
    uint16_t width;
    uint16_t height;
};

enum class PetGifState : uint8_t {
    Sleep = 0,
    Idle,
    Busy,
    Attention,
    Celebrate,
    Dizzy,
    Heart,
    Wave,
    Sparkle,
};

const GifAnimation& seedyAnimation(PetGifState state);

}  // namespace codex_buddy_assets
