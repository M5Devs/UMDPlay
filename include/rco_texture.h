#pragma once

// Decodes the PNG (and, in principle, GIM) images embedded in an .RCO menu
// container into cached SDL_Textures keyed by their IMG entry offset, so the
// Disc Menu renderer can blit them by the offset stored on each object.
//
// All textures are owned by the cache and destroyed in clear()/the destructor,
// so unloading a disc or exiting the app leaves nothing behind (LSan-clean).

#include <SDL2/SDL.h>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "rco_parser.h"

namespace umdplay {

class RcoTextureCache {
public:
    RcoTextureCache() = default;
    ~RcoTextureCache();

    RcoTextureCache(const RcoTextureCache&) = delete;
    RcoTextureCache& operator=(const RcoTextureCache&) = delete;

    // Decode every image in `rco` (whose raw bytes are `data`/`size`) into
    // textures on `renderer`. Returns the number decoded. Safe to call again
    // (it clears first).
    size_t build(SDL_Renderer* renderer, const RcoFile& rco,
                 const uint8_t* data, size_t size);

    // Texture for an IMG entry offset (from RcoObject::image_offset), or
    // nullptr when that image was absent or failed to decode.
    SDL_Texture* get(uint32_t image_offset) const;

    // Decode a single image (used by tests). Returns nullptr on failure.
    SDL_Texture* load_one(SDL_Renderer* renderer, const RcoImage& img,
                          const uint8_t* data, size_t size);

    void clear();
    size_t count() const { return m_tex.size(); }
    const std::string& last_error() const { return m_error; }

private:
    SDL_Renderer* m_renderer = nullptr;
    std::unordered_map<uint32_t, SDL_Texture*> m_tex;
    std::string m_error;
};

}  // namespace umdplay
