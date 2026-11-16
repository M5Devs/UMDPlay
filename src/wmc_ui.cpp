#include "wmc_ui.h"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "stb_image.h"
#include "umdplay_logo.h"      // embedded official logo (header mark + window icon)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace {

// --- WMC palette: deep oceanic / cobalt blue -------------------------------
constexpr ImU32 kCobaltMid   = IM_COL32( 14,  60, 128, 255);
constexpr ImU32 kCobaltGlow  = IM_COL32( 46, 132, 214, 255);
constexpr ImU32 kCobaltDeep  = IM_COL32(  4,  10,  28, 255);
constexpr ImU32 kGlassHi     = IM_COL32(190, 225, 255, 255);
constexpr ImU32 kAccent      = IM_COL32(120, 200, 255, 255);
constexpr ImU32 kAuroraTeal  = IM_COL32( 40, 190, 210, 255);
constexpr ImU32 kTextPrimary = IM_COL32(240, 248, 255, 255);
constexpr ImU32 kTextDim     = IM_COL32(170, 196, 226, 255);

// Bullet separator (U+2022), used between header words. Guaranteed to be in the
// glyph range via the sample string passed to init().
constexpr const char* kBullet = "\xE2\x80\xA2";

// Fonts probed in order of preference; first hit wins. Noto Sans CJK gives us
// the Japanese glyphs PARAM.SFO titles frequently contain.
const char* kFontCandidates[] = {
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSerifCJK-Regular.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "C:\\Windows\\Fonts\\segoeui.ttf",
    "C:\\Windows\\Fonts\\arial.ttf",
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/STHeiti Medium.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/Helvetica.ttc",
    "/Library/Fonts/Arial.ttf",
};

// ImGui's AddFontFromFileTTF calls ErrorLog() on a missing file, which asserts
// via BeginErrorTooltip() outside a frame and crashes. Test existence first.
bool file_exists(const char* path) {
    std::error_code ec;
    return path && std::filesystem::exists(path, ec);
}

// Decode the embedded official logo PNG into a fresh renderer texture. The
// caller owns it; null on failure so the header can fall back to the word mark.
SDL_Texture* make_logo_texture(SDL_Renderer* r) {
    if (!r) return nullptr;
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(
        umdplay_logo_png, static_cast<int>(umdplay_logo_png_len), &w, &h, &comp, 4);
    if (!px) return nullptr;
    SDL_Texture* tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, w, h);
    if (tex) {
        SDL_UpdateTexture(tex, nullptr, px, w * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
    }
    stbi_image_free(px);
    return tex;
}

ImU32 with_alpha(ImU32 c, float a) {
    a = std::clamp(a, 0.0f, 1.0f);
    return (c & 0x00FFFFFF) | (static_cast<ImU32>(a * 255.0f) << 24);
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }

float hash01(int i) {
    const float x = std::sin(static_cast<float>(i) * 12.9898f) * 43758.5453f;
    return x - std::floor(x);
}

void format_time(double s, char* buf, size_t n) {
    if (s < 0 || !std::isfinite(s)) s = 0;
    const int total = static_cast<int>(s);
    std::snprintf(buf, n, "%d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
}

std::string human_size(uint32_t bytes) {
    char buf[32];
    const double mb = bytes / (1024.0 * 1024.0);
    if (mb >= 1000.0) std::snprintf(buf, sizeof(buf), "%.1f GB", mb / 1024.0);
    else              std::snprintf(buf, sizeof(buf), "%.0f MB", mb);
    return buf;
}

// Footer reserve: nothing else may draw below this y (keeps the key-hint bar
// clear of tiles and text at every window size).
float footer_top(float h) { return h - 56.0f; }

// Category rows shown in the vertical list, top to bottom. NowPlaying is
// prepended by the caller when a stream is open.
struct CategoryRow { const char* label; HubMode mode; };
const CategoryRow kCategoriesNoNp[] = {
    {"Recent Discs",   HubMode::Recent},
    {"Browse System",  HubMode::Browse},
    {"Settings",       HubMode::Settings},
};

}  // namespace

WmcUi::WmcUi() {}
WmcUi::~WmcUi() { close(); }

bool WmcUi::init(SDL_Window* window, SDL_Renderer* renderer,
                 const std::string& glyph_sample) {
    m_window = window;
    m_renderer = renderer;
    if (!m_window || !m_renderer) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // Build a glyph range covering ASCII + the code points actually present in
    // the disc metadata (titles are often Japanese). ImGui otherwise only loads
    // Latin-1 and the CJK glyphs would render as boxes.
    static ImVector<ImWchar> ranges;
    {
        ImFontGlyphRangesBuilder b;
        b.AddRanges(io.Fonts->GetGlyphRangesDefault());
        // Always include the bullet (U+2022), dashes and the arrow used in HUDs.
        b.AddText("\xE2\x80\xA2\xE2\x80\x93\xE2\x80\x94\xE2\x86\x92");
        if (!glyph_sample.empty()) b.AddText(glyph_sample.c_str());
        b.BuildRanges(&ranges);
    }

    // 10-foot typography: load a scalable font; prefer a CJK-capable face.
    ImFontConfig fc;
    fc.OversampleH = 2;
    fc.OversampleV = 2;
    for (const char* path : kFontCandidates) {
        if (!file_exists(path)) continue;
        if (io.Fonts->AddFontFromFileTTF(path, 20.0f, &fc, ranges.Data)) break;
    }
    if (io.Fonts->Fonts.Size == 0) {
        io.Fonts->AddFontDefault();
    }

    ImGui::StyleColorsDark();
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.0f;
    st.WindowBorderSize = 0.0f;
    st.WindowPadding = ImVec2(0, 0);

    if (!ImGui_ImplSDL2_InitForSDLRenderer(m_window, m_renderer)) return false;
    if (!ImGui_ImplSDLRenderer2_Init(m_renderer)) return false;
    m_imgui_ready = true;

    // Reuse the base font for the larger roles (scaled at draw time).
    m_font_body  = io.Fonts->Fonts[0];
    m_font_title = m_font_body;
    m_font_huge  = m_font_body;

    m_logo_tex = make_logo_texture(m_renderer);
    m_initialized = true;
    return true;
}

void WmcUi::close() {
    if (m_imgui_ready) {
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        m_imgui_ready = false;
    }
    if (m_initialized) {
        ImGui::DestroyContext();
        m_initialized = false;
    }
    if (m_logo_tex) { SDL_DestroyTexture(m_logo_tex); m_logo_tex = nullptr; }
    m_font_title = m_font_huge = m_font_body = nullptr;
}

void WmcUi::process_event(const SDL_Event& e) {
    if (m_imgui_ready) ImGui_ImplSDL2_ProcessEvent(&e);
    // Queue touch samples here (event time) so their timestamps are exact.
    if (e.type == SDL_FINGERDOWN || e.type == SDL_FINGERMOTION || e.type == SDL_FINGERUP) {
        feed_finger(e);
    }
}

void WmcUi::begin_frame() {
    if (!m_imgui_ready) return;
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

void WmcUi::render() {
    if (!m_imgui_ready) return;
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), m_renderer);
}

void WmcUi::present() {
    if (m_renderer) SDL_RenderPresent(m_renderer);
}

SDL_Texture* WmcUi::texture_from_png(const std::vector<uint8_t>& bytes,
                                     int* out_w, int* out_h) {
    if (bytes.empty() || !m_renderer) return nullptr;
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(bytes.data(),
                                              static_cast<int>(bytes.size()),
                                              &w, &h, &comp, 4);
    if (!px) return nullptr;

    SDL_Texture* tex = SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC, w, h);
    if (tex) {
        SDL_UpdateTexture(tex, nullptr, px, w * 4);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
    }
    stbi_image_free(px);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return tex;
}

// ---------------------------------------------------------------------------
// Aurora background
// ---------------------------------------------------------------------------
// Deep oceanic gradient with slow, drifting diagonal aurora sweeps (teal/blue
// light bands) and floating motes. A focused PIC1 backdrop cross-fades in,
// dimmed, for atmosphere.
void WmcUi::draw_aurora(ImDrawList* dl, float w, float h, float t, SDL_Texture* pic1) {
    const ImVec2 a(0, 0), b(w, h);

    const float breathe = 0.5f + 0.5f * std::sin(t * 0.22f);
    const ImU32 top = ImGui::GetColorU32(ImVec4(
        lerp(0.01f, 0.05f, breathe), lerp(0.06f, 0.12f, breathe),
        lerp(0.20f, 0.33f, breathe), 1.0f));
    dl->AddRectFilledMultiColor(a, b, top, top, kCobaltMid, kCobaltDeep);

    if (pic1) {
        // Slow atmospheric cross-fade + drift of the disc's PIC1 backdrop.
        const float drift = 16.0f * std::sin(t * 0.10f);
        const float pulse = 0.30f + 0.10f * std::sin(t * 0.18f);
        dl->AddImage(reinterpret_cast<ImTextureID>(pic1),
                     ImVec2(-48 + drift, -36 + drift * 0.5f),
                     ImVec2(w + 48 + drift, h + 36 + drift * 0.5f),
                     ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(255, 255, 255, static_cast<int>(90 * pulse + 30)));
        dl->AddRectFilled(a, b, IM_COL32(4, 10, 28, 160));
    }

    // Diagonal aurora sweeps: a few wide, soft, slowly moving light bands.
    dl->PushClipRect(a, b, true);
    for (int i = 0; i < 3; ++i) {
        const float speed = 0.035f + 0.02f * i;
        const float phase = std::fmod(t * speed + i * 0.37f, 1.6f) - 0.3f;
        const float sx = phase * (w + 400.0f) - 200.0f;
        const float band = w * (0.16f + 0.05f * i);
        const float tilt = h * 0.55f;
        const ImU32 c = (i % 2 == 0) ? kAuroraTeal : kCobaltGlow;
        const float al = 0.05f + 0.035f * (0.5f + 0.5f * std::sin(t * 0.5f + i));
        dl->AddQuadFilled(ImVec2(sx, 0), ImVec2(sx + band, 0),
                          ImVec2(sx + band - tilt, h), ImVec2(sx - tilt, h),
                          with_alpha(c, al));
    }
    dl->PopClipRect();

    // Glass sheen sweep.
    const float sweep = std::fmod(t * 0.05f, 1.4f) - 0.2f;
    const float sx = sweep * w;
    dl->AddQuadFilled(ImVec2(sx, 0), ImVec2(sx + w * 0.18f, 0),
                      ImVec2(sx + w * 0.04f, h), ImVec2(sx - w * 0.14f, h),
                      with_alpha(kGlassHi, 0.05f));

    // Floating light motes.
    for (int i = 0; i < 26; ++i) {
        const float base_x = hash01(i) * w;
        const float speed  = 8.0f + hash01(i + 100) * 22.0f;
        const float y = h - std::fmod(t * speed + hash01(i + 50) * h, h);
        const float x = base_x + 18.0f * std::sin(t * 0.6f + i);
        const float r = 1.0f + hash01(i + 200) * 3.0f;
        const float alpha = 0.06f + 0.10f * (0.5f + 0.5f * std::sin(t + i));
        dl->AddCircleFilled(ImVec2(x, y), r, with_alpha(kCobaltGlow, alpha));
    }

    // Vignette.
    dl->AddRectFilledMultiColor(a, b, IM_COL32(0, 0, 0, 110), IM_COL32(0, 0, 0, 110),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
}

// ---------------------------------------------------------------------------
// Official UMD logo (top-left header)
// ---------------------------------------------------------------------------
// Draws the embedded glossy UMD cartridge. A gentle breathing scale keeps a
// little life in the header; `loading` (ISO read in progress) also spins a
// soft accent halo behind the artwork. Draws nothing if the texture failed
// to load, leaving the "UMDPlay" word mark on its own.
void WmcUi::draw_umd_logo(ImDrawList* dl, float cx, float cy, float r, float t,
                          bool loading) {
    if (!m_logo_tex) return;
    m_logo_spin += (loading ? 5.0f : 0.35f) * ImGui::GetIO().DeltaTime;

    const float half = r * (1.0f + 0.015f * std::sin(t * 1.6f));
    const ImVec2 a(cx - half, cy - half);
    const ImVec2 b(cx + half, cy + half);

    if (loading) {
        // Soft rotating halo while the disc is being read.
        const int seg = 24;
        for (int i = 0; i < seg; ++i) {
            const float f0 = m_logo_spin + (static_cast<float>(i) / seg) * 6.2831853f;
            const float f1 = m_logo_spin + (static_cast<float>(i + 1) / seg) * 6.2831853f;
            const float lum = 0.35f + 0.65f * (0.5f + 0.5f * std::sin(f0 + t * 0.6f));
            dl->AddTriangleFilled(ImVec2(cx + std::cos(f0) * half * 1.09f,
                                         cy + std::sin(f0) * half * 1.09f),
                                  ImVec2(cx + std::cos(f1) * half * 1.09f,
                                         cy + std::sin(f1) * half * 1.09f),
                                  ImVec2(cx, cy),
                                  with_alpha(kAccent, 0.16f * lum));
        }
    }

    dl->AddImageQuad(reinterpret_cast<ImTextureID>(m_logo_tex),
                     ImVec2(a.x, a.y), ImVec2(b.x, a.y),
                     ImVec2(b.x, b.y), ImVec2(a.x, b.y),
                     ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1));
}

