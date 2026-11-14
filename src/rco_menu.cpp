#include "rco_menu.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace umdplay {

namespace {

// A movieplayer status object (what getPlayerStatus() returns): the menu
// script reads playListNumber / chapterNumber from it.
class RcoStatusObject : public VsmxNative {
public:
    RcoStatusObject(int playlist, int chapter)
        : m_playlist(playlist), m_chapter(chapter) {}
    std::string name() const override { return "movieplayer.status"; }
    VsmxValue get(const std::string& prop) override {
        if (prop == "playListNumber") return VsmxValue::number(m_playlist);
        if (prop == "chapterNumber")  return VsmxValue::number(m_chapter);
        return VsmxValue::number(0);
    }
private:
    int m_playlist, m_chapter;
};

std::string join_args(const std::vector<VsmxValue>& args) {
    std::string s;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) s += ",";
        s += args[i].to_string();
    }
    return s;
}

// Trailing decimal run of `s` ("chapter12" -> 12), or -1 when there is none.
int trailing_number(const std::string& s) {
    size_t end = s.size();
    size_t begin = end;
    while (begin > 0 && s[begin - 1] >= '0' && s[begin - 1] <= '9') --begin;
    if (begin == end || begin == 0) return -1;
    return std::atoi(s.c_str() + begin);
}

// Chapter number encoded in a chapter button's object name, else -1. Only names
// beginning with "chapter" count, so "main1" is not mistaken for a chapter.
int chapter_from_name(const std::string& name) {
    const std::string prefix = "chapter";
    if (name.size() <= prefix.size() ||
        name.compare(0, prefix.size(), prefix) != 0)
        return -1;
    return trailing_number(name);
}

}  // namespace

// ---------------------------------------------------------------------------
// RcoMenuHost
// ---------------------------------------------------------------------------
VsmxValue RcoMenuHost::call(const std::string& method,
                            const std::vector<VsmxValue>& args) {
    calls.push_back(m_name + "." + method + "(" + join_args(args) + ")");
    if (m_name == "movieplayer" && (method == "play" || method == "resume")) {
        play_called = true;
        if (method == "play") {
            if (args.size() >= 1) play_track = static_cast<int>(args[0].to_number());
            if (args.size() >= 2) play_chapter = static_cast<int>(args[1].to_number());
            if (args.size() >= 3) play_arg2 = static_cast<int>(args[2].to_number());
        }
    }
    if (m_name == "movieplayer" && method == "getPlayerStatus")
        return VsmxValue::object(
            std::make_shared<RcoStatusObject>(m_playlist, m_chapter));
    return VsmxValue();
}

VsmxValue RcoMenuHost::get(const std::string& prop) {
    gets.push_back(m_name + "." + prop);
    return VsmxValue();
}

void RcoMenuHost::set(const std::string& prop, const VsmxValue& v) {
    sets.push_back(m_name + "." + prop + "=" + v.to_string());
}

// ---------------------------------------------------------------------------
// RcoMenu
// ---------------------------------------------------------------------------
RcoMenu::RcoMenu() = default;
RcoMenu::~RcoMenu() = default;

bool RcoMenu::build(const RcoFile& rco, int playlist_number, int chapter_number) {
    m_valid = false;
    if (!rco.valid || !rco.vsmx.present || rco.vsmx.size == 0)
        return false;
    // The RCO object bytes are not retained by RcoFile, so re-decode the VSMX
    // from the program bytes the parser already extracted.
    if (rco.vsmx.code.empty())
        return false;

    m_rco = rco;   // metadata copy (images, objects, pages)

    // The parser already split the container; rebuild the program from the
    // decoded instruction bytes + constant pools.
    m_prog = make_vsmx_program(rco.vsmx.code, rco.vsmx.texts,
                               rco.vsmx.properties, rco.vsmx.names);
    m_prog.version = rco.vsmx.version;
    if (!m_prog.valid)
        return false;

    m_vm = std::make_unique<VsmxMachine>(m_prog);
    m_controller = std::make_shared<RcoMenuHost>("controller", playlist_number,
                                                 chapter_number);
    m_movie = std::make_shared<RcoMenuHost>("movieplayer", playlist_number,
                                            chapter_number);
    m_resource = std::make_shared<RcoMenuHost>("resource", playlist_number,
                                               chapter_number);
    m_system = std::make_shared<RcoMenuHost>("system", playlist_number,
                                             chapter_number);
    m_vm->bind("controller", m_controller);
    m_vm->bind("movieplayer", m_movie);
    m_vm->bind("resource", m_resource);
    m_vm->bind("system", m_system);

    // Page order: pageID N -> the N-th page in file order (main_menu, ...).
    m_page_order.clear();
    for (uint32_t p : rco.pages()) {
        const RcoObject* o = rco.object(p);
        if (o) m_page_order.push_back(o->name);
    }
    if (m_page_order.empty())
        return false;

    run_script();
    sync_page_from_script();
    m_page_index = std::clamp(m_page_index, 0, page_count() - 1);
    rebuild_page();
    m_valid = true;
    return true;
}

