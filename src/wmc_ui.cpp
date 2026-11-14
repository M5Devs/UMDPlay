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
    const float scale = lerp(0.84f, 1.0f, focus);
    const float cw = w * scale, ch = h * scale;
    const ImVec2 p0(cx - cw * 0.5f, cy - ch * 0.5f);
    const ImVec2 p1(cx + cw * 0.5f, cy + ch * 0.5f);

    dl->AddRectFilled(ImVec2(p0.x + 6, p0.y + 10), ImVec2(p1.x + 6, p1.y + 10),
                      IM_COL32(0, 0, 0, static_cast<int>(100 * focus)), 14.0f);

    const ImU32 top = with_alpha(IM_COL32(40, 96, 166, 255), lerp(0.5f, 1.0f, focus));
    const ImU32 bot = with_alpha(IM_COL32(10, 26, 60, 255),  lerp(0.5f, 1.0f, focus));
    dl->AddRectFilledMultiColor(p0, p1, top, top, bot, bot);

    // Cover art.
    if (disc.icon) {
        const float pad = cw * 0.10f;
        const float art = cw - pad * 2.0f;
        const ImVec2 a0(p0.x + pad, p0.y + pad);
        const ImVec2 a1(a0.x + art, a0.y + art * 0.62f);
        dl->AddImage(reinterpret_cast<ImTextureID>(disc.icon), a0, a1);
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
    const float ty = p1.y - ch * 0.26f;
    const char* title = disc.title.empty() ? "(untitled disc)" : disc.title.c_str();
    dl->AddText(m_font_body, 17.0f, ImVec2(p0.x + 12, ty),
                with_alpha(kTextPrimary, lerp(0.55f, 1.0f, focus)), title);
    if (!disc.disc_id.empty()) {
        dl->AddText(m_font_body, 14.0f, ImVec2(p0.x + 12, ty + 24),
                    with_alpha(kTextDim, lerp(0.45f, 1.0f, focus)),
                    disc.disc_id.c_str());
    }
}