// ---------------------------------------------------------------------------
// HH:MM clock (top-right)
// ---------------------------------------------------------------------------
void WmcUi::draw_clock(ImDrawList* dl, float w, float h) {
    (void)h;
    std::time_t now = std::time(nullptr);
    std::tm lt{};
#if defined(_WIN32)
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);

    const float px = 30.0f;
    const ImVec2 ts = m_font_body->CalcTextSizeA(px, FLT_MAX, 0.0f, buf);
    const ImVec2 p(w - 64.0f - ts.x, 26.0f);
    dl->AddText(m_font_body, px, p, with_alpha(kTextPrimary, 0.95f), buf);
}

// ---------------------------------------------------------------------------
// Glass tiles (horizontal strip)
// ---------------------------------------------------------------------------
void WmcUi::draw_tile(ImDrawList* dl, const DiscItem& disc, float cx, float cy,
                      float w, float h, float focus, bool selected, float t) {
    const float scale = lerp(0.86f, 1.0f, focus);
    const float cw = w * scale, ch = h * scale;
    const ImVec2 p0(cx - cw * 0.5f, cy - ch * 0.5f);
    const ImVec2 p1(cx + cw * 0.5f, cy + ch * 0.5f);

    dl->AddRectFilled(ImVec2(p0.x + 6, p0.y + 10), ImVec2(p1.x + 6, p1.y + 10),
                      IM_COL32(0, 0, 0, static_cast<int>(110 * focus)), 16.0f);

    // Aero glass body: translucent cobalt with a bright top highlight.
    const ImU32 top = with_alpha(IM_COL32(48, 108, 182, 255), lerp(0.55f, 1.0f, focus));
    const ImU32 bot = with_alpha(IM_COL32(10, 26, 60, 255),  lerp(0.55f, 1.0f, focus));
    dl->AddRectFilledMultiColor(p0, p1, top, top, bot, bot);
    dl->AddRectFilledMultiColor(ImVec2(p0.x + 3, p0.y + 3), ImVec2(p1.x - 3, cy),
                                IM_COL32(255, 255, 255, 46), IM_COL32(255, 255, 255, 46),
                                IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));

    // Cover art (or a simple disc glyph when the disc has no ICON0).
    const float pad = cw * 0.10f;
    const float art = cw - pad * 2.0f;
    const ImVec2 a0(p0.x + pad, p0.y + pad);
    const ImVec2 a1(a0.x + art, a0.y + art * 0.62f);
    if (disc.icon) {
        dl->AddImage(reinterpret_cast<ImTextureID>(disc.icon), a0, a1);
    } else {
        dl->AddRectFilled(a0, a1, IM_COL32(18, 42, 84, 220), 6.0f);
        const ImVec2 cc((a0.x + a1.x) * 0.5f, (a0.y + a1.y) * 0.5f);
        dl->AddCircle(cc, art * 0.22f, with_alpha(kGlassHi, 0.55f), 0, 2.5f);
        dl->AddCircleFilled(cc, art * 0.06f, with_alpha(kGlassHi, 0.55f));
    }

    // Glass border + highlight glow on the active tile.
    dl->AddRect(p0, p1, with_alpha(kGlassHi, lerp(0.22f, 0.9f, focus)), 16.0f, 0,
                selected ? lerp(1.6f, 3.4f, focus) : 1.0f);
    if (selected) {
        dl->AddRect(ImVec2(p0.x - 5, p0.y - 5), ImVec2(p1.x + 5, p1.y + 5),
                    with_alpha(kAccent, 0.35f * focus + 0.12f * std::sin(t * 3.0f)),
                    19.0f, 0, 2.0f);
    }

    // Title + disc id under the art.
    const float ty = p1.y - ch * 0.28f;
    const char* title = disc.title.empty() ? "(untitled disc)" : disc.title.c_str();
    dl->AddText(m_font_body, 16.0f, ImVec2(p0.x + 12, ty),
                with_alpha(kTextPrimary, lerp(0.55f, 1.0f, focus)), title);
    if (!disc.disc_id.empty()) {
        dl->AddText(m_font_body, 13.0f, ImVec2(p0.x + 12, ty + 22),
                    with_alpha(kTextDim, lerp(0.45f, 1.0f, focus)),
                    disc.disc_id.c_str());
    }
}

void WmcUi::draw_browse_tile(ImDrawList* dl, const BrowseEntry& e, float cx, float cy,
                             float w, float h, float focus, bool selected, float t) {
    DiscItem tmp;
    tmp.title = e.name;
    tmp.disc_id = e.is_dir ? "Folder" : (e.is_iso ? "UMD ISO" : "File");
    draw_tile(dl, tmp, cx, cy, w, h, focus, selected, t);
    // A folder/ISO glyph in the art area so Browse tiles read distinctly.
    const float scale = lerp(0.86f, 1.0f, focus);
    const float cw = w * scale, ch = h * scale;
    const float pad = cw * 0.10f;
    const ImVec2 a0(cx - cw * 0.5f + pad, cy - ch * 0.5f + pad);
    const ImVec2 a1(a0.x + cw - pad * 2.0f, a0.y + (cw - pad * 2.0f) * 0.62f);
    const ImVec2 cc((a0.x + a1.x) * 0.5f, (a0.y + a1.y) * 0.5f);
    if (e.is_dir) {
        dl->AddRectFilled(ImVec2(cc.x - 22, cc.y - 14), ImVec2(cc.x + 22, cc.y + 16),
                          with_alpha(kAccent, 0.55f * focus + 0.2f), 4.0f);
        dl->AddRectFilled(ImVec2(cc.x - 22, cc.y - 20), ImVec2(cc.x - 4, cc.y - 12),
                          with_alpha(kAccent, 0.55f * focus + 0.2f), 3.0f);
    } else if (e.is_iso) {
        dl->AddCircleFilled(cc, 22.0f, with_alpha(kGlassHi, 0.28f * focus + 0.1f), 32);
        dl->AddCircle(cc, 22.0f, with_alpha(kAccent, 0.6f), 32, 2.0f);
        dl->AddCircleFilled(cc, 6.0f, with_alpha(kGlassHi, 0.6f), 16);
    }
}

// ---------------------------------------------------------------------------
// Vertical category list (cross-axis left column)
// ---------------------------------------------------------------------------
void WmcUi::draw_category_list(ImDrawList* dl, float x, float y, HubMode cat,
                               bool has_now_playing, float t) {
    (void)t;
    std::vector<CategoryRow> rows;
    if (has_now_playing) rows.push_back({"Now Playing", HubMode::NowPlaying});
    for (const auto& r : kCategoriesNoNp) rows.push_back(r);

    const float row_h = 52.0f;
    float ry = y;
    for (const auto& r : rows) {
        const bool active = (r.mode == cat);
        const ImVec2 r0(x, ry), r1(x + 250.0f, ry + row_h - 6.0f);
        if (active) {
            // Active category expands: brighter glass bar with an accent edge.
            dl->AddRectFilled(r0, r1, with_alpha(kCobaltGlow, 0.40f), 10.0f);
            dl->AddRect(r0, r1, with_alpha(kAccent, 0.9f), 10.0f, 0, 2.0f);
            dl->AddRectFilled(ImVec2(r1.x, r0.y + 6), ImVec2(r1.x + 5, r1.y - 6),
                              with_alpha(kAccent, 0.9f), 2.0f);
        } else {
            dl->AddRectFilled(r0, r1, IM_COL32(10, 26, 56, 120), 10.0f);
            dl->AddRect(r0, r1, with_alpha(kGlassHi, 0.14f), 10.0f);
        }
        dl->AddText(m_font_body, active ? 24.0f : 21.0f,
                    ImVec2(r0.x + 20, ry + (active ? 8.0f : 9.0f)),
                    active ? kTextPrimary : with_alpha(kTextDim, 0.95f), r.label);
        ry += row_h;
    }
}

// ---------------------------------------------------------------------------
// Lower action / browse / settings panels
// ---------------------------------------------------------------------------
void WmcUi::draw_action_panel(ImDrawList* dl, float x, float y, float w,
                              const DiscItem& disc, int action_sel, bool extras_open,
                              int extras_sel, int bonus_count) {
    const float row_h = 44.0f;
    const float gap = 10.0f;

    if (extras_open && bonus_count > 0) {
        dl->AddText(m_font_body, 15.0f, ImVec2(x + 6, y), with_alpha(kTextDim, 0.95f),
                    "BONUS CLIPS");
        float ry = y + 24.0f;
        int bonus_i = 0;
        for (const auto& s : disc.streams) {
            if (s.is_main) continue;
            const bool active = (bonus_i == extras_sel);
            const ImVec2 r0(x, ry), r1(x + w, ry + row_h - 4.0f);
            if (active) {
                dl->AddRectFilled(r0, r1, with_alpha(kCobaltGlow, 0.32f), 8.0f);
                dl->AddRect(r0, r1, with_alpha(kAccent, 0.9f), 8.0f, 0, 1.8f);
            } else {
                dl->AddRectFilled(r0, r1, IM_COL32(10, 24, 52, 160), 8.0f);
                dl->AddRect(r0, r1, with_alpha(kGlassHi, 0.16f), 8.0f);
            }
            char label[64];
            std::snprintf(label, sizeof(label), "Bonus Clip %d", bonus_i + 1);
            dl->AddText(m_font_body, 18.0f, ImVec2(r0.x + 16, ry + 9),
                        active ? kTextPrimary : with_alpha(kTextPrimary, 0.82f), label);
            const std::string sz = human_size(s.size_bytes);
            const ImVec2 ds = ImGui::CalcTextSize(sz.c_str());
            const float sc = 14.0f / ImGui::GetFontSize();
            dl->AddText(m_font_body, 14.0f, ImVec2(r1.x - 16 - ds.x * sc, ry + 13),
                        kTextDim, sz.c_str());
            ry += row_h;
            ++bonus_i;
        }
        return;
    }

    int main_idx = 0;
    for (size_t i = 0; i < disc.streams.size(); ++i) {
        if (disc.streams[i].is_main) { main_idx = static_cast<int>(i); break; }
    }
    const std::string main_size =
        disc.streams.empty() ? std::string("--") : human_size(disc.streams[main_idx].size_bytes);

    struct Row { const char* label; std::string detail; bool enabled; };
    std::vector<Row> rows;
    rows.push_back({"Play", "Main Feature " + main_size, !disc.streams.empty()});
    if (!disc.chapters.empty())
        rows.push_back({"Chapters", std::to_string(disc.chapters.size()) + " chapter(s)", true});
    if (bonus_count > 0)
        rows.push_back({"Extras", std::to_string(bonus_count) + " bonus clip(s)", true});
    rows.push_back({"Options", "Display & audio", true});

    float ry = y;
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        const bool active = (i == action_sel);
        const ImVec2 r0(x, ry), r1(x + w, ry + row_h);
        if (active) {
            dl->AddRectFilled(r0, r1, with_alpha(kCobaltGlow, 0.32f), 8.0f);
            dl->AddRect(r0, r1, with_alpha(kAccent, 0.9f), 8.0f, 0, 1.8f);
        } else {
            dl->AddRectFilled(r0, r1, IM_COL32(12, 30, 62, 160), 8.0f);
            dl->AddRect(r0, r1, with_alpha(kGlassHi, 0.16f), 8.0f);
        }
        const ImU32 fg = rows[i].enabled
                             ? (active ? kTextPrimary : with_alpha(kTextPrimary, 0.82f))
                             : with_alpha(kTextDim, 0.5f);
        dl->AddText(m_font_body, 18.0f, ImVec2(r0.x + 16, ry + 11), fg, rows[i].label);
        const ImVec2 ds = ImGui::CalcTextSize(rows[i].detail.c_str());
        const float sc = 14.0f / ImGui::GetFontSize();
        dl->AddText(m_font_body, 14.0f,
                    ImVec2(r1.x - 16 - ds.x * sc, ry + 15), with_alpha(kTextDim, 0.9f),
                    rows[i].detail.c_str());
        ry += row_h + gap;
    }
}

