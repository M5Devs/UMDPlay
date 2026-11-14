#include "wmc_ui.h"

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sys/stat.h>

namespace {

// --- WMC palette: deep oceanic / cobalt blue -------------------------------
constexpr ImU32 kCobaltMid   = IM_COL32( 14,  60, 128, 255);
constexpr ImU32 kCobaltGlow  = IM_COL32( 46, 132, 214, 255);
constexpr ImU32 kCobaltDeep  = IM_COL32(  4,  10,  28, 255);
constexpr ImU32 kGlassHi     = IM_COL32(190, 225, 255, 255);
constexpr ImU32 kAccent      = IM_COL32(120, 200, 255, 255);
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
};

// ImGui's AddFontFromFileTTF calls ErrorLog() on a missing file, which asserts
// via BeginErrorTooltip() outside a frame and crashes. Stat the file first.
bool file_exists(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0;
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
    std::snprintf(buf, n, "%d:%02d", total / 60, total % 60);
}

std::string human_size(uint32_t bytes) {
    char buf[32];
    const double mb = bytes / (1024.0 * 1024.0);
    if (mb >= 1000.0) std::snprintf(buf, sizeof(buf), "%.1f GB", mb / 1024.0);
    else              std::snprintf(buf, sizeof(buf), "%.0f MB", mb);
    return buf;
}

// Footer reserve: nothing else may draw below this y (keeps the key-hint bar
// clear of cards and text at every window size).
float footer_top(float h) { return h - 56.0f; }

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
        // Always include the bullet (U+2022) and en/em dashes used in labels.
        b.AddText("\xE2\x80\xA2\xE2\x80\x93\xE2\x80\x94");
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
    m_font_title = m_font_huge = m_font_body = nullptr;
}

