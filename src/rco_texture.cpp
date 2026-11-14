#include "rco_texture.h"

#include <cstring>

#include "stb_image.h"

namespace umdplay {

RcoTextureCache::~RcoTextureCache() { clear(); }

SDL_Texture* RcoTextureCache::load_one(SDL_Renderer* renderer,
                                       const RcoImage& img,
                                       const uint8_t* data, size_t size) {
    if (!renderer || !data) return nullptr;
    if (static_cast<uint64_t>(img.offset) + img.size > size) return nullptr;

    const uint8_t* bytes = data + img.offset;
    const int len = static_cast<int>(img.size);

    int w = 0, h = 0, comp = 0;
    unsigned char* px = nullptr;
    if (len >= 4 && std::memcmp(bytes, "\x89PNG", 4) == 0) {
        px = stbi_load_from_memory(bytes, len, &w, &h, &comp, 4);
    }
    if (!px) {
        m_error = "image '" + img.name + "' did not decode";
        return nullptr;
    }

    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, w, h);
    if (tex) {
        SDL_UpdateTexture(tex, nullptr, px, w * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
    } else {
        m_error = std::string("SDL_CreateTexture failed: ") + SDL_GetError();
    }
    stbi_image_free(px);
    return tex;
}

size_t RcoTextureCache::build(SDL_Renderer* renderer, const RcoFile& rco,
                              const uint8_t* data, size_t size) {
    clear();
    m_renderer = renderer;
    if (!renderer || !data) return 0;
    size_t ok = 0;
    for (const auto& img : rco.images) {
        if (m_tex.find(img.entry_offset) != m_tex.end()) continue;
        SDL_Texture* t = load_one(renderer, img, data, size);
        if (t) { m_tex[img.entry_offset] = t; ++ok; }
    }
    return ok;
}

SDL_Texture* RcoTextureCache::get(uint32_t image_offset) const {
    auto it = m_tex.find(image_offset);
    return it != m_tex.end() ? it->second : nullptr;
}

void RcoTextureCache::clear() {
    for (auto& kv : m_tex)
        if (kv.second) SDL_DestroyTexture(kv.second);
    m_tex.clear();
    m_renderer = nullptr;
}

}  // namespace umdplay