WmcInput WmcUi::render_hub(const std::vector<DiscItem>& discs,
                           int& selected_disc, int& selected_stream, float t) {
    WmcInput out;
    if (!m_imgui_ready || discs.empty()) return out;

    ImGuiIO& io = ImGui::GetIO();
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::Begin("##wmc_hub", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const DiscItem& sel = discs[std::clamp(selected_disc, 0,
                                           static_cast<int>(discs.size()) - 1)];
    draw_background(dl, w, h, t, sel.pic1);

    // --- Keyboard / gamepad navigation ---
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) && selected_disc > 0) selected_disc--;
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) &&
        selected_disc + 1 < static_cast<int>(discs.size())) selected_disc++;
    const int nstreams = static_cast<int>(sel.streams.size());
    if (nstreams > 0) {
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) && selected_stream > 0) selected_stream--;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) && selected_stream + 1 < nstreams) selected_stream++;
    } else {
        selected_stream = 0;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, true) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, true) ||
        ImGui::IsKeyPressed(ImGuiKey_Space, true)) {
        out.action = WmcAction::Play;
    }

    // --- Header ---
    dl->AddText(m_font_huge, 40.0f, ImVec2(64, 40), kTextPrimary, "UMDPlay");
    dl->AddText(m_font_body, 17.0f, ImVec2(66, 88), kTextDim,
                "UMD VIDEO  \xE2\x80\xA2  PRESERVATION LIBRARY");
    dl->AddLine(ImVec2(64, 118), ImVec2(w - 64, 118), with_alpha(kGlassHi, 0.25f), 1.5f);

    // --- Smooth lerped carousel ---
    const float target = static_cast<float>(selected_disc);
    m_scroll = lerp(m_scroll, target, std::clamp(io.DeltaTime * 8.0f, 0.0f, 1.0f));

    const float card_w = std::min(w * 0.24f, 300.0f);
    const float card_h = card_w * 1.16f;
    const float cy = 118 + (h * 0.42f - 118) * 0.5f + card_h * 0.28f;
    const float spacing = card_w * 1.16f;
    const float cx_center = w * 0.5f;

    for (int i = 0; i < static_cast<int>(discs.size()); ++i) {
        const float cx = cx_center + (static_cast<float>(i) - m_scroll) * spacing;
        if (cx < -card_w || cx > w + card_w) continue;
        const float dist = std::fabs(static_cast<float>(i) - m_scroll);
        const float focus = std::clamp(1.0f - dist * 0.55f, 0.0f, 1.0f);
        draw_card(dl, discs[i], cx, cy, card_w, card_h, focus, i == selected_disc, t);
    }

    // --- Info panel: title, metadata, stream list ---
    const float panel_y = cy + card_h * 0.5f + 22.0f;
    const char* title = sel.title.empty() ? "(untitled disc)" : sel.title.c_str();
    const float title_px = 26.0f;
    const float tw = ImGui::CalcTextSize(title).x * (title_px / ImGui::GetFontSize());
    dl->AddText(m_font_title, title_px, ImVec2((w - tw) * 0.5f, panel_y),
                kTextPrimary, title);

    // Metadata chips.
    {
        char meta[160];
        std::snprintf(meta, sizeof(meta), "%s    %s",
                      sel.disc_id.empty()  ? "DISC_ID: --" : sel.disc_id.c_str(),
                      sel.category.empty() ? "CAT: --"     : sel.category.c_str());
        const float mw = ImGui::CalcTextSize(meta).x * (16.0f / ImGui::GetFontSize());
        dl->AddText(m_font_body, 16.0f, ImVec2((w - mw) * 0.5f, panel_y + 34),
                    kTextDim, meta);
    }

    // Stream list, centered, with the main feature highlighted.
    float sy = panel_y + 64.0f;
    const float list_w = std::min(w * 0.42f, 560.0f);
    const float list_x = w * 0.5f - list_w * 0.5f;
    for (int i = 0; i < nstreams; ++i) {
        const StreamItem& s = sel.streams[i];
        const bool active = (i == selected_stream);
        const ImVec2 r0(list_x, sy);
        const ImVec2 r1(list_x + list_w, sy + 28.0f);
        if (active) {
            dl->AddRectFilled(r0, r1, with_alpha(kCobaltGlow, 0.30f), 6.0f);
            dl->AddRect(r0, r1, with_alpha(kGlassHi, 0.55f), 6.0f);
        } else if (s.is_main) {
            dl->AddRectFilled(r0, r1, with_alpha(kAccent, 0.10f), 6.0f);
        }
        char line[192];
        std::snprintf(line, sizeof(line), "%s  %-12s  %5u MB",
                      s.is_main ? "[MAIN]" : "      ", s.name.c_str(),
                      s.size_bytes / (1024u * 1024u));
        dl->AddText(m_font_body, 16.0f, ImVec2(r0.x + 12, sy + 6),
                    active ? kTextPrimary : kTextDim, line);
        sy += 32.0f;
    }

    // --- Footer hints ---
    dl->AddText(m_font_body, 15.0f, ImVec2(64, h - 34), kTextDim,
                "LEFT / RIGHT  browse      UP / DOWN  select stream      "
                "ENTER  play      ESC / Q  quit");

    ImGui::End();
    return out;
}

void WmcUi::draw_osd(ImDrawList* dl, float w, float h, float alpha,
                     bool paused, double elapsed, double total, WmcInput& out) {
    if (alpha <= 0.001f) return;

    const float bar_h = 92.0f;
    const ImVec2 b0(w * 0.08f, h - bar_h - 26.0f);
    const ImVec2 b1(w * 0.92f, h - 26.0f);
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
    const float sx0 = b0.x + 96.0f, sx1 = b1.x - 210.0f, sy = b0.y + 45.0f;
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
    dl->AddText(m_font_body, 17.0f, ImVec2(sx1 + 22, sy - 8), with_alpha(kTextPrimary, alpha), line);

    // Back to Hub button.
    const ImVec2 hb(b1.x - 156, b0.y + 24);
    dl->AddRectFilled(hb, ImVec2(hb.x + 132, hb.y + 44),
                      with_alpha(IM_COL32(30, 70, 130, 230), alpha), 8.0f);
    dl->AddRect(hb, ImVec2(hb.x + 132, hb.y + 44), with_alpha(kGlassHi, alpha * 0.5f), 8.0f);
    dl->AddText(m_font_body, 16.0f, ImVec2(hb.x + 16, hb.y + 13),
                with_alpha(kTextPrimary, alpha), "Back to Hub");
    if (ImGui::IsMouseClicked(0) &&
        ImGui::IsMouseHoveringRect(hb, ImVec2(hb.x + 132, hb.y + 44))) {
        out.action = WmcAction::BackToHub;
    }
}

WmcInput WmcUi::render_player(SDL_Texture* video_tex, int video_w, int video_h,
                              const DiscItem& disc, const std::string& stream_name,
                              bool paused, double elapsed, double total,
                              bool& osd_visible, float t) {
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
                (disc.title + "   \xE2\x80\xA2   " + stream_name).c_str());

    draw_osd(dl, w, h, m_osd_alpha, paused, elapsed, total, out);

    ImGui::End();
    return out;
}