void WmcUi::process_event(const SDL_Event& e) {
    if (m_imgui_ready) ImGui_ImplSDL2_ProcessEvent(&e);
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

// Animated oceanic gradient + floating light sheen. A focused PIC1 backdrop is
// blended in, dimmed, behind the gradient for atmosphere.
void WmcUi::draw_background(ImDrawList* dl, float w, float h, float t,
                            SDL_Texture* pic1) {
    const ImVec2 a(0, 0), b(w, h);

    const float breathe = 0.5f + 0.5f * std::sin(t * 0.25f);
    const ImU32 top = ImGui::GetColorU32(ImVec4(
        lerp(0.02f, 0.06f, breathe), lerp(0.07f, 0.13f, breathe),
        lerp(0.22f, 0.32f, breathe), 1.0f));
    dl->AddRectFilledMultiColor(a, b, top, top, kCobaltMid, kCobaltDeep);

    if (pic1) {
        const float drift = 14.0f * std::sin(t * 0.12f);
        dl->AddImage(reinterpret_cast<ImTextureID>(pic1),
                     ImVec2(-40 + drift, -30 + drift * 0.5f),
                     ImVec2(w + 40 + drift, h + 30 + drift * 0.5f),
                     ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, 60));
        dl->AddRectFilled(a, b, IM_COL32(4, 10, 28, 150));
    }

    // Diagonal glass sheen sweep.
    const float sweep = std::fmod(t * 0.06f, 1.4f) - 0.2f;
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

    dl->AddRectFilledMultiColor(a, b, IM_COL32(0, 0, 0, 100), IM_COL32(0, 0, 0, 100),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
}

void WmcUi::draw_card(ImDrawList* dl, const DiscItem& disc, float cx, float cy,
                      float w, float h, float focus, bool selected, float t) {
    const float scale = lerp(0.86f, 1.0f, focus);
    const float cw = w * scale, ch = h * scale;
    const ImVec2 p0(cx - cw * 0.5f, cy - ch * 0.5f);
    const ImVec2 p1(cx + cw * 0.5f, cy + ch * 0.5f);

    dl->AddRectFilled(ImVec2(p0.x + 6, p0.y + 10), ImVec2(p1.x + 6, p1.y + 10),
                      IM_COL32(0, 0, 0, static_cast<int>(100 * focus)), 14.0f);

    const ImU32 top = with_alpha(IM_COL32(40, 96, 166, 255), lerp(0.5f, 1.0f, focus));
    const ImU32 bot = with_alpha(IM_COL32(10, 26, 60, 255),  lerp(0.5f, 1.0f, focus));
    dl->AddRectFilledMultiColor(p0, p1, top, top, bot, bot);

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

    // Glass border + highlight glow on the active card.
    dl->AddRect(p0, p1, with_alpha(kGlassHi, lerp(0.22f, 0.9f, focus)), 14.0f, 0,
                selected ? lerp(1.6f, 3.2f, focus) : 1.0f);
    if (selected) {
        dl->AddRect(ImVec2(p0.x - 5, p0.y - 5), ImVec2(p1.x + 5, p1.y + 5),
                    with_alpha(kAccent, 0.35f * focus + 0.12f * std::sin(t * 3.0f)),
                    17.0f, 0, 2.0f);
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

void WmcUi::draw_strip_tabs(ImDrawList* dl, float w, float y, HubMode strip, float t) {
    (void)t;
    struct Tab { const char* label; HubMode mode; };
    const Tab tabs[] = {
        {"Recent Discs",   HubMode::Recent},
        {"Browse System",  HubMode::Browse},
        {"Options",        HubMode::Settings},
    };
    const int n = static_cast<int>(sizeof(tabs) / sizeof(tabs[0]));
    const float tab_w = std::min(w * 0.15f, 188.0f);
    const float gap = 10.0f;
    const float total = n * tab_w + (n - 1) * gap;
    float x = w - 64.0f - total;   // right-aligned, clear of the top-left logo
    for (int i = 0; i < n; ++i) {
        const ImVec2 r0(x, y), r1(x + tab_w, y + 40.0f);
        const bool active = (tabs[i].mode == strip);
        if (active) {
            dl->AddRectFilled(r0, r1, with_alpha(kCobaltGlow, 0.34f), 9.0f);
            dl->AddRect(r0, r1, with_alpha(kAccent, 0.85f), 9.0f, 0, 1.8f);
        } else {
            dl->AddRectFilled(r0, r1, IM_COL32(12, 30, 62, 150), 9.0f);
            dl->AddRect(r0, r1, with_alpha(kGlassHi, 0.18f), 9.0f);
        }
        const ImVec2 ts = ImGui::CalcTextSize(tabs[i].label);
        const float sc = 17.0f / ImGui::GetFontSize();
        dl->AddText(m_font_body, 17.0f,
                    ImVec2(r0.x + (tab_w - ts.x * sc) * 0.5f, r0.y + 10.0f),
                    active ? kTextPrimary : kTextDim, tabs[i].label);
        x += tab_w + gap;
    }
}

void WmcUi::draw_action_panel(ImDrawList* dl, float x, float y, float w,
                              const DiscItem& disc, int action_sel, bool extras_open,
                              int extras_sel, int bonus_count) {
    const float row_h = 44.0f;
    const float gap = 10.0f;

    // --- Extras sub-page: replace the action rows so the panel stays bounded
    // and can never push into the footer key-hint bar. ---
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

void WmcUi::draw_browse_panel(ImDrawList* dl, float x, float y, float w,
                              const std::string& path,
                              const std::vector<BrowseEntry>& entries,
                              int browse_sel, float scroll) {
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
    (void)scroll;
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
                                bool muted) {
    const float row_h = 52.0f;
    const float gap = 10.0f;
    const char* labels[3] = {"Display Mode", "Volume", "Mute"};
    char values[3][48];
    std::snprintf(values[0], sizeof(values[0]), "%s", fullscreen ? "Fullscreen" : "Windowed");
    std::snprintf(values[1], sizeof(values[1]), "%d%%", static_cast<int>(volume * 100.0f + 0.5f));
    std::snprintf(values[2], sizeof(values[2]), "%s", muted ? "On" : "Off");

    float ry = y;
    for (int i = 0; i < 3; ++i) {
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
            // Volume bar.
            const float bx0 = r0.x + w * 0.46f, bx1 = r1.x - 110.0f, by = ry + 23.0f;
            dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx1, by + 6),
                              with_alpha(kTextDim, 0.35f), 3.0f);
            dl->AddRectFilled(ImVec2(bx0, by), ImVec2(bx0 + (bx1 - bx0) * std::clamp(volume, 0.f, 1.f), by + 6),
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

void WmcUi::draw_footer(ImDrawList* dl, float w, float h, const std::string& hint) {
    const float ft = footer_top(h);
    dl->AddRectFilled(ImVec2(0, ft), ImVec2(w, h), IM_COL32(3, 8, 20, 200));
    dl->AddLine(ImVec2(0, ft), ImVec2(w, ft), with_alpha(kGlassHi, 0.18f), 1.0f);
    dl->AddText(m_font_body, 15.0f, ImVec2(48, ft + 18), with_alpha(kTextDim, 0.95f),
                hint.c_str());
}

WmcInput WmcUi::render_hub(HubMode& strip,
                           const std::vector<DiscItem>& recent, int& recent_sel,
                           const std::vector<BrowseEntry>& browse, int& browse_sel,
                           const std::string& browse_path,
                           int& action_sel, bool& extras_open, int& extras_sel,
                           int& setting_sel,
                           bool fullscreen, float volume, bool muted, float t) {
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

    // Focus model: 0 = strip tabs, 1 = card row, 2 = panel rows.
    // Changing strips resets focus to the card row.
    if (strip != m_last_strip) { m_focus = 1; extras_open = false; m_last_strip = strip; }
    m_focus = std::clamp(m_focus, 0, 2);

    const DiscItem* sel = nullptr;
    if (strip == HubMode::Recent && !recent.empty()) {
        recent_sel = std::clamp(recent_sel, 0, static_cast<int>(recent.size()) - 1);
        sel = &recent[recent_sel];
    }
    draw_background(dl, w, h, t, sel ? sel->pic1 : nullptr);

    // Read each key at most once per frame so a focus change cannot also be
    // consumed by the block it just moved into.
    bool used_l = false, used_r = false, used_u = false, used_d = false, used_e = false;
    auto take_l = [&] { if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) && !used_l) { used_l = true; return true; } return false; };
    auto take_r = [&] { if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) && !used_r) { used_r = true; return true; } return false; };
    auto take_u = [&] { if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) && !used_u) { used_u = true; return true; } return false; };
    auto take_d = [&] { if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) && !used_d) { used_d = true; return true; } return false; };
    auto take_e = [&] { if ((ImGui::IsKeyPressed(ImGuiKey_Enter, true) ||
                             ImGui::IsKeyPressed(ImGuiKey_Space, true)) && !used_e) { used_e = true; return true; } return false; };

    // --- focus 0: strip tabs ---
    if (m_focus == 0) {
        if (take_l() && strip != HubMode::Recent) {
            strip = (strip == HubMode::Settings) ? HubMode::Browse : HubMode::Recent;
        }
        if (take_r() && strip != HubMode::Settings) {
            strip = (strip == HubMode::Recent) ? HubMode::Browse : HubMode::Settings;
        }
        if (take_d()) m_focus = 1;
    }

    draw_strip_tabs(dl, w, 22.0f, strip, t);

    // --- Header ---
    dl->AddText(m_font_huge, 36.0f, ImVec2(64, 28), kTextPrimary, "UMDPlay");
    dl->AddText(m_font_body, 15.0f, ImVec2(66, 72), kTextDim,
                (std::string("UMD VIDEO  ") + kBullet + "  PRESERVATION LIBRARY").c_str());
    dl->AddLine(ImVec2(64, 98), ImVec2(w - 64, 98), with_alpha(kGlassHi, 0.22f), 1.5f);

    // --- Carousel row ---
    const float card_w = std::min(w * 0.19f, 232.0f);
    const float card_h = card_w * 1.18f;
    const float cy = 100.0f + card_h * 0.5f + 22.0f;
    const float spacing = card_w * 1.14f;
    const float cx_center = w * 0.5f;
    const bool on_cards = (m_focus == 1);

    if (strip == HubMode::Recent) {
        if (recent.empty()) {
            dl->AddText(m_font_body, 17.0f, ImVec2(cx_center - 190, cy - 8),
                        with_alpha(kTextDim, 0.85f),
                        "No recent discs yet - open one from Browse System.");
        }
        m_scroll = lerp(m_scroll, static_cast<float>(recent_sel),
                        std::clamp(io.DeltaTime * 8.0f, 0.0f, 1.0f));
        for (int i = 0; i < static_cast<int>(recent.size()); ++i) {
            const float cx = cx_center + (static_cast<float>(i) - m_scroll) * spacing;
            if (cx < -card_w || cx > w + card_w) continue;
            const float dist = std::fabs(static_cast<float>(i) - m_scroll);
            const float focus = std::clamp(1.0f - dist * 0.5f, 0.0f, 1.0f);
            draw_card(dl, recent[i], cx, cy, card_w, card_h, focus,
                      (i == recent_sel) && on_cards, t);
        }
        if (on_cards) {
            if (take_l() && recent_sel > 0) recent_sel--;
            if (take_r() && recent_sel + 1 < static_cast<int>(recent.size())) recent_sel++;
            if (take_e() && sel) { out.action = WmcAction::OpenIso; out.iso_path = sel->iso_path; }
            if (take_u()) m_focus = 0;
            if (take_d()) m_focus = 2;
        }
    } else if (strip == HubMode::Browse) {
        m_scroll = lerp(m_scroll, static_cast<float>(browse_sel),
                        std::clamp(io.DeltaTime * 8.0f, 0.0f, 1.0f));
        for (int i = 0; i < static_cast<int>(browse.size()); ++i) {
            const float cx = cx_center + (static_cast<float>(i) - m_scroll) * spacing;
            if (cx < -card_w || cx > w + card_w) continue;
            const float dist = std::fabs(static_cast<float>(i) - m_scroll);
            const float focus = std::clamp(1.0f - dist * 0.5f, 0.0f, 1.0f);
            const BrowseEntry& e = browse[i];
            DiscItem tmp;
            tmp.title = e.name;
            tmp.disc_id = e.is_dir ? "Folder" : (e.is_iso ? "UMD ISO" : "File");
            draw_card(dl, tmp, cx, cy, card_w, card_h, focus,
                      (i == browse_sel) && on_cards, t);
        }
        if (on_cards) {
            if (take_l() && browse_sel > 0) browse_sel--;
            if (take_r() && browse_sel + 1 < static_cast<int>(browse.size())) browse_sel++;
            if (take_e() && browse_sel >= 0 && browse_sel < static_cast<int>(browse.size())) {
                const BrowseEntry& e = browse[browse_sel];
                if (e.is_dir) { out.action = WmcAction::BrowseEnter; out.iso_path = e.path; }
                else if (e.is_iso) { out.action = WmcAction::OpenIso; out.iso_path = e.path; }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Backspace, true)) out.action = WmcAction::BrowseUp;
            if (take_u()) m_focus = 0;
            if (take_d()) m_focus = 2;
        }
    }

    // --- Lower panel ---
    const float panel_top = footer_top(h) - 8.0f;
    const float panel_y = cy + card_h * 0.5f + 16.0f;
    const float panel_w = std::min(w * 0.62f, 760.0f);
    const float panel_x = (w - panel_w) * 0.5f;
    const float avail = std::max(0.0f, panel_top - panel_y);

    // Hard clip: the lower panel can never draw over the footer key-hint bar,
    // no matter the window size or how many rows a disc has.
    dl->PushClipRect(ImVec2(0, panel_y), ImVec2(w, panel_top), true);

    if (strip == HubMode::Recent) {
        const char* title = sel ? (sel->title.empty() ? "(untitled disc)" : sel->title.c_str())
                                : "(no disc selected)";
        const float title_px = 23.0f;
        const float tw = ImGui::CalcTextSize(title).x * (title_px / ImGui::GetFontSize());
        dl->AddText(m_font_title, title_px, ImVec2((w - tw) * 0.5f, panel_y),
                    kTextPrimary, title);
        if (sel && !sel->disc_id.empty()) {
            const float mw = ImGui::CalcTextSize(sel->disc_id.c_str()).x * (14.0f / ImGui::GetFontSize());
            dl->AddText(m_font_body, 14.0f, ImVec2((w - mw) * 0.5f, panel_y + 28),
                        kTextDim, sel->disc_id.c_str());
        }
        int bonus = 0;
        if (sel) for (const auto& s : sel->streams) if (!s.is_main) ++bonus;
        const float ap_y = panel_y + 54.0f;
        const float ap_avail = std::max(0.0f, panel_top - ap_y);
        if (ap_avail > 56.0f && sel) {
            const int nrows = 2 + (bonus > 0 ? 1 : 0);
            action_sel = std::clamp(action_sel, 0, nrows - 1);
            draw_action_panel(dl, panel_x, ap_y, panel_w, *sel, action_sel,
                              extras_open, extras_sel, bonus);
            if (m_focus == 2) {
                if (extras_open) {
                    if (take_u() && extras_sel > 0) extras_sel--;
                    if (take_d() && extras_sel + 1 < bonus) extras_sel++;
                    if (take_l() || take_r()) extras_open = false;
                    if (take_e()) {
                        int b = 0;
                        for (int i = 0; i < static_cast<int>(sel->streams.size()); ++i) {
                            if (sel->streams[i].is_main) continue;
                            if (b == extras_sel) { out.action = WmcAction::PlayStream; out.stream_index = i; break; }
                            ++b;
                        }
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_Escape, true)) extras_open = false;
                } else {
                    const int options_idx = nrows - 1;
                    if (take_u()) {
                        if (action_sel > 0) action_sel--;
                        else m_focus = 1;
                    }
                    if (take_d() && action_sel + 1 < nrows) action_sel++;
                    if (take_e()) {
                        if (action_sel == 0) out.action = WmcAction::Play;
                        else if (action_sel == options_idx) strip = HubMode::Settings;
                        else if (bonus > 0) { extras_open = true; extras_sel = 0; }
                    }
                }
            }
        } else if (!sel) {
            dl->AddText(m_font_body, 16.0f, ImVec2(panel_x, ap_y),
                        with_alpha(kTextDim, 0.8f),
                        "Open an ISO from Browse System to populate your library.");
        }
    } else if (strip == HubMode::Browse) {
        const float bh = std::min(avail, 8 * 34.0f + 16.0f);
        if (bh > 80.0f) {
            draw_browse_panel(dl, panel_x, panel_y, panel_w, browse_path, browse,
                              browse_sel, m_browse_scroll);
            if (m_focus == 2 && take_u()) m_focus = 1;
        }
    } else {  // Settings
        const float sh = std::min(avail, 3 * 62.0f + 10.0f);
        if (sh > 80.0f) {
            if (m_focus == 2) {
                if (take_u()) { if (setting_sel > 0) setting_sel--; else m_focus = 1; }
                if (take_d() && setting_sel < 2) setting_sel++;
                const bool sl = take_l();
                const bool sr = take_r();
                if (sl || sr) {
                    if (setting_sel == 0) {
                        out.action = WmcAction::ToggleFullscreen;
                    } else if (setting_sel == 1) {
                        out.action = WmcAction::VolumeChanged;
                        out.volume = std::clamp(volume + (sr ? 0.05f : -0.05f), 0.0f, 1.0f);
                    } else {
                        out.action = WmcAction::MuteToggled;
                    }
                }
                if (take_e()) {
                    if (setting_sel == 0) out.action = WmcAction::ToggleFullscreen;
                    else if (setting_sel == 2) out.action = WmcAction::MuteToggled;
                }
            }
            draw_settings_panel(dl, panel_x, panel_y, panel_w, setting_sel,
                                fullscreen, volume, muted);
        }
    }

    dl->PopClipRect();

    // --- Footer hints (drawn last; nothing may overlap it) ---
    std::string hint;
    if (m_focus == 0)
        hint = std::string("LEFT / RIGHT  switch strip     DOWN  enter strip     ")
             + "F11  fullscreen     ESC / Q  quit";
    else if (m_focus == 1)
        hint = std::string("LEFT / RIGHT  move     UP  strips     DOWN  actions     ")
             + "ENTER  open     F11  fullscreen     ESC / Q  quit";
    else
        hint = std::string("UP / DOWN  select     ENTER / LEFT / RIGHT  activate     ")
             + "F11  fullscreen     ESC / Q  quit";
    draw_footer(dl, w, h, hint);

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, true)) {
        if (extras_open) extras_open = false;
        else if (m_focus > 0) m_focus = 0;
        else if (strip != HubMode::Recent) strip = HubMode::Recent;
        else out.action = WmcAction::Quit;
    }

    ImGui::End();
    return out;
}