void WmcUi::draw_chapters_panel(ImDrawList* dl, float w, float h,
                                const std::vector<ChapterItem>& chapters,
                                int chapter_sel) {
    const float row_h = 34.0f;
    const int visible = 10;
    const float pw = std::min(w * 0.5f, 560.0f);
    const float ph = row_h * visible + 56.0f;
    const ImVec2 p0((w - pw) * 0.5f, (h - ph) * 0.5f - 10.0f);
    const ImVec2 p1(p0.x + pw, p0.y + ph);
    dl->AddRectFilled(p0, p1, IM_COL32(8, 20, 46, 236), 12.0f);
    dl->AddRect(p0, p1, with_alpha(kAccent, 0.55f), 12.0f, 0, 1.8f);

    dl->AddText(m_font_body, 18.0f, ImVec2(p0.x + 20, p0.y + 14),
                with_alpha(kTextPrimary, 0.96f), "CHAPTERS");
    char cnt[32];
    std::snprintf(cnt, sizeof(cnt), "%d marker(s)", static_cast<int>(chapters.size()));
    const ImVec2 cs = m_font_body->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, cnt);
    dl->AddText(m_font_body, 15.0f, ImVec2(p1.x - 20 - cs.x, p0.y + 18),
                with_alpha(kTextDim, 0.95f), cnt);
    dl->AddLine(ImVec2(p0.x + 20, p0.y + 44), ImVec2(p1.x - 20, p0.y + 44),
                with_alpha(kGlassHi, 0.2f), 1.0f);

    const int n = static_cast<int>(chapters.size());
    int first = 0;
    if (chapter_sel >= visible) first = chapter_sel - visible + 1;
    if (first > n - visible) first = std::max(0, n - visible);
    const int last = std::min(n, first + visible);

    float ry = p0.y + 50.0f;
    for (int i = first; i < last; ++i) {
        const bool active = (i == chapter_sel);
        const ImVec2 a0(p0.x + 12, ry), a1(p1.x - 12, ry + row_h - 4.0f);
        if (active) {
            dl->AddRectFilled(a0, a1, with_alpha(kCobaltGlow, 0.36f), 8.0f);
            dl->AddRect(a0, a1, with_alpha(kAccent, 0.9f), 8.0f, 0, 1.8f);
        }
        char idx[16];
        std::snprintf(idx, sizeof(idx), "%02d", i + 1);
        dl->AddText(m_font_body, 15.0f, ImVec2(a0.x + 12, ry + 6),
                    with_alpha(active ? kAccent : kTextDim, 0.95f), idx);
        std::string label = chapters[i].label;
        if (label.size() > 30) label = label.substr(0, 29) + "\u2026";
        dl->AddText(m_font_body, 17.0f, ImVec2(a0.x + 52, ry + 4),
                    active ? kTextPrimary : with_alpha(kTextPrimary, 0.82f), label.c_str());
        char ts[16];
        format_time(chapters[i].seconds, ts, sizeof(ts));
        const ImVec2 tsz = m_font_body->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, ts);
        dl->AddText(m_font_body, 15.0f, ImVec2(a1.x - 12 - tsz.x, ry + 6),
                    with_alpha(kTextDim, 0.95f), ts);
        ry += row_h;
    }
    // Scroll hint when the list is longer than the window.
    if (n > visible) {
        char sc[48];
        std::snprintf(sc, sizeof(sc), "%d-%d of %d", first + 1, last, n);
        dl->AddText(m_font_body, 13.0f, ImVec2(p0.x + 20, p1.y - 20),
                    with_alpha(kTextDim, 0.9f), sc);
    }
}

void WmcUi::draw_browse_panel(ImDrawList* dl, float x, float y, float w,
                              const std::string& path,
                              const std::vector<BrowseEntry>& entries,
                              int browse_sel) {
    const float row_h = 34.0f;
    const int visible = 8;
    const ImVec2 r0(x, y), r1(x + w, y + row_h * visible + 16.0f);
    dl->AddRectFilled(r0, r1, IM_COL32(8, 20, 46, 210), 10.0f);
    dl->AddRect(r0, r1, with_alpha(kGlassHi, 0.22f), 10.0f);

    dl->AddText(m_font_body, 14.0f, ImVec2(x + 14, y + 8),
                with_alpha(kTextDim, 0.95f), ("PATH  " + path).c_str());

    if (entries.empty()) {
        dl->AddText(m_font_body, 16.0f, ImVec2(x + 16, y + 40),
                    with_alpha(kTextDim, 0.8f), "(no folders or .iso files here)");
        return;
    }

    const int first = std::clamp(browse_sel - visible / 2, 0,
                                 std::max(0, static_cast<int>(entries.size()) - visible));
    float ry = y + 32.0f;
    for (int i = first; i < std::min<int>(first + visible, static_cast<int>(entries.size())); ++i) {
        const BrowseEntry& e = entries[i];
        const bool active = (i == browse_sel);
        const ImVec2 a(x + 8, ry), b(x + w - 8, ry + row_h - 4.0f);
        if (active) {
            dl->AddRectFilled(a, b, with_alpha(kCobaltGlow, 0.30f), 6.0f);
            dl->AddRect(a, b, with_alpha(kAccent, 0.85f), 6.0f, 0, 1.5f);
        }
        const char* glyph = e.is_dir ? "[DIR]" : (e.is_iso ? "[ISO]" : "     ");
        dl->AddText(m_font_body, 16.0f, ImVec2(a.x + 10, ry + 6),
                    active ? kTextPrimary : with_alpha(kTextPrimary, 0.8f), glyph);
        std::string name = e.name;
        if (name.size() > 52) name = name.substr(0, 49) + "...";
        dl->AddText(m_font_body, 16.0f, ImVec2(a.x + 66, ry + 6),
                    active ? kTextPrimary : with_alpha(kTextPrimary, 0.82f), name.c_str());
        ry += row_h;
    }
    if (static_cast<int>(entries.size()) > visible) {
        char cnt[32];
        std::snprintf(cnt, sizeof(cnt), "%d items", static_cast<int>(entries.size()));
        dl->AddText(m_font_body, 13.0f, ImVec2(x + w - 90, y + 8),
                    with_alpha(kTextDim, 0.8f), cnt);
    }
}

void WmcUi::draw_settings_panel(ImDrawList* dl, float x, float y, float w,
                                int setting_sel, bool fullscreen, float volume,
                                bool muted, bool ui_sounds) {
    const float row_h = 52.0f;
    const float gap = 10.0f;
    const int   rows = 4;
    const char* labels[4] = {"Display Mode", "Volume", "Mute", "UI Sounds"};
    char values[4][48];
    std::snprintf(values[0], sizeof(values[0]), "%s", fullscreen ? "Fullscreen" : "Windowed");
    std::snprintf(values[1], sizeof(values[1]), "%d%%", static_cast<int>(volume * 100.0f + 0.5f));
    std::snprintf(values[2], sizeof(values[2]), "%s", muted ? "On" : "Off");
    std::snprintf(values[3], sizeof(values[3]), "%s", ui_sounds ? "On" : "Off");

    float ry = y;
    for (int i = 0; i < rows; ++i) {
        const bool active = (i == setting_sel);
        const ImVec2 r0(x, ry), r1(x + w, ry + row_h);
        if (active) {
            dl->AddRectFilled(r0, r1, with_alpha(kCobaltGlow, 0.32f), 8.0f);
            dl->AddRect(r0, r1, with_alpha(kAccent, 0.9f), 8.0f, 0, 1.8f);
        } else {
            dl->AddRectFilled(r0, r1, IM_COL32(12, 30, 62, 160), 8.0f);
            dl->AddRect(r0, r1, with_alpha(kGlassHi, 0.16f), 8.0f);
        }
        dl->AddText(m_font_body, 18.0f, ImVec2(r0.x + 16, ry + 15),
                    active ? kTextPrimary : with_alpha(kTextPrimary, 0.85f), labels[i]);

        if (i == 1) {
            const float bx0 = r0.x + w * 0.46f, bx1 = r1.x - 110.0f, by = ry + 23.0f;
            dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx1, by + 6),
                              with_alpha(kTextDim, 0.35f), 3.0f);
            dl->AddRectFilled(ImVec2(bx0, by),
                              ImVec2(bx0 + (bx1 - bx0) * std::clamp(volume, 0.f, 1.f), by + 6),
                              with_alpha(kAccent, 1.0f), 3.0f);
            dl->AddCircleFilled(ImVec2(bx0 + (bx1 - bx0) * std::clamp(volume, 0.f, 1.f), by + 3),
                                7.0f, kTextPrimary);
        }
        const ImVec2 vs = ImGui::CalcTextSize(values[i]);
        const float sc = 17.0f / ImGui::GetFontSize();
        dl->AddText(m_font_body, 17.0f, ImVec2(r1.x - 20 - vs.x * sc, ry + 16),
                    active ? kTextPrimary : kTextDim, values[i]);
        ry += row_h + gap;
    }
}

// ---------------------------------------------------------------------------
// Mini "Now Playing" PIP (bottom-left) + glossy hub transport (bottom-right)
// ---------------------------------------------------------------------------
void WmcUi::draw_now_playing_pip(ImDrawList* dl, float w, float h,
                                 const NowPlaying& np, SDL_Texture* video_tex,
                                 int video_w, int video_h, float t) {
    (void)w; (void)t;
    const float pw = 300.0f, ph = 96.0f;
    const ImVec2 p0(48.0f, footer_top(h) - ph - 18.0f);
    const ImVec2 p1(p0.x + pw, p0.y + ph);

    dl->AddRectFilled(ImVec2(p0.x + 4, p0.y + 6), ImVec2(p1.x + 4, p1.y + 6),
                      IM_COL32(0, 0, 0, 120), 12.0f);
    dl->AddRectFilledMultiColor(p0, p1, IM_COL32(30, 74, 140, 235),
                                IM_COL32(30, 74, 140, 235),
                                IM_COL32(8, 20, 46, 235), IM_COL32(8, 20, 46, 235));
    dl->AddRect(p0, p1, with_alpha(kAccent, 0.55f), 12.0f, 0, 1.6f);

    // Live video thumbnail (aspect-fit) when a frame is available, else a glyph.
    const ImVec2 th(p0.x + 12, p0.y + 12);
    const ImVec2 th1(th.x + 108, p1.y - 12);
    dl->AddRectFilled(th, th1, IM_COL32(4, 12, 28, 220), 6.0f);
    if (video_tex && video_w > 0 && video_h > 0) {
        const float va = static_cast<float>(video_w) / static_cast<float>(video_h);
        const float ta = (th1.x - th.x) / (th1.y - th.y);
        float dw = th1.x - th.x, dh = th1.y - th.y;
        if (ta > va) dw = (th1.y - th.y) * va; else dh = (th1.x - th.x) / va;
        const ImVec2 c(th.x + ((th1.x - th.x) - dw) * 0.5f,
                       th.y + ((th1.y - th.y) - dh) * 0.5f);
        dl->PushClipRect(th, th1, true);
        dl->AddImage(reinterpret_cast<ImTextureID>(video_tex), c,
                     ImVec2(c.x + dw, c.y + dh));
        dl->PopClipRect();
    } else {
        const ImVec2 c((th.x + th1.x) * 0.5f, (th.y + th1.y) * 0.5f);
        dl->AddCircle(c, 26.0f, with_alpha(kGlassHi, 0.35f), 32, 2.5f);
        dl->AddCircleFilled(c, 6.0f, with_alpha(kGlassHi, 0.5f));
    }
    dl->AddRect(th, th1, with_alpha(kGlassHi, 0.25f), 6.0f);
    if (np.paused) {
        const ImVec2 c((th.x + th1.x) * 0.5f, (th.y + th1.y) * 0.5f);
        dl->AddCircleFilled(c, 18.0f, IM_COL32(0, 0, 0, 150), 24);
        dl->AddRectFilled(ImVec2(c.x - 6, c.y - 9), ImVec2(c.x - 1, c.y + 9),
                          with_alpha(kTextPrimary, 0.95f));
        dl->AddRectFilled(ImVec2(c.x + 2, c.y - 9), ImVec2(c.x + 7, c.y + 9),
                          with_alpha(kTextPrimary, 0.95f));
    }

    std::string title = np.title.empty() ? std::string("(untitled)") : np.title;
    if (title.size() > 24) title = title.substr(0, 21) + "...";
    dl->AddText(m_font_body, 13.0f, ImVec2(th1.x + 14, p0.y + 14),
                with_alpha(kAccent, 0.95f), "NOW PLAYING");
    dl->AddText(m_font_body, 17.0f, ImVec2(th1.x + 14, p0.y + 34),
                kTextPrimary, title.c_str());

    char ts[48];
    char te[16], tt[16];
    format_time(np.elapsed, te, sizeof(te));
    format_time(np.total, tt, sizeof(tt));
    std::snprintf(ts, sizeof(ts), "%s / %s", te, tt);
    dl->AddText(m_font_body, 13.0f, ImVec2(th1.x + 14, p0.y + 60),
                with_alpha(kTextDim, 0.95f), ts);
}

