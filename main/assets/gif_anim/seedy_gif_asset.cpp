#include "seedy_gif_asset.h"

namespace codex_buddy_assets {

static const uint16_t fallback_frame_pixels[96 * 104] = {};

static const GifFrame fallback_frames[] = {
    {fallback_frame_pixels, 1000},
};

static const GifAnimation fallback_animation = {
    fallback_frames,
    1,
    96,
    104,
};

const GifAnimation& seedyAnimation(PetGifState)
{
    return fallback_animation;
}

}  // namespace codex_buddy_assets