void WmcUi::draw_osd(ImDrawList* dl, float w, float h, float alpha,
                     bool paused, double elapsed, double total,
                     float volume, bool muted, bool fullscreen, WmcInput& out) {
    if (alpha <= 0.001f) return;

    const float bar_h = 92.0f;
    const ImVec2 b0(w * 0.06f, h - bar_h - 26.0f);
    const ImVec2 b1(w * 0.94f, h - 26.0f);
    dl->AddRectFilled(b0, b1, with_alpha(IM_COL32(8, 20, 46, 235), alpha * 0.94f), 12.0f);
    dl->AddRect(b0, b1, with_alpha(kGlassHi, alpha * 0.35f), 12.0f, 0, 1.5f);

    // Play / pause button.
    const ImVec2 btn(b0.x + 28, b0.y + 24);
    dl->AddCircleFilled(ImVec2(btn.x + 22, btn.y + 22), 22.0f,
                        with_alpha(kCobaltGlow, alpha * 0.9f));
    if (paused) {
        dl->AddTriangleFilled(ImVec2(btn.x + 16, btn.y + 10), ImVec2(btn.x + 16, btn.y + 34),
                              ImVec2(btn.x + 34, btn.y + 22), with_alpha(kTextPrimary, alpha));
    } else {
        dl->AddRectFilled(ImVec2(btn.x + 14, btn.y + 11), ImVec2(btn.x + 20, btn.y + 33),
                          with_alpha(kTextPrimary, alpha));
        dl->AddRectFilled(ImVec2(btn.x + 25, btn.y + 11), ImVec2(btn.x + 31, btn.y + 33),
                          with_alpha(kTextPrimary, alpha));
    }
    if (ImGui::IsMouseClicked(0) &&
        ImGui::IsMouseHoveringRect(btn, ImVec2(btn.x + 44, btn.y + 44))) {
        out.action = WmcAction::TogglePause;
    }

    // Seek bar.
    const float sx0 = b0.x + 96.0f, sx1 = b1.x - 300.0f, sy = b0.y + 45.0f;
    dl->AddRectFilled(ImVec2(sx0, sy), ImVec2(sx1, sy + 6),
                      with_alpha(kTextDim, alpha * 0.4f), 3.0f);
    const float frac = (total > 0) ? static_cast<float>(elapsed / total) : 0.0f;
    const float knob = sx0 + (sx1 - sx0) * std::clamp(frac, 0.0f, 1.0f);
    dl->AddRectFilled(ImVec2(sx0, sy), ImVec2(knob, sy + 6), with_alpha(kAccent, alpha), 3.0f);
    dl->AddCircleFilled(ImVec2(knob, sy + 3), 8.0f, with_alpha(kTextPrimary, alpha));

    if (ImGui::IsMouseDragging(0) &&
        ImGui::IsMouseHoveringRect(ImVec2(sx0, sy - 12), ImVec2(sx1, sy + 18))) {
        const float f = std::clamp((ImGui::GetMousePos().x - sx0) / (sx1 - sx0), 0.0f, 1.0f);
        out.action = WmcAction::SeekTo;
        out.seek_seconds = f * total;
    }

    char te[16], tt[16], line[40];
    format_time(elapsed, te, sizeof(te));
    format_time(total, tt, sizeof(tt));
    std::snprintf(line, sizeof(line), "%s / %s", te, tt);
    dl->AddText(m_font_body, 17.0f, ImVec2(sx1 + 18, sy - 8), with_alpha(kTextPrimary, alpha), line);

    // Volume readout.
    char vol[24];
    std::snprintf(vol, sizeof(vol), "%s %d%%", muted ? "MUTE" : "VOL",
                  static_cast<int>(volume * 100.0f + 0.5f));
    dl->AddText(m_font_body, 15.0f, ImVec2(sx1 + 18, sy + 18), with_alpha(kTextDim, alpha), vol);

    // Fullscreen button.
    const ImVec2 fb(b1.x - 232, b0.y + 24);
    dl->AddRectFilled(fb, ImVec2(fb.x + 104, fb.y + 44),
                      with_alpha(IM_COL32(30, 70, 130, 230), alpha), 8.0f);
    dl->AddRect(fb, ImVec2(fb.x + 104, fb.y + 44), with_alpha(kGlassHi, alpha * 0.5f), 8.0f);
    dl->AddText(m_font_body, 15.0f, ImVec2(fb.x + 12, fb.y + 14),
                with_alpha(kTextPrimary, alpha), fullscreen ? "Windowed" : "Fullscr.");
    if (ImGui::IsMouseClicked(0) &&
        ImGui::IsMouseHoveringRect(fb, ImVec2(fb.x + 104, fb.y + 44))) {
        out.action = WmcAction::ToggleFullscreen;
    }

    // Back to Hub button.
    const ImVec2 hb(b1.x - 120, b0.y + 24);
    dl->AddRectFilled(hb, ImVec2(hb.x + 96, hb.y + 44),
                      with_alpha(IM_COL32(30, 70, 130, 230), alpha), 8.0f);
    dl->AddRect(hb, ImVec2(hb.x + 96, hb.y + 44), with_alpha(kGlassHi, alpha * 0.5f), 8.0f);
    dl->AddText(m_font_body, 15.0f, ImVec2(hb.x + 12, hb.y + 14),
                with_alpha(kTextPrimary, alpha), "Hub");
    if (ImGui::IsMouseClicked(0) &&
        ImGui::IsMouseHoveringRect(hb, ImVec2(hb.x + 96, hb.y + 44))) {
        out.action = WmcAction::BackToHub;
    }
}