void WmcUi::draw_hub_transport(ImDrawList* dl, float w, float h, bool paused,
                               float t, WmcInput& out) {
    (void)t;
    const float by = footer_top(h) - 44.0f;
    const float r = 22.0f;
    float cx = w - 48.0f - r;

    auto circle = [&](const char* glyph, bool primary) -> bool {
        const ImVec2 c(cx, by);
        const bool hov = ImGui::IsMouseHoveringRect(ImVec2(c.x - r, c.y - r),
                                                    ImVec2(c.x + r, c.y + r));
        dl->AddCircleFilled(c, r, with_alpha(hov ? kAccent : (primary ? kCobaltGlow
                                            : IM_COL32(24, 56, 110, 230)), 0.96f));
        dl->AddCircle(c, r, with_alpha(kGlassHi, 0.5f), 0, 1.6f);
        dl->AddCircleFilled(ImVec2(c.x, c.y - r * 0.35f), r * 0.5f,
                            with_alpha(IM_COL32(255, 255, 255, 40), 0.6f));
        const ImVec2 ts = m_font_body->CalcTextSizeA(16.0f, FLT_MAX, 0.0f, glyph);
        dl->AddText(m_font_body, 16.0f, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                    with_alpha(kTextPrimary, 0.96f), glyph);
        cx -= r * 2.0f + 14.0f;
        return hov && ImGui::IsMouseClicked(0);
    };

    // Left to right on screen (drawn right to left): Fullscreen, Play/Pause, Hub.
    if (circle("FS", false)) out.action = WmcAction::ToggleFullscreen;
    if (circle(paused ? ">" : "||", true)) out.action = WmcAction::TogglePause;
    if (circle("HUB", false)) out.action = WmcAction::BackToPlayback;
}

// ---------------------------------------------------------------------------
// Footer hint bar
// ---------------------------------------------------------------------------
void WmcUi::draw_footer(ImDrawList* dl, float w, float h, const std::string& hint) {
    const float ft = footer_top(h);
    dl->AddRectFilled(ImVec2(0, ft), ImVec2(w, h), IM_COL32(3, 8, 20, 200));
    dl->AddLine(ImVec2(0, ft), ImVec2(w, ft), with_alpha(kGlassHi, 0.18f), 1.0f);
    dl->AddText(m_font_body, 15.0f, ImVec2(48, ft + 18), with_alpha(kTextDim, 0.95f),
                hint.c_str());
}