bool RcoMenu::run_script() {
    if (!m_vm) return false;
    m_controller->clear();
    m_movie->clear();
    const bool ok = m_vm->run();
    return ok;
}

void RcoMenu::sync_page_from_script() {
    int pid = static_cast<int>(m_vm->global("pageID").to_number());
    if (pid < 1 || pid > page_count()) pid = 1;
    m_page_index = pid - 1;
}

void RcoMenu::sync_focus_from_script() {
    // The script's focusID is a chapter/global id, not a per-page index; match
    // it against the button named "chapter<focusID>" when possible.
    int fid = static_cast<int>(m_vm->global("focusID").to_number());
    m_focus = 0;
    if (fid >= 1) {
        const std::string want = "chapter" + std::to_string(fid);
        const auto bs = buttons();
        for (size_t i = 0; i < bs.size(); ++i)
            if (bs[i]->name == want) { m_focus = static_cast<int>(i); break; }
    }
    if (m_focus >= static_cast<int>(buttons().size())) m_focus = -1;
}

void RcoMenu::rebuild_page() {
    m_page = RcoMenuPage{};
    if (m_page_index < 0 || m_page_index >= page_count()) return;
    m_page_name = m_page_order[m_page_index];

    // Locate the page object by name.
    const RcoObject* page = nullptr;
    for (const auto& o : m_rco.objects)
        if (o.name == m_page_name && o.is_page()) { page = &o; break; }
    if (!page) return;

    const float cx = kMenuDesignW * 0.5f, cy = kMenuDesignH * 0.5f;

    auto add = [&](const RcoObject& o) {
        if (o.image_offset == 0xFFFFFFFF && !(o.width > 0))
            return;   // nothing to draw (dummy / pure container)
        RcoMenuItem it;
        it.obj_offset = o.offset;
        it.name = o.name;
        it.type = o.type;
        it.is_button = o.is_button() && !o.on_push_event.empty();
        it.image_offset = o.image_offset;
        // Selected-state art uses the "<image name>_sel" naming convention.
        if (o.image_offset != 0xFFFFFFFF) {
            for (const auto& im : m_rco.images) {
                if (im.entry_offset == o.image_offset) {
                    const std::string sel = im.name + "_sel";
                    for (const auto& im2 : m_rco.images)
                        if (im2.name == sel) it.image_sel_offset = im2.entry_offset;
                    break;
                }
            }
        }
        it.red = o.red; it.green = o.green; it.blue = o.blue; it.alpha = o.alpha;
        it.on_push_event = o.on_push_event;
        // Centre-origin, Y-up object space -> normalised screen space (Y down).
        const float w = o.width * o.scale_w, h = o.height * o.scale_h;
        const float left   = (o.pos_x - w * 0.5f + cx) / kMenuDesignW;
        const float top    = (cy - (o.pos_y + h * 0.5f)) / kMenuDesignH;
        const float right  = (o.pos_x + w * 0.5f + cx) / kMenuDesignW;
        const float bottom = (cy - (o.pos_y - h * 0.5f)) / kMenuDesignH;
        it.rect_x = left; it.rect_y = top;
        it.rect_w = right - left; it.rect_h = bottom - top;
        m_page.items.push_back(std::move(it));
    };

    // Planes/backgrounds first, then buttons, so focus rings draw on top.
    for (uint32_t k : m_rco.children_of(page->offset)) {
        const RcoObject* o = m_rco.object(k);
        if (o && !o->is_button()) add(*o);
    }
    for (uint32_t k : m_rco.children_of(page->offset)) {
        const RcoObject* o = m_rco.object(k);
        if (o && o->is_button()) add(*o);
    }

    sync_focus_from_script();
    // Mark the focused item.
    const auto bs = buttons();
    for (auto& it : m_page.items) it.focused = false;
    if (m_focus >= 0 && m_focus < static_cast<int>(bs.size())) {
        for (auto& it : m_page.items)
            if (it.obj_offset == bs[m_focus]->obj_offset) it.focused = true;
    }
}