WmcInput WmcUi::render_player(SDL_Texture* video_tex, int video_w, int video_h,
                              const DiscItem& disc, const std::string& stream_name,
                              bool paused, double elapsed, double total,
                              bool& osd_visible, float volume, bool muted,
                              bool fullscreen, float t) {
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

    // Letterboxed video, aspect-fit.
    if (video_tex && video_w > 0 && video_h > 0) {
        const float va = static_cast<float>(video_w) / static_cast<float>(video_h);
        const float wa = w / h;
        float dw = w, dh = h;
        if (wa > va) dw = h * va; else dh = w / va;
        const ImVec2 c((w - dw) * 0.5f, (h - dh) * 0.5f);
        dl->AddImage(reinterpret_cast<ImTextureID>(video_tex), c, ImVec2(c.x + dw, c.y + dh));
    }

    // Auto-hide OSD on input; fade with lerp.
    const bool moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
    const bool any_key = ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ||
                         ImGui::IsKeyPressed(ImGuiKey_RightArrow) ||
                         ImGui::IsKeyPressed(ImGuiKey_Space) ||
                         ImGui::IsKeyPressed(ImGuiKey_UpArrow) ||
                         ImGui::IsKeyPressed(ImGuiKey_DownArrow);
    if (moved || any_key) { osd_visible = true; m_last_input_time = t; }
    if (osd_visible && (t - m_last_input_time) > 3.0) osd_visible = false;

    m_osd_alpha = lerp(m_osd_alpha, osd_visible ? 1.0f : 0.0f,
                       std::clamp(io.DeltaTime * 6.0f, 0.0f, 1.0f));

    if (ImGui::IsKeyPressed(ImGuiKey_Space, true)) out.action = WmcAction::TogglePause;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true)) out.action = WmcAction::SeekBackward;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) out.action = WmcAction::SeekForward;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, true)) out.action = WmcAction::BackToHub;

    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, 52), IM_COL32(4, 12, 30, 150));
    dl->AddText(m_font_body, 18.0f, ImVec2(28, 15), with_alpha(kTextPrimary, 0.9f),
                (disc.title + "   " + kBullet + "   " + stream_name).c_str());

    draw_osd(dl, w, h, m_osd_alpha, paused, elapsed, total, volume, muted, fullscreen, out);

    ImGui::End();
    return out;
}