// ---------------------------------------------------------------------------
// Hub: WMC cross-axis carousel (vertical categories + horizontal glass tiles)
// ---------------------------------------------------------------------------
WmcInput WmcUi::render_hub(SDL_Texture* video_tex, int video_w, int video_h,
                           const std::vector<DiscItem>& recent, int& recent_sel,
                           const std::vector<BrowseEntry>& browse, int& browse_sel,
                           const std::string& browse_path,
                           HubMode& category,
                           int& action_sel, bool& extras_open, int& extras_sel,
                           bool& chapters_open, int& chapter_sel,
                           int& setting_sel,
                           const NowPlaying& now_playing, bool loading,
                           bool fullscreen, float volume, bool muted,
                           bool ui_sounds, float t) {
    WmcInput out;
    if (!m_imgui_ready) return out;

    ImGuiIO& io = ImGui::GetIO();
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("##wmc_hub", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Drain pointer gestures (touch + mouse-drag parity) for this frame.
    std::vector<gesture::Event> ges;
    collect_gestures(true, ges);

    // Category rows, ordered; NowPlaying is only present while a stream is open.
    const bool has_np = now_playing.active;
    std::vector<HubMode> cats;
    if (has_np) cats.push_back(HubMode::NowPlaying);
    for (const auto& r : kCategoriesNoNp) cats.push_back(r.mode);
    auto cat_index = [&](HubMode m) {
        for (int i = 0; i < static_cast<int>(cats.size()); ++i) if (cats[i] == m) return i;
        return 0;
    };

    // Focus model: 0 = categories, 1 = tiles, 2 = panel rows.
    if (category != m_last_strip) { m_focus = 1; extras_open = false; chapters_open = false; m_last_strip = category; }
    m_focus = std::clamp(m_focus, 0, 2);

    const DiscItem* sel = nullptr;
    int tile_count = 0;
    if (category == HubMode::Recent && !recent.empty()) {
        recent_sel = std::clamp(recent_sel, 0, static_cast<int>(recent.size()) - 1);
        sel = &recent[recent_sel];
        tile_count = static_cast<int>(recent.size());
    } else if (category == HubMode::Browse) {
        browse_sel = browse.empty() ? 0 : std::clamp(browse_sel, 0, static_cast<int>(browse.size()) - 1);
        tile_count = static_cast<int>(browse.size());
    }

    draw_aurora(dl, w, h, t, sel ? sel->pic1 : nullptr);

    // --- Header: animated UMD logo (top-left) + clock (top-right) ---
    draw_umd_logo(dl, 98.0f, 72.0f, 62.0f, t, loading);
    dl->AddText(m_font_huge, 30.0f, ImVec2(182, 46), kTextPrimary, "UMDPlay");
    dl->AddText(m_font_body, 13.0f, ImVec2(184, 82), kTextDim,
                (std::string("UMD VIDEO  ") + kBullet + "  PRESERVATION LIBRARY").c_str());
    draw_clock(dl, w, h);

    // --- Vertical category column ---
    const float cat_x = 48.0f, cat_y = 150.0f;
    const float cat_row_h = 52.0f;
    draw_category_list(dl, cat_x, cat_y, category, has_np, t);

    // Read each key at most once per frame.
    bool used_l = false, used_r = false, used_u = false, used_d = false, used_e = false;
    auto take_l = [&] { if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) && !used_l) { used_l = true; return true; } return false; };
    auto take_r = [&] { if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) && !used_r) { used_r = true; return true; } return false; };
    auto take_u = [&] { if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) && !used_u) { used_u = true; return true; } return false; };
    auto take_d = [&] { if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) && !used_d) { used_d = true; return true; } return false; };
    auto take_e = [&] { if ((ImGui::IsKeyPressed(ImGuiKey_Enter, true) ||
                             ImGui::IsKeyPressed(ImGuiKey_Space, true)) && !used_e) { used_e = true; return true; } return false; };

    // --- Gesture routing ------------------------------------------------------
    // Vertical swipes move the category; horizontal swipes fling the tile strip.
    int cat_i = cat_index(category);
    for (const auto& g : ges) {
        if (g.type == gesture::Type::Swipe && g.axis == gesture::Axis::Vertical &&
            m_focus == 0) {
            cat_i = std::clamp(cat_i + (g.dy < 0 ? 1 : -1), 0, static_cast<int>(cats.size()) - 1);
            category = cats[cat_i];
        } else if (g.type == gesture::Type::Swipe && g.axis == gesture::Axis::Horizontal &&
                   m_focus == 1 && tile_count > 0) {
            // Fling the strip: velocity is px/s, convert to tiles/s.
            const float spacing = std::min(w * 0.20f, 250.0f);
            m_scroll_vel = -(g.vx / spacing);
        } else if (g.type == gesture::Type::Tap) {
            // Taps activate: category column taps change category; tile taps open.
            if (g.x < 320.0f) {
                const int r = static_cast<int>((g.y - cat_y) / cat_row_h);
                if (r >= 0 && r < static_cast<int>(cats.size())) {
                    category = cats[r];
                    m_focus = (category == HubMode::NowPlaying) ? 1 : 0;
                }
            } else if (m_focus == 1) {
                if (category == HubMode::Recent && sel) {
                    out.action = WmcAction::OpenIso; out.iso_path = sel->iso_path;
                } else if (category == HubMode::Browse && !browse.empty()) {
                    const BrowseEntry& e = browse[browse_sel];
                    if (e.is_dir) { out.action = WmcAction::BrowseEnter; out.iso_path = e.path; }
                    else if (e.is_iso) { out.action = WmcAction::OpenIso; out.iso_path = e.path; }
                }
            }
        }
    }

    // --- Focus 0: category column (keyboard) ---
    if (m_focus == 0) {
        if (take_u() && cat_i > 0) { category = cats[--cat_i]; out.cue = umdplay::UiCue::Navigate; }
        if (take_d() && cat_i + 1 < static_cast<int>(cats.size())) { category = cats[++cat_i]; out.cue = umdplay::UiCue::Navigate; }
        if (take_r() || take_e()) {
            if (category == HubMode::NowPlaying) out.action = WmcAction::BackToPlayback;
            else { m_focus = 1; out.cue = umdplay::UiCue::Navigate; }
        }
    }

    // --- Horizontal tile strip (right of the categories) ---
    const float strip_x0 = 330.0f;
    const float strip_cx = (strip_x0 + w) * 0.5f;
    const float tile_w = std::min(w * 0.19f, 224.0f);
    const float tile_h = tile_w * 1.18f;
    const float tile_cy = cat_y + tile_h * 0.5f + 40.0f;
    const float spacing = tile_w * 1.16f;
    const bool on_tiles = (m_focus == 1);

    // Authoritative float position + inertia.
    if (tile_count > 0) {
        if (std::fabs(m_tile_pos - static_cast<float>(category == HubMode::Browse ? browse_sel : recent_sel)) > 2.0f) {
            // Selection jumped externally (open/enter): snap the carousel.
            m_tile_pos = static_cast<float>(category == HubMode::Browse ? browse_sel : recent_sel);
        }
        // Integrate inertia from a fling.
        if (std::fabs(m_scroll_vel) > 0.01f) {
            m_tile_pos += m_scroll_vel * io.DeltaTime;
            m_scroll_vel *= std::pow(0.02f, io.DeltaTime);   // exponential decay
            if (std::fabs(m_scroll_vel) < 0.05f) m_scroll_vel = 0.0f;
        }
        m_tile_pos = std::clamp(m_tile_pos, 0.0f, static_cast<float>(tile_count - 1));
        if (m_scroll_vel == 0.0f) {
            // Settle onto the nearest tile when not flinging.
            const float target = std::round(m_tile_pos);
            m_tile_pos = lerp(m_tile_pos, target, std::clamp(io.DeltaTime * 12.0f, 0.0f, 1.0f));
            if (std::fabs(m_tile_pos - target) < 0.01f) m_tile_pos = target;
        }
        if (category == HubMode::Browse) browse_sel = std::clamp(static_cast<int>(std::lround(m_tile_pos)), 0, tile_count - 1);
        else recent_sel = std::clamp(static_cast<int>(std::lround(m_tile_pos)), 0, tile_count - 1);
    } else {
        m_tile_pos = 0.0f; m_scroll_vel = 0.0f;
    }
    m_tile_scroll = lerp(m_tile_scroll, m_tile_pos, std::clamp(io.DeltaTime * 10.0f, 0.0f, 1.0f));

    if (category == HubMode::Recent) {
        if (recent.empty()) {
            dl->AddText(m_font_body, 17.0f, ImVec2(strip_cx - 190, tile_cy - 8),
                        with_alpha(kTextDim, 0.85f),
                        "No recent discs yet - open one from Browse System.");
        }
        for (int i = 0; i < static_cast<int>(recent.size()); ++i) {
            const float cx = strip_cx + (static_cast<float>(i) - m_tile_scroll) * spacing;
            if (cx < strip_x0 - tile_w || cx > w + tile_w) continue;
            const float dist = std::fabs(static_cast<float>(i) - m_tile_scroll);
            const float focus = std::clamp(1.0f - dist * 0.5f, 0.0f, 1.0f);
            draw_tile(dl, recent[i], cx, tile_cy, tile_w, tile_h, focus,
                      (i == recent_sel) && on_tiles, t);
        }
        if (on_tiles) {
            if (take_l() && recent_sel > 0) { recent_sel--; m_tile_pos = recent_sel; out.cue = umdplay::UiCue::Navigate; }
            if (take_r() && recent_sel + 1 < static_cast<int>(recent.size())) { recent_sel++; m_tile_pos = recent_sel; out.cue = umdplay::UiCue::Navigate; }
            if (take_e() && sel) { out.action = WmcAction::OpenIso; out.iso_path = sel->iso_path; out.cue = umdplay::UiCue::Select; }
            if (take_u()) { m_focus = 0; out.cue = umdplay::UiCue::Navigate; }
            if (take_d()) { m_focus = 2; out.cue = umdplay::UiCue::Navigate; }
        }
    } else if (category == HubMode::Browse) {
        for (int i = 0; i < static_cast<int>(browse.size()); ++i) {
            const float cx = strip_cx + (static_cast<float>(i) - m_tile_scroll) * spacing;
            if (cx < strip_x0 - tile_w || cx > w + tile_w) continue;
            const float dist = std::fabs(static_cast<float>(i) - m_tile_scroll);
            const float focus = std::clamp(1.0f - dist * 0.5f, 0.0f, 1.0f);
            draw_browse_tile(dl, browse[i], cx, tile_cy, tile_w, tile_h, focus,
                             (i == browse_sel) && on_tiles, t);
        }
        if (on_tiles) {
            if (take_l() && browse_sel > 0) { browse_sel--; m_tile_pos = browse_sel; out.cue = umdplay::UiCue::Navigate; }
            if (take_r() && browse_sel + 1 < static_cast<int>(browse.size())) { browse_sel++; m_tile_pos = browse_sel; out.cue = umdplay::UiCue::Navigate; }
            if (take_e() && browse_sel >= 0 && browse_sel < static_cast<int>(browse.size())) {
                const BrowseEntry& e = browse[browse_sel];
                if (e.is_dir) { out.action = WmcAction::BrowseEnter; out.iso_path = e.path; out.cue = umdplay::UiCue::Select; }
                else if (e.is_iso) { out.action = WmcAction::OpenIso; out.iso_path = e.path; out.cue = umdplay::UiCue::Select; }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) { out.action = WmcAction::BrowseUp; out.cue = umdplay::UiCue::Back; }
            if (take_u()) { m_focus = 0; out.cue = umdplay::UiCue::Navigate; }
            if (take_d()) { m_focus = 2; out.cue = umdplay::UiCue::Navigate; }
        }
    }

    // --- Lower panel ---
    const float panel_top = footer_top(h) - 96.0f;   // leave room for the PIP/transport
    const float panel_y = tile_cy + tile_h * 0.5f + 16.0f;
    const float panel_w = std::min(w * 0.58f, 720.0f);
    const float panel_x = strip_x0 + (w - strip_x0 - panel_w) * 0.5f;
    dl->PushClipRect(ImVec2(0, panel_y), ImVec2(w, panel_top), true);

    if (category == HubMode::Recent) {
        const char* title = sel ? (sel->title.empty() ? "(untitled disc)" : sel->title.c_str())
                                : "(no disc selected)";
        const float title_px = 22.0f;
        const float tw = ImGui::CalcTextSize(title).x * (title_px / ImGui::GetFontSize());
        dl->AddText(m_font_title, title_px, ImVec2(strip_x0 + (w - strip_x0 - tw) * 0.5f, panel_y),
                    kTextPrimary, title);
        int bonus = 0;
        if (sel) for (const auto& s : sel->streams) if (!s.is_main) ++bonus;
        const int chapters = sel ? static_cast<int>(sel->chapters.size()) : 0;
        const float ap_y = panel_y + 34.0f;
        if (sel && (panel_top - ap_y) > 56.0f) {
            // Row layout: Play, [Chapters], [Extras], Options.
            const int chapters_idx = chapters > 0 ? 1 : -1;
            const int extras_idx   = chapters > 0 ? 2 : 1;
            const int options_idx  = (chapters > 0 ? 1 : 0) + (bonus > 0 ? 1 : 0) + 1;
            const int nrows        = (chapters > 0 ? 1 : 0) + (bonus > 0 ? 1 : 0) + 2;
            action_sel = std::clamp(action_sel, 0, nrows - 1);
            draw_action_panel(dl, panel_x, ap_y, panel_w, *sel, action_sel,
                              extras_open, extras_sel, bonus);
            if (m_focus == 2) {
                if (chapters_open && chapters > 0) {
                    chapter_sel = std::clamp(chapter_sel, 0, chapters - 1);
                    if (take_u() && chapter_sel > 0) { chapter_sel--; out.cue = umdplay::UiCue::Navigate; }
                    if (take_d() && chapter_sel + 1 < chapters) { chapter_sel++; out.cue = umdplay::UiCue::Navigate; }
                    if (take_l() || take_r()) { chapters_open = false; out.cue = umdplay::UiCue::Back; }
                    if (take_e()) {
                        out.action = WmcAction::PlayChapter;
                        out.chapter_index = chapter_sel;
                        out.cue = umdplay::UiCue::Select;
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape, true)) { chapters_open = false; out.cue = umdplay::UiCue::Back; }
                } else if (extras_open && bonus > 0) {
                    if (take_u() && extras_sel > 0) { extras_sel--; out.cue = umdplay::UiCue::Navigate; }
                    if (take_d() && extras_sel + 1 < bonus) { extras_sel++; out.cue = umdplay::UiCue::Navigate; }
                    if (take_l() || take_r()) { extras_open = false; out.cue = umdplay::UiCue::Back; }
                    if (take_e()) {
                        int b = 0;
                        for (int i = 0; i < static_cast<int>(sel->streams.size()); ++i) {
                            if (sel->streams[i].is_main) continue;
                            if (b == extras_sel) { out.action = WmcAction::PlayStream; out.stream_index = i; break; }
                            ++b;
                        }
                        out.cue = umdplay::UiCue::Select;
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape, true)) { extras_open = false; out.cue = umdplay::UiCue::Back; }
                } else {
                    if (take_u()) { if (action_sel > 0) { action_sel--; out.cue = umdplay::UiCue::Navigate; } else { m_focus = 1; out.cue = umdplay::UiCue::Navigate; } }
                    if (take_d() && action_sel + 1 < nrows) { action_sel++; out.cue = umdplay::UiCue::Navigate; }
                    if (take_e()) {
                        if (action_sel == 0) out.action = WmcAction::Play;
                        else if (action_sel == options_idx) category = HubMode::Settings;
                        else if (action_sel == chapters_idx) { chapters_open = true; chapter_sel = 0; }
                        else if (action_sel == extras_idx) { extras_open = true; extras_sel = 0; }
                        out.cue = umdplay::UiCue::Select;
                    }
                }
            }
            // Chapters sub-page is drawn as a centred modal after the panel
            // clip rect is popped (see below), so a long list is not clipped.
        } else if (!sel) {
            dl->AddText(m_font_body, 16.0f, ImVec2(panel_x, ap_y),
                        with_alpha(kTextDim, 0.8f),
                        "Open an ISO from Browse System to populate your library.");
        }
    } else if (category == HubMode::Browse) {
        const float bh = std::min(panel_top - panel_y, 8 * 34.0f + 16.0f);
        if (bh > 80.0f) {
            draw_browse_panel(dl, panel_x, panel_y, panel_w, browse_path, browse, browse_sel);
            if (m_focus == 2 && take_u()) { m_focus = 1; out.cue = umdplay::UiCue::Navigate; }
        }
    } else if (category == HubMode::Settings) {
        const float sh = std::min(panel_top - panel_y, 4 * 62.0f + 10.0f);
        if (sh > 80.0f) {
            if (m_focus == 2) {
                if (take_u()) { if (setting_sel > 0) { setting_sel--; out.cue = umdplay::UiCue::Navigate; } else m_focus = 1; }
                if (take_d() && setting_sel < 3) { setting_sel++; out.cue = umdplay::UiCue::Navigate; }
                const bool sl = take_l();
                const bool sr = take_r();
                if (sl || sr) {
                    if (setting_sel == 0) out.action = WmcAction::ToggleFullscreen;
                    else if (setting_sel == 1) {
                        out.action = WmcAction::VolumeChanged;
                        out.volume = std::clamp(volume + (sr ? 0.05f : -0.05f), 0.0f, 1.0f);
                    } else if (setting_sel == 2) out.action = WmcAction::MuteToggled;
                    else out.action = WmcAction::ToggleUiSounds;
                    if (out.cue == umdplay::UiCue::None) out.cue = umdplay::UiCue::Navigate;
                }
                if (take_e()) {
                    if (setting_sel == 0) out.action = WmcAction::ToggleFullscreen;
                    else if (setting_sel == 2) out.action = WmcAction::MuteToggled;
                    else if (setting_sel == 3) out.action = WmcAction::ToggleUiSounds;
                    if (out.cue == umdplay::UiCue::None) out.cue = umdplay::UiCue::Select;
                }
            }
            draw_settings_panel(dl, panel_x, panel_y, panel_w, setting_sel,
                                fullscreen, volume, muted, ui_sounds);
        }
    }
    dl->PopClipRect();

    // Chapters modal: centred over the tile area, above the panels.
    if (chapters_open && category == HubMode::Recent && sel && !sel->chapters.empty()) {
        chapter_sel = std::clamp(chapter_sel, 0, static_cast<int>(sel->chapters.size()) - 1);
        draw_chapters_panel(dl, w, h, sel->chapters, chapter_sel);
    }

    // --- Mini "Now Playing" PIP + glossy transport ---
    if (has_np) {
        draw_now_playing_pip(dl, w, h, now_playing, video_tex, video_w, video_h, t);
        draw_hub_transport(dl, w, h, now_playing.paused, t, out);
        // A click on the PIP jumps back to fullscreen playback.
        const ImVec2 p0(48.0f, footer_top(h) - 96.0f - 18.0f);
        if (ImGui::IsMouseHoveringRect(p0, ImVec2(p0.x + 300.0f, p0.y + 96.0f)) &&
            ImGui::IsMouseClicked(0)) {
            out.action = WmcAction::BackToPlayback;
        }
    }

    // --- Footer hints ---
    std::string hint;
    if (m_focus == 0)
        hint = std::string("UP / DOWN  category     RIGHT  tiles     ENTER  open     ")
             + "SWIPE  scroll     F11  fullscreen     ESC / Q  quit";
    else if (m_focus == 1)
        hint = std::string("LEFT / RIGHT  move     UP  categories     DOWN  actions     ")
             + "ENTER  open     SWIPE  fling     F11  fullscreen     ESC / Q  quit";
    else
        hint = std::string("UP / DOWN  select     ENTER / LEFT / RIGHT  activate     ")
             + "F11  fullscreen     ESC / Q  quit";
    draw_footer(dl, w, h, hint);

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, true)) {
        if (chapters_open) chapters_open = false;
        else if (extras_open) extras_open = false;
        else if (m_focus > 0) m_focus = 0;
        else if (category != HubMode::Recent && category != HubMode::NowPlaying) category = HubMode::Recent;
        else out.action = WmcAction::Quit;
    }

    ImGui::End();
    return out;
}

// ---------------------------------------------------------------------------
// Gesture plumbing
// ---------------------------------------------------------------------------
void WmcUi::feed_finger(const SDL_Event& e) {
    // SDL reports finger coords normalised [0,1]; scale to window pixels.
    int ww = 1280, wh = 720;
    if (m_window) SDL_GetWindowSize(m_window, &ww, &wh);
    PointerSample s;
    s.x = e.tfinger.x * static_cast<float>(ww);
    s.y = e.tfinger.y * static_cast<float>(wh);
    s.t = static_cast<double>(e.tfinger.timestamp) / 1000.0;
    s.mouse = false;
    if (e.type == SDL_FINGERDOWN)      s.kind = 0;
    else if (e.type == SDL_FINGERMOTION) s.kind = 1;
    else                               s.kind = 2;
    m_ptr_queue.push_back(s);
}