std::vector<const RcoMenuItem*> RcoMenu::buttons() const {
    std::vector<const RcoMenuItem*> v;
    for (const auto& it : m_page.items)
        if (it.is_button) v.push_back(&it);
    return v;
}

const RcoMenuItem* RcoMenu::focused_item() const {
    const auto bs = buttons();
    if (m_focus < 0 || m_focus >= static_cast<int>(bs.size())) return nullptr;
    return bs[m_focus];
}

int RcoMenu::move_focus(int dx, int dy) {
    const auto bs = buttons();
    if (bs.empty()) { m_focus = -1; return -1; }
    if (m_focus < 0 || m_focus >= static_cast<int>(bs.size())) m_focus = 0;
    const RcoMenuItem* cur = bs[m_focus];
    const float ccx = cur->rect_x + cur->rect_w * 0.5f;
    const float ccy = cur->rect_y + cur->rect_h * 0.5f;

    int best = -1;
    float best_cost = 1e9f;
    for (int i = 0; i < static_cast<int>(bs.size()); ++i) {
        if (i == m_focus) continue;
        const float bx = bs[i]->rect_x + bs[i]->rect_w * 0.5f;
        const float by = bs[i]->rect_y + bs[i]->rect_h * 0.5f;
        const float ddx = bx - ccx, ddy = by - ccy;
        // Require movement along the requested axis.
        if (dx > 0 && ddx < 0.02f) continue;
        if (dx < 0 && ddx > -0.02f) continue;
        if (dy > 0 && ddy < 0.02f) continue;
        if (dy < 0 && ddy > -0.02f) continue;
        const float primary = std::fabs(dx ? ddx : ddy);
        const float perp    = std::fabs(dx ? ddy : ddx);
        const float cost    = primary + perp * 2.0f;
        if (cost < best_cost) { best_cost = cost; best = i; }
    }
    if (best >= 0) {
        m_focus = best;
        for (auto& it : m_page.items) it.focused = false;
        for (auto& it : m_page.items)
            if (it.obj_offset == bs[best]->obj_offset) it.focused = true;
    }
    return m_focus;
}

int RcoMenu::focus_at(float nx, float ny) {
    const auto bs = buttons();
    if (bs.empty()) { m_focus = -1; return -1; }
    // Prefer the button whose rect contains the point.
    for (int i = 0; i < static_cast<int>(bs.size()); ++i) {
        const RcoMenuItem* it = bs[i];
        if (nx >= it->rect_x && nx <= it->rect_x + it->rect_w &&
            ny >= it->rect_y && ny <= it->rect_y + it->rect_h) {
            m_focus = i;
            for (auto& mi : m_page.items) mi.focused = false;
            for (auto& mi : m_page.items)
                if (mi.obj_offset == it->obj_offset) mi.focused = true;
            return i;
        }
    }
    // Otherwise the nearest centre.
    int best = 0; float best_d = 1e9f;
    for (int i = 0; i < static_cast<int>(bs.size()); ++i) {
        const float bx = bs[i]->rect_x + bs[i]->rect_w * 0.5f;
        const float by = bs[i]->rect_y + bs[i]->rect_h * 0.5f;
        const float d = (bx - nx) * (bx - nx) + (by - ny) * (by - ny);
        if (d < best_d) { best_d = d; best = i; }
    }
    m_focus = best;
    for (auto& mi : m_page.items) mi.focused = false;
    for (auto& mi : m_page.items)
        if (mi.obj_offset == bs[best]->obj_offset) mi.focused = true;
    return best;
}

MenuAction RcoMenu::activate_focused() {
    const RcoMenuItem* it = focused_item();
    if (!it) return MenuAction{};
    return activate_at(-1.0f, -1.0f);   // -1,-1 = use the focused item
}