void WmcUi::feed_mouse() {
    // Mouse-drag parity for desktop testing. We only feed the recognizer when
    // the left button is engaged, so ordinary hovering never becomes a gesture.
    const Uint32 state = SDL_GetMouseState(nullptr, nullptr);
    const bool down = (state & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    const double now_ms = static_cast<double>(SDL_GetTicks());
    PointerSample s;
    s.x = static_cast<float>(mx);
    s.y = static_cast<float>(my);
    s.t = now_ms / 1000.0;
    s.mouse = true;
    if (down && !m_mouse_gesture) { s.kind = 0; m_mouse_gesture = true; m_ptr_queue.push_back(s); }
    else if (down && m_mouse_gesture) { s.kind = 1; m_ptr_queue.push_back(s); }
    else if (!down && m_mouse_gesture) { s.kind = 2; m_mouse_gesture = false; m_ptr_queue.push_back(s); }
}

void WmcUi::collect_gestures(bool allow_taps, std::vector<gesture::Event>& out) {
    out.clear();
    // Do not synthesise gestures while the ImGui pointer is captured by a
    // widget (e.g. an ImGui slider), to avoid double-handling.
    feed_mouse();

    // Drop mouse samples when a touch is active (touch wins on hybrid devices).
    for (const auto& s : m_ptr_queue) {
        if (!s.mouse) m_touch_active = (s.kind != 2);
    }

    double last_t = 0.0;
    for (const auto& s : m_ptr_queue) {
        if (s.mouse && m_touch_active) continue;   // a finger owns the gesture
        gesture::Event e;
        int n = 0;
        if (s.kind == 0)      m_gesture.begin(s.x, s.y, s.t);
        else if (s.kind == 1) m_gesture.move(s.x, s.y, s.t);
        else                  n = m_gesture.end(s.x, s.y, s.t, &e);
        if (n > 0) { if (allow_taps || e.type != gesture::Type::Tap) out.push_back(e); }
        last_t = s.t;
    }
    m_ptr_queue.clear();

    // Flush a deferred single tap once the double-tap window has elapsed.
    gesture::Event e;
    if (m_gesture.tick(last_t, &e) > 0) {
        if (allow_taps) out.push_back(e);
    }
}

// ---------------------------------------------------------------------------
// Player gesture HUDs (seek scrub, double-tap indicator, volume)
// ---------------------------------------------------------------------------
void WmcUi::draw_player_huds(ImDrawList* dl, float w, float h, float alpha) {
    (void)alpha;
    // Centered seek/scrub HUD.
    if (m_scrub_hud > 0.01f) {
        char te[16];
        format_time(m_scrub_target, te, sizeof(te));
        char line[48];
        std::snprintf(line, sizeof(line), "\xE2\x86\x92  %s", te);
        const float px = 40.0f;
        const ImVec2 ts = m_font_body->CalcTextSizeA(px, FLT_MAX, 0.0f, line);
        const ImVec2 c(w * 0.5f, h * 0.5f);
        const float pad_x = 34.0f, pad_y = 20.0f;
        dl->AddRectFilled(ImVec2(c.x - ts.x * 0.5f - pad_x, c.y - ts.y * 0.5f - pad_y),
                          ImVec2(c.x + ts.x * 0.5f + pad_x, c.y + ts.y * 0.5f + pad_y),
                          with_alpha(IM_COL32(8, 20, 46, 230), m_scrub_hud), 14.0f);
        dl->AddRect(ImVec2(c.x - ts.x * 0.5f - pad_x, c.y - ts.y * 0.5f - pad_y),
                    ImVec2(c.x + ts.x * 0.5f + pad_x, c.y + ts.y * 0.5f + pad_y),
                    with_alpha(kAccent, 0.7f * m_scrub_hud), 14.0f, 0, 1.6f);
        dl->AddText(m_font_body, px, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                    with_alpha(kTextPrimary, m_scrub_hud), line);
    }
    // Double-tap -10s / +10s indicator on the tapped half.
    if (m_dt_hud > 0.01f) {
        const bool left = (m_dt_delta < 0.0f);
        const float cx = left ? w * 0.25f : w * 0.75f;
        const ImVec2 c(cx, h * 0.5f);
        dl->AddCircleFilled(c, 46.0f, with_alpha(IM_COL32(8, 20, 46, 210), m_dt_hud), 48);
        dl->AddCircle(c, 46.0f, with_alpha(kAccent, 0.8f * m_dt_hud), 48, 2.0f);
        char lbl[16];
        std::snprintf(lbl, sizeof(lbl), "%+ds", static_cast<int>(m_dt_delta));
        const ImVec2 ts = m_font_body->CalcTextSizeA(28.0f, FLT_MAX, 0.0f, lbl);
        dl->AddText(m_font_body, 28.0f, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                    with_alpha(kTextPrimary, m_dt_hud), lbl);
        // Direction chevrons.
        const float dir = left ? -1.0f : 1.0f;
        for (int i = 0; i < 3; ++i) {
            const float ox = c.x + dir * (60.0f + i * 16.0f);
            dl->AddTriangleFilled(ImVec2(ox - dir * 9.0f, c.y - 12.0f),
                                  ImVec2(ox - dir * 9.0f, c.y + 12.0f),
                                  ImVec2(ox + dir * 5.0f, c.y),
                                  with_alpha(kAccent, 0.7f * m_dt_hud));
        }
    }
    // Floating volume HUD (right side).
    if (m_vol_hud > 0.01f) {
        const float bx = w - 120.0f, by0 = h * 0.28f, by1 = h * 0.72f;
        const float bw = 12.0f;
        dl->AddRectFilled(ImVec2(bx, by0), ImVec2(bx + bw, by1),
                          with_alpha(IM_COL32(8, 20, 46, 200), m_vol_hud), 6.0f);
        const float fill = by1 - (by1 - by0) * std::clamp(m_vol_hud, 0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(bx, fill), ImVec2(bx + bw, by1),
                          with_alpha(kAccent, m_vol_hud), 6.0f);
        dl->AddCircleFilled(ImVec2(bx + bw * 0.5f, fill), 10.0f,
                            with_alpha(kTextPrimary, m_vol_hud), 20);
    }
}

// ---------------------------------------------------------------------------
// Glossy player OSD (timeline + circular transport + volume)
// ---------------------------------------------------------------------------
bool WmcUi::draw_osd(ImDrawList* dl, float w, float h, float alpha,
                     bool paused, double elapsed, double total,
                     float volume, bool muted, bool fullscreen,
                     bool settings_open, int chapter, int chapter_count,
                     bool has_menu, bool& gear_clicked, bool& menu_clicked,
                     WmcInput& out) {
    gear_clicked = false;
    menu_clicked = false;
    if (alpha <= 0.001f) return false;

    const float bar_h = 112.0f;
    const ImVec2 b0(w * 0.06f, h - bar_h - 26.0f);
    const ImVec2 b1(w * 0.94f, h - 26.0f);
    dl->AddRectFilled(b0, b1, with_alpha(IM_COL32(8, 20, 46, 238), alpha * 0.95f), 12.0f);
    dl->AddRect(b0, b1, with_alpha(kGlassHi, alpha * 0.35f), 12.0f, 0, 1.5f);

    const bool hover = ImGui::IsMouseHoveringRect(b0, b1);

    // --- Timeline scrub bar ------------------------------------------------
    const float tx0 = b0.x + 26.0f, tx1 = b1.x - 26.0f, ty = b0.y + 30.0f;
    float frac = (total > 0.0) ? static_cast<float>(elapsed / total) : 0.0f;
    frac = std::clamp(frac, 0.0f, 1.0f);
    if (m_seeking) frac = m_seek_preview;

    dl->AddRectFilled(ImVec2(tx0, ty), ImVec2(tx1, ty + 6),
                      with_alpha(kTextDim, alpha * 0.40f), 3.0f);
    const float knob = tx0 + (tx1 - tx0) * frac;
    dl->AddRectFilled(ImVec2(tx0, ty), ImVec2(knob, ty + 6), with_alpha(kAccent, alpha), 3.0f);
    dl->AddCircleFilled(ImVec2(knob, ty + 3), m_seeking ? 10.0f : 8.0f,
                        with_alpha(kTextPrimary, alpha));

    const ImVec2 s_min(tx0, ty - 14.0f), s_max(tx1, ty + 20.0f);
    if (ImGui::IsMouseHoveringRect(s_min, s_max) && ImGui::IsMouseClicked(0)) m_seeking = true;
    if (m_seeking) {
        const float mx = ImGui::GetMousePos().x;
        m_seek_preview = std::clamp((mx - tx0) / (tx1 - tx0), 0.0f, 1.0f);
        if (ImGui::IsMouseReleased(0)) {
            out.action = WmcAction::SeekTo;
            out.seek_seconds = static_cast<double>(m_seek_preview) * total;
            m_seeking = false;
        }
    }

    char te[16], tt[16], line[64];
    format_time(m_seeking ? m_seek_preview * total : elapsed, te, sizeof(te));
    format_time(total, tt, sizeof(tt));
    std::snprintf(line, sizeof(line), "%s / %s", te, tt);
    const float lw = m_font_body->CalcTextSizeA(16.0f, FLT_MAX, 0.0f, line).x;
    dl->AddText(m_font_body, 16.0f, ImVec2(tx1 - lw, ty - 26.0f),
                with_alpha(kTextPrimary, alpha), line);

    // --- Circular transport buttons ---------------------------------------
    const float by = b0.y + 58.0f;
    const float r = 20.0f;

    auto circle_btn = [&](float cx, const char* glyph, bool primary) -> bool {
        const ImVec2 c(cx, by + 20.0f);
        const bool hov = ImGui::IsMouseHoveringRect(ImVec2(c.x - r, c.y - r),
                                                    ImVec2(c.x + r, c.y + r));
        dl->AddCircleFilled(c, r, with_alpha(hov ? kAccent
                             : (primary ? kCobaltGlow : IM_COL32(30, 70, 130, 230)), alpha * 0.96f), 32);
        dl->AddCircle(c, r, with_alpha(kGlassHi, alpha * 0.5f), 32, 1.5f);
        const ImVec2 ts = m_font_body->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, glyph);
        dl->AddText(m_font_body, 15.0f, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                    with_alpha(kTextPrimary, alpha), glyph);
        return hov && ImGui::IsMouseClicked(0);
    };

    float cx = b0.x + 26.0f + r;
    // Play / pause glyph drawn as shapes for a crisper look.
    {
        const ImVec2 c(cx, by + 20.0f);
        const bool hov = ImGui::IsMouseHoveringRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r));
        dl->AddCircleFilled(c, r, with_alpha(hov ? kAccent : kCobaltGlow, alpha * 0.96f), 32);
        if (paused)
            dl->AddTriangleFilled(ImVec2(c.x - 5, c.y - 10), ImVec2(c.x - 5, c.y + 10),
                                  ImVec2(c.x + 11, c.y), with_alpha(kTextPrimary, alpha));
        else {
            dl->AddRectFilled(ImVec2(c.x - 6, c.y - 10), ImVec2(c.x - 1, c.y + 10), with_alpha(kTextPrimary, alpha));
            dl->AddRectFilled(ImVec2(c.x + 3, c.y - 10), ImVec2(c.x + 8, c.y + 10), with_alpha(kTextPrimary, alpha));
        }
        if (hov && ImGui::IsMouseClicked(0)) out.action = WmcAction::TogglePause;
        cx += r * 2.0f + 12.0f;
    }
    if (circle_btn(cx, "Stop", false)) { out.action = WmcAction::Stop; }
    cx += r * 2.0f + 12.0f;
    if (circle_btn(cx, "-10", false)) { out.action = WmcAction::SeekRelative; out.seek_delta = -10.0; }
    cx += r * 2.0f + 12.0f;
    if (circle_btn(cx, "+10", false)) { out.action = WmcAction::SeekRelative; out.seek_delta = 10.0; }
    cx += r * 2.0f + 12.0f;

    // --- Chapter navigation (only when the disc exposes chapters) -----------
    if (chapter_count > 0) {
        // Draw a small "|<<" / ">>|" pair with the current chapter label above.
        const ImVec2 c0(cx, by + 20.0f);
        auto chap_btn = [&](float ccx, const char* glyph, bool enabled) -> bool {
            const ImVec2 c(ccx, by + 20.0f);
            const bool hov = enabled && ImGui::IsMouseHoveringRect(
                ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r));
            dl->AddCircleFilled(c, r, with_alpha(
                !enabled ? IM_COL32(24, 40, 70, 200)
                         : (hov ? kAccent : IM_COL32(30, 70, 130, 230)), alpha * 0.96f), 32);
            dl->AddCircle(c, r, with_alpha(kGlassHi, alpha * 0.5f), 32, 1.5f);
            const ImVec2 ts = m_font_body->CalcTextSizeA(14.0f, FLT_MAX, 0.0f, glyph);
            dl->AddText(m_font_body, 14.0f, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                        with_alpha(enabled ? kTextPrimary : kTextDim, alpha), glyph);
            return hov && ImGui::IsMouseClicked(0);
        };
        if (chap_btn(c0.x, "|<", chapter > 0)) out.action = WmcAction::PrevChapter;
        cx += r * 2.0f + 12.0f;
        if (chap_btn(cx, ">|", chapter + 1 < chapter_count)) out.action = WmcAction::NextChapter;
        cx += r * 2.0f + 12.0f;
        // Current chapter readout above the buttons.
        char cl[48];
        std::snprintf(cl, sizeof(cl), "CH %d/%d", chapter + 1, chapter_count);
        dl->AddText(m_font_body, 13.0f, ImVec2(c0.x - 6.0f, by - 2.0f),
                    with_alpha(kTextDim, alpha), cl);
    }

    // --- Right group: gear, volume, mute, fullscreen ------------------------
    const float r2 = 22.0f;
    float rcx = b1.x - 26.0f - r2;
    auto rcircle = [&](const char* glyph, bool primary) -> bool {
        const ImVec2 c(rcx, by + 20.0f);
        const bool hov = ImGui::IsMouseHoveringRect(ImVec2(c.x - r2, c.y - r2), ImVec2(c.x + r2, c.y + r2));
        dl->AddCircleFilled(c, r2, with_alpha(hov ? kAccent
                             : (primary ? kCobaltGlow : IM_COL32(30, 70, 130, 230)), alpha * 0.96f), 32);
        dl->AddCircle(c, r2, with_alpha(kGlassHi, alpha * 0.5f), 32, 1.5f);
        const ImVec2 ts = m_font_body->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, glyph);
        dl->AddText(m_font_body, 15.0f, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                    with_alpha(kTextPrimary, alpha), glyph);
        rcx -= r2 * 2.0f + 12.0f;
        return hov && ImGui::IsMouseClicked(0);
    };
    // Fullscreen (rightmost), Mute, Gear (opens the Settings overlay).
    if (rcircle(fullscreen ? "Win" : "FS", false)) out.action = WmcAction::ToggleFullscreen;
    if (rcircle(muted ? "Unm" : "Mut", false)) out.action = WmcAction::MuteToggled;
    // Disc Menu entry point (only when the disc ships an .RCO menu).
    if (has_menu && rcircle("Menu", false)) menu_clicked = true;
    if (rcircle("\xE2\x9A\x99", settings_open)) gear_clicked = true;   // gear glyph (U+2699)

    // Volume slider between the transport and the right group.
    const float vw = 130.0f;
    const float v0x = rcx - 20.0f - vw;
    const float vy = by + 20.0f - 3.0f;
    dl->AddText(m_font_body, 14.0f, ImVec2(v0x - 40.0f, vy - 9.0f),
                with_alpha(muted ? kTextDim : kTextPrimary, alpha), "VOL");
    dl->AddRectFilled(ImVec2(v0x, vy), ImVec2(v0x + vw, vy + 6),
                      with_alpha(kTextDim, alpha * 0.40f), 3.0f);
    const float vfill = v0x + vw * std::clamp(volume, 0.0f, 1.0f);
    dl->AddRectFilled(ImVec2(v0x, vy), ImVec2(vfill, vy + 6),
                      with_alpha(muted ? kTextDim : kAccent, alpha), 3.0f);
    dl->AddCircleFilled(ImVec2(vfill, vy + 3), 8.0f, with_alpha(kTextPrimary, alpha));
    if (ImGui::IsMouseHoveringRect(ImVec2(v0x, vy - 12.0f), ImVec2(v0x + vw, vy + 16.0f)) &&
        ImGui::IsMouseDown(0)) {
        out.action = WmcAction::VolumeChanged;
        out.volume = std::clamp((ImGui::GetMousePos().x - v0x) / vw, 0.0f, 1.0f);
    }

    return hover || m_seeking;
}

// ---------------------------------------------------------------------------
// In-player Settings overlay (translucent; playback keeps running)
// ---------------------------------------------------------------------------
void WmcUi::draw_settings_overlay(ImDrawList* dl, float w, float h, float alpha,
                                  int setting_sel, aspect::Mode aspect_mode,
                                  const PlayerMediaInfo& mi, float volume,
                                  bool muted, bool fullscreen, bool ui_sounds,
                                  WmcInput& out) {
    const float pw = 460.0f;
    const ImVec2 p0(w - pw - 36.0f, 90.0f);
    const ImVec2 p1(w - 36.0f, h - 96.0f);
    dl->AddRectFilled(p0, p1, with_alpha(IM_COL32(8, 20, 46, 232), alpha), 14.0f);
    dl->AddRect(p0, p1, with_alpha(kAccent, 0.5f * alpha), 14.0f, 0, 1.8f);

    dl->AddText(m_font_body, 22.0f, ImVec2(p0.x + 22, p0.y + 16),
                with_alpha(kTextPrimary, alpha), "Settings");
    dl->AddLine(ImVec2(p0.x + 22, p0.y + 50), ImVec2(p1.x - 22, p0.y + 50),
                with_alpha(kGlassHi, 0.2f * alpha), 1.0f);

    const float row_h = 46.0f;
    float ry = p0.y + 64.0f;
    auto row = [&](int idx, const char* label, const char* value, bool active) {
        const ImVec2 r0(p0.x + 16, ry), r1(p1.x - 16, ry + row_h - 6.0f);
        if (active) {
            dl->AddRectFilled(r0, r1, with_alpha(kCobaltGlow, 0.34f * alpha), 8.0f);
            dl->AddRect(r0, r1, with_alpha(kAccent, 0.85f * alpha), 8.0f, 0, 1.6f);
        }
        dl->AddText(m_font_body, 18.0f, ImVec2(r0.x + 12, ry + 10),
                    with_alpha(active ? kTextPrimary : with_alpha(kTextPrimary, 0.85f), alpha),
                    label);
        const ImVec2 vs = m_font_body->CalcTextSizeA(17.0f, FLT_MAX, 0.0f, value);
        dl->AddText(m_font_body, 17.0f, ImVec2(r1.x - 12 - vs.x, ry + 11),
                    with_alpha(active ? kAccent : kTextDim, alpha), value);
        (void)idx;
        ry += row_h;
    };

    char v[64];
    row(0, "Deinterlace (yadif)", mi.deinterlace ? "On" : "Off", setting_sel == 0);
    row(1, "Aspect / Zoom", aspect::label(aspect_mode), setting_sel == 1);
    std::snprintf(v, sizeof(v), "%d%%%s", static_cast<int>(volume * 100.0f + 0.5f), muted ? " (muted)" : "");
    row(2, "Audio Volume", v, setting_sel == 2);
    row(3, "Display", fullscreen ? "Fullscreen" : "Windowed", setting_sel == 3);
    row(4, "UI Sounds", ui_sounds ? "On" : "Off", setting_sel == 4);

    // --- Media Info readout ---
    dl->AddLine(ImVec2(p0.x + 22, ry + 4), ImVec2(p1.x - 22, ry + 4),
                with_alpha(kGlassHi, 0.2f * alpha), 1.0f);
    dl->AddText(m_font_body, 15.0f, ImVec2(p0.x + 22, ry + 12),
                with_alpha(kAccent, 0.95f * alpha), "MEDIA INFO");
    ry += 38.0f;

    auto info = [&](const std::string& s) {
        dl->AddText(m_font_body, 14.0f, ImVec2(p0.x + 22, ry),
                    with_alpha(kTextDim, 0.98f * alpha), s.c_str());
        ry += 22.0f;
    };
    info("Title: " + (mi.sfo_title.empty() ? std::string("(untitled)") : mi.sfo_title));
    char vb[128];
    std::snprintf(vb, sizeof(vb), "Video: %s  %dx%d  %.2f fps",
                  mi.video_codec.empty() ? "?" : mi.video_codec.c_str(),
                  mi.video_w, mi.video_h, mi.fps);
    info(vb);
    std::snprintf(vb, sizeof(vb), "PAR: %.3f  %s", mi.par,
                  mi.interlaced ? "(interlaced source)" : "(progressive source)");
    info(vb);
    std::snprintf(vb, sizeof(vb), "Audio: %s  %d Hz  stride %zu B",
                  mi.psmf_audio ? "ATRAC3+ (PSMF)" : "FFmpeg", mi.sample_rate,
                  mi.stride);
    info(vb);
    if (mi.chapter_count > 0) {
        std::snprintf(vb, sizeof(vb), "Chapter: %d / %d  %s",
                      mi.chapter + 1, mi.chapter_count,
                      mi.chapter_label.empty() ? "" : mi.chapter_label.c_str());
        info(vb);
        info("Chapter nav: PgUp/PgDn or [ / ]");
    }

    // Interaction: left/right and Enter change the focused setting.
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) && setting_sel > 0) { --setting_sel; out.cue = umdplay::UiCue::Navigate; }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) && setting_sel < 4) { ++setting_sel; out.cue = umdplay::UiCue::Navigate; }
    const bool left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true);
    const bool right = ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);
    const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, true);
    if (left || right || enter) {
        switch (setting_sel) {
            case 0: out.action = WmcAction::ToggleDeinterlace; break;
            case 1:
                out.action = WmcAction::SetAspect;
                out.aspect_mode = right ? aspect::next(aspect_mode) : aspect::prev(aspect_mode);
                break;
            case 2:
                if (enter) out.action = WmcAction::MuteToggled;
                else { out.action = WmcAction::VolumeChanged;
                       out.volume = std::clamp(volume + (right ? 0.05f : -0.05f), 0.0f, 1.0f); }
                break;
            case 3: out.action = WmcAction::ToggleFullscreen; break;
            case 4: out.action = WmcAction::ToggleUiSounds; break;
        }
        out.cue = enter ? umdplay::UiCue::Select : umdplay::UiCue::Navigate;
    }
}