MenuAction RcoMenu::activate_at(float nx, float ny) {
    MenuAction out;
    const RcoMenuItem* it;
    if (nx < 0.0f && ny < 0.0f) {
        it = focused_item();
    } else {
        const int idx = focus_at(nx, ny);
        const auto bs = buttons();
        it = (idx >= 0 && idx < static_cast<int>(bs.size())) ? bs[idx] : nullptr;
    }
    if (!it) return out;

    const int page_before = m_page_index;
    m_controller->clear();
    m_movie->clear();
    m_resource->clear();
    m_system->clear();

    if (!it->on_push_event.empty()) {
        m_vm->call_event(it->on_push_event);
        out.source = it->on_push_event;
    }

    // 1. The script explicitly asked the movie player to play/resume.
    if (m_movie->play_called) {
        out.kind = MenuActionKind::PlayMovie;
        out.track = m_movie->play_track;         // -1 when resume() (main feature)
        out.chapter = m_movie->play_chapter;
        return out;
    }

    // 2. The script moved the menu to another page (pageID changed).
    const int pid_after = static_cast<int>(m_vm->global("pageID").to_number());
    if (pid_after >= 1 && pid_after <= page_count() && pid_after - 1 != page_before) {
        m_page_index = pid_after - 1;
        rebuild_page();
        out.kind = MenuActionKind::SwitchPage;
        out.page = m_page_name;
        return out;
    }

    // 3. A chapter tile: its object name encodes the chapter number.
    const int ch = chapter_from_name(it->name);
    if (ch >= 0) {
        out.kind = MenuActionKind::PlayMovie;
        out.track = -1;      // main feature
        out.chapter = ch;
        return out;
    }

    // 4. Any other non-navigating button on the menu = "play the feature".
    out.kind = MenuActionKind::PlayMovie;
    out.track = -1;
    out.chapter = 0;
    return out;
}

MenuAction RcoMenu::back() {
    MenuAction out;
    // Prefer the page's own "return" button so the script drives the change.
    for (const auto& it : m_page.items) {
        if (it.is_button &&
            it.on_push_event.find("chapter_ret") != std::string::npos) {
            return activate_item(it);
        }
    }
    if (m_page_index != 0) {
        m_page_index = 0;
        rebuild_page();
        out.kind = MenuActionKind::SwitchPage;
        out.page = m_page_name;
        return out;
    }
    out.kind = MenuActionKind::ExitMenu;
    return out;
}

MenuAction RcoMenu::activate_item(const RcoMenuItem& it) {
    // Activate a specific item by object offset (used by back()).
    const int page_before = m_page_index;
    m_controller->clear();
    m_movie->clear();
    if (!it.on_push_event.empty())
        m_vm->call_event(it.on_push_event);
    const int pid_after = static_cast<int>(m_vm->global("pageID").to_number());
    MenuAction out;
    out.source = it.on_push_event;
    if (pid_after >= 1 && pid_after <= page_count() && pid_after - 1 != page_before) {
        m_page_index = pid_after - 1;
        rebuild_page();
        out.kind = MenuActionKind::SwitchPage;
        out.page = m_page_name;
    } else {
        out.kind = MenuActionKind::ExitMenu;
    }
    return out;
}

bool RcoMenu::goto_page(const std::string& name) {
    for (int i = 0; i < page_count(); ++i) {
        if (m_page_order[i] == name) {
            m_page_index = i;
            rebuild_page();
            return true;
        }
    }
    return false;
}

MenuStepResult RcoMenu::step_script() {
    MenuStepResult r;
    if (!m_vm) return r;
    m_movie->clear();
    const bool ok = m_vm->call_event("script:/main/onMenu");
    r.ran = ok;
    r.error = m_vm->last_error();
    if (ok) {
        const int pid = static_cast<int>(m_vm->global("pageID").to_number());
        if (pid >= 1 && pid <= page_count() && pid - 1 != m_page_index) {
            m_page_index = pid - 1;
            rebuild_page();
        }
        if (m_movie->play_called) {
            r.play_requested = true;
            r.play_track = m_movie->play_track;
            r.play_chapter = m_movie->play_chapter;
        }
    }
    return r;
}

const RcoMenuHost* RcoMenu::host(const std::string& name) const {
    if (name == "controller") return m_controller.get();
    if (name == "movieplayer") return m_movie.get();
    if (name == "resource") return m_resource.get();
    if (name == "system") return m_system.get();
    return nullptr;
}

}  // namespace umdplay