// ---------------------------------------------------------------------------
// Player: fullscreen video + OSD + Settings overlay + gestures
// ---------------------------------------------------------------------------
WmcInput WmcUi::render_player(SDL_Texture* video_tex, int video_w, int video_h,
                              const DiscItem& disc, const std::string& stream_name,
                              bool paused, double elapsed, double total,
                              bool& osd_visible, bool& settings_open,
                              int& setting_sel, aspect::Mode aspect_mode,
                              const PlayerMediaInfo& mi,
                              float volume, bool muted, bool fullscreen,
                              bool has_menu, bool ui_sounds, bool& menu_clicked,
                              float t) {
    menu_clicked = false;
    WmcInput out;
    if (!m_imgui_ready) return out;

    ImGuiIO& io = ImGui::GetIO();
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("##wmc_player", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(0, 0, 0, 255));

    // Letterboxed/zoomed video per the current Aspect/Zoom mode.
    if (video_tex && video_w > 0 && video_h > 0) {
        const aspect::Rect r = aspect::fit(aspect_mode, static_cast<float>(video_w),
                                           static_cast<float>(video_h), mi.par, w, h);
        dl->PushClipRect(ImVec2(0, 0), ImVec2(w, h), true);
        dl->AddImage(reinterpret_cast<ImTextureID>(video_tex),
                     ImVec2(r.x, r.y), ImVec2(r.x + r.w, r.y + r.h));
        dl->PopClipRect();
    }

    // Drain gestures (tap / double-tap / swipe).
    std::vector<gesture::Event> ges;
    collect_gestures(true, ges);

    const bool moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
    const bool any_key = ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ||
                         ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
                         ImGui::IsKeyPressed(ImGuiKey_Space) ||
                         ImGui::IsKeyPressed(ImGuiKey_UpArrow) ||
                         ImGui::IsKeyPressed(ImGuiKey_DownArrow) ||
                         ImGui::IsKeyPressed(ImGuiKey_M) ||
                         ImGui::IsKeyPressed(ImGuiKey_F11);
    if (moved || any_key) { osd_visible = true; m_last_input_time = t; }
    if (paused || settings_open) osd_visible = true;

    m_osd_alpha = lerp(m_osd_alpha, osd_visible ? 1.0f : 0.0f,
                       std::clamp(io.DeltaTime * 6.0f, 0.0f, 1.0f));

    // --- Keyboard ---
    if (ImGui::IsKeyPressed(ImGuiKey_Space, true)) out.action = WmcAction::TogglePause;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) && !settings_open) {
        out.action = WmcAction::SeekRelative;
        out.seek_delta = ImGui::GetIO().KeyShift ? -30.0 : -10.0;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) && !settings_open) {
        out.action = WmcAction::SeekRelative;
        out.seek_delta = ImGui::GetIO().KeyShift ? 30.0 : 10.0;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) && !settings_open) {
        out.action = WmcAction::VolumeChanged;
        out.volume = std::clamp(volume + 0.05f, 0.0f, 1.0f);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) && !settings_open) {
        out.action = WmcAction::VolumeChanged;
        out.volume = std::clamp(volume - 0.05f, 0.0f, 1.0f);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_M, true)) out.action = WmcAction::MuteToggled;
    if (ImGui::IsKeyPressed(ImGuiKey_F11, true)) out.action = WmcAction::ToggleFullscreen;
    // Chapter navigation: PageUp/PageDown or [ / ] jump between markers.
    if (mi.chapter_count > 0 && !settings_open) {
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true) ||
            ImGui::IsKeyPressed(ImGuiKey_LeftBracket, true)) {
            out.action = WmcAction::PrevChapter;
            osd_visible = true; m_last_input_time = t;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true) ||
            ImGui::IsKeyPressed(ImGuiKey_RightBracket, true)) {
            out.action = WmcAction::NextChapter;
            osd_visible = true; m_last_input_time = t;
        }
    }
    // O / Tab toggle the Settings overlay without stopping playback.
    if (ImGui::IsKeyPressed(ImGuiKey_O, true) || ImGui::IsKeyPressed(ImGuiKey_Tab, true)) {
        settings_open = !settings_open;
        setting_sel = 0;
        osd_visible = true;
        m_last_input_time = t;
        out.cue = umdplay::UiCue::Navigate;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, true)) {
        if (settings_open) settings_open = false;
        else out.action = WmcAction::Stop;
        out.cue = umdplay::UiCue::Back;
    }

    // --- Gestures ---
    for (const auto& g : ges) {
        if (g.type == gesture::Type::Swipe && g.axis == gesture::Axis::Horizontal) {
            // Horizontal scrub: preview a seek and commit on release.
            const double span = std::clamp(static_cast<double>(g.dx) / w * 4.0 * 60.0, -600.0, 600.0);
            m_scrub_target = std::clamp(elapsed + span, 0.0, total > 0 ? total : elapsed + span);
            m_scrub_hud = 1.0f;
            out.action = WmcAction::SeekTo;
            out.seek_seconds = m_scrub_target;
            osd_visible = true; m_last_input_time = t;
        } else if (g.type == gesture::Type::Swipe && g.axis == gesture::Axis::Vertical) {
            // Vertical swipe on the right half adjusts volume.
            if (g.x > w * 0.5f) {
                const float dv = -g.dy / (h * 0.6f);
                out.action = WmcAction::VolumeChanged;
                out.volume = std::clamp(volume + dv, 0.0f, 1.0f);
                m_vol_hud = out.volume;
            }
        } else if (g.type == gesture::Type::DoubleTap) {
            out.action = WmcAction::SeekRelative;
            out.seek_delta = (g.x < w * 0.5f) ? -10.0 : 10.0;
            m_dt_delta = static_cast<float>(out.seek_delta);
            m_dt_hud = 1.0f;
        } else if (g.type == gesture::Type::Tap) {
            osd_visible = !osd_visible;
            m_last_input_time = t;
        }
    }

    // --- Title bar ---
    if (m_osd_alpha > 0.02f) {
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, 52),
                          IM_COL32(4, 12, 30, static_cast<int>(150 * m_osd_alpha)));
        std::string title = disc.title + "   " + kBullet + "   " + stream_name;
        if (mi.chapter_count > 0 && mi.chapter >= 0) {
            char cb[64];
            std::snprintf(cb, sizeof(cb), "   %s   CH %d/%d",
                          kBullet, mi.chapter + 1, mi.chapter_count);
            title += cb;
            if (!mi.chapter_label.empty()) title += "  " + mi.chapter_label;
        }
        dl->AddText(m_font_body, 18.0f, ImVec2(28, 15),
                    with_alpha(kTextPrimary, 0.92f * m_osd_alpha), title.c_str());
    }

    bool gear_clicked = false;
    const bool osd_hover = draw_osd(dl, w, h, m_osd_alpha, paused, elapsed, total,
                                    volume, muted, fullscreen, settings_open,
                                    mi.chapter, mi.chapter_count, has_menu,
                                    gear_clicked, menu_clicked, out);
    if (gear_clicked) { settings_open = !settings_open; setting_sel = 0; osd_visible = true; m_last_input_time = t; out.cue = umdplay::UiCue::Navigate; }
    if (menu_clicked) { out.action = WmcAction::OpenDiscMenu; osd_visible = false; out.cue = umdplay::UiCue::Select; }

    // --- Settings overlay (keeps playback running) ---
    if (settings_open) draw_settings_overlay(dl, w, h, 1.0f, setting_sel, aspect_mode,
                                             mi, volume, muted, fullscreen, ui_sounds, out);

    // --- Gesture HUDs, fading out ---
    m_scrub_hud = std::max(0.0f, m_scrub_hud - io.DeltaTime * 1.6f);
    m_dt_hud = std::max(0.0f, m_dt_hud - io.DeltaTime * 1.2f);
    m_vol_hud = std::max(0.0f, m_vol_hud - io.DeltaTime * 1.2f);
    draw_player_huds(dl, w, h, m_osd_alpha);

    if (osd_visible && !paused && !osd_hover && !settings_open &&
        (t - m_last_input_time) > 3.0) {
        osd_visible = false;
    }

    ImGui::End();
    return out;
}

// ---------------------------------------------------------------------------
// Interactive Disc Menu (.RCO pages)
// ---------------------------------------------------------------------------
// Renders the active RCO page as a 16:9 letterboxed panel centred in the
// window: planes/backgrounds first, then buttons (with the "_sel" art and a
// glowing focus ring on the focused one). Input: arrow keys / D-pad move the
// focus, Enter / click / tap activate, Esc / Backspace go back, plus mouse
// hover and touch taps. Returns the resulting MenuAction (if any).
DiscMenuResult WmcUi::render_disc_menu(umdplay::RcoMenu& menu,
                                       const umdplay::RcoTextureCache& tex,
                                       const DiscItem& disc, float t) {
    DiscMenuResult res;
    if (!m_imgui_ready) return res;

    ImGuiIO& io = ImGui::GetIO();
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("##wmc_discmenu", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Deep backdrop + soft vignette.
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(2, 6, 16, 255));
    draw_aurora(dl, w, h, t, nullptr);

    // Letterbox the 854x480 design space into the window (16:9 preserved).
    const float design_ar = umdplay::kMenuDesignW / umdplay::kMenuDesignH;
    float panel_w = w * 0.92f;
    float panel_h = panel_w / design_ar;
    if (panel_h > h * 0.86f) { panel_h = h * 0.86f; panel_w = panel_h * design_ar; }
    const float px = (w - panel_w) * 0.5f;
    const float py = (h - panel_h) * 0.5f + 6.0f;

    dl->AddRectFilled(ImVec2(px, py), ImVec2(px + panel_w, py + panel_h),
                      IM_COL32(0, 0, 0, 235), 10.0f);
    dl->AddRect(ImVec2(px, py), ImVec2(px + panel_w, py + panel_h),
                with_alpha(kGlassHi, 0.22f), 10.0f, 0, 1.4f);

    auto to_px = [&](const umdplay::RcoMenuItem& it) {
        return ImVec4(px + it.rect_x * panel_w, py + it.rect_y * panel_h,
                      px + (it.rect_x + it.rect_w) * panel_w,
                      py + (it.rect_y + it.rect_h) * panel_h);
    };

    dl->PushClipRect(ImVec2(px, py), ImVec2(px + panel_w, py + panel_h), true);

    const umdplay::RcoMenuItem* hovered = nullptr;
    // Hover/pointer focus (mouse or touch). Only re-focus when the pointer
    // actually moves, so keyboard/D-pad navigation is not clobbered by a
    // stationary cursor sitting over the panel.
    const ImVec2 mp = ImGui::GetMousePos();
    const ImVec2 md = io.MouseDelta;
    const bool inside = mp.x >= px && mp.x <= px + panel_w &&
                        mp.y >= py && mp.y <= py + panel_h;
    if (inside && (md.x != 0.0f || md.y != 0.0f))
        menu.focus_at((mp.x - px) / panel_w, (mp.y - py) / panel_h);
    if (inside) {
        const auto& items = menu.page().items;
        const umdplay::RcoMenuItem* foc = menu.focused_item();
        for (const auto& it : items)
            if (it.is_button && foc && it.obj_offset == foc->obj_offset) hovered = &it;
    }

    // Draw planes/backgrounds, then buttons.
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& it : menu.page().items) {
            const bool is_btn = it.is_button;
            if ((pass == 0) == is_btn) continue;
            const ImVec4 r = to_px(it);
            if (it.image_offset != 0xFFFFFFFF) {
                SDL_Texture* tx = tex.get(it.image_offset);
                SDL_Texture* sel = (it.focused && it.image_sel_offset != 0xFFFFFFFF)
                                       ? tex.get(it.image_sel_offset) : nullptr;
                if (sel) tx = sel;
                if (tx) {
                    dl->AddImage(reinterpret_cast<ImTextureID>(tx),
                                 ImVec2(r.x, r.y), ImVec2(r.z, r.w),
                                 ImVec2(0, 0), ImVec2(1, 1),
                                 IM_COL32(static_cast<int>(it.red * 255),
                                          static_cast<int>(it.green * 255),
                                          static_cast<int>(it.blue * 255),
                                          static_cast<int>(it.alpha * 255)));
                }
            }
        }
    }

    // Focus ring: a glowing rounded rect around the focused button.
    const umdplay::RcoMenuItem* focus = menu.focused_item();
    if (focus) {
        const ImVec4 r = to_px(*focus);
        const float pad = 3.0f;
        const float pulse = 0.55f + 0.45f * (0.5f + 0.5f * std::sin(t * 6.0f));
        dl->AddRect(ImVec2(r.x - pad, r.y - pad), ImVec2(r.z + pad, r.w + pad),
                    with_alpha(kAccent, 0.55f + 0.35f * pulse), 8.0f, 0, 3.0f);
        dl->AddRect(ImVec2(r.x - pad - 2, r.y - pad - 2),
                    ImVec2(r.z + pad + 2, r.w + pad + 2),
                    with_alpha(kCobaltGlow, 0.35f * pulse), 10.0f, 0, 6.0f);
    }
    dl->PopClipRect();

    // Header: disc title + page name + page counter.
    {
        const std::string title = disc.title.empty() ? std::string("Disc Menu") : disc.title;
        dl->AddText(m_font_title, 20.0f, ImVec2(px, py - 30.0f),
                    with_alpha(kTextPrimary, 0.95f), title.c_str());
        char pg[64];
        std::snprintf(pg, sizeof(pg), "%s   %d/%d", menu.page_name().c_str(),
                      menu.index_1based(), menu.page_count());
        const float pw = ImGui::CalcTextSize(pg).x;
        dl->AddText(m_font_body, 15.0f, ImVec2(px + panel_w - pw, py - 26.0f),
                    with_alpha(kAccent, 0.9f), pg);
    }

    // --- Input -------------------------------------------------------------
    auto take_key = [&](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    if (take_key(ImGuiKey_LeftArrow))  { menu.move_focus(-1, 0); res.cue = umdplay::UiCue::Navigate; }
    if (take_key(ImGuiKey_RightArrow)) { menu.move_focus(1, 0);  res.cue = umdplay::UiCue::Navigate; }
    if (take_key(ImGuiKey_UpArrow))    { menu.move_focus(0, -1); res.cue = umdplay::UiCue::Navigate; }
    if (take_key(ImGuiKey_DownArrow))  { menu.move_focus(0, 1);  res.cue = umdplay::UiCue::Navigate; }

    bool activated = false;
    if (take_key(ImGuiKey_Enter) || take_key(ImGuiKey_Space) ||
        take_key(ImGuiKey_KeypadEnter)) {
        res.action = menu.activate_focused();
        activated = true;
        res.cue = umdplay::UiCue::Select;
    }
    // Mouse click / touch tap on a button.
    if (ImGui::IsMouseClicked(0) && hovered) {
        res.action = menu.activate_at((mp.x - px) / panel_w, (mp.y - py) / panel_h);
        activated = true;
        res.cue = umdplay::UiCue::Select;
    }
    if (take_key(ImGuiKey_Escape) || take_key(ImGuiKey_Backspace)) {
        umdplay::MenuAction back = menu.back();
        if (back.kind == umdplay::MenuActionKind::ExitMenu) res.close = true;
        else res.action = back;
        res.cue = umdplay::UiCue::Back;
    }

    // After a press, let the disc's own onMenu state machine act (it may ask
    // for playback even when the button itself only set a flag).
    if (activated && res.action.kind != umdplay::MenuActionKind::ExitMenu) {
        auto step = menu.step_script();
        if (step.play_requested && res.action.kind == umdplay::MenuActionKind::None) {
            res.action.kind = umdplay::MenuActionKind::PlayMovie;
            res.action.track = step.play_track;
            res.action.chapter = step.play_chapter;
        }
    }
    // Footer hint bar.
    std::string hint = "ARROWS / D-PAD  move     ENTER / A  select     "
                       "MOUSE / TOUCH  tap     ESC / B  back";
    if (hovered) hint = std::string("SELECT  ") + hovered->name + "     " + hint;
    draw_footer(dl, w, h, hint);

    ImGui::End();
    return res;
}
