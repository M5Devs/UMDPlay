#pragma once

// Interactive "Disc Menu" model: drives a parsed .RCO menu (see rco_parser.h)
// through its embedded VSMX program (see vsmx_vm.h) and exposes the result as
// a flat, screen-space draw list the UI can render with SDL/ImGui.
//
// This layer is deliberately SDL-free so it can be unit-tested headlessly. It:
//   * runs the menu's VSMX script with host objects (controller / movieplayer /
//     resource / system) that record the actions the disc asks for;
//   * tracks the active page + focused button, driven by the script's
//     "pageID"/"focusID" globals;
//   * performs D-pad / pointer navigation over the current page's buttons,
//     including the page's own next/back buttons (chapter_nextPush / ...);
//   * emits high-level MenuAction intents (switch page, play a track/chapter,
//     return to the disc menu, ...) for the host player to execute.
//
// Coordinate model: object positions are in the RCO's own space with the
// origin at the screen centre and Y growing upward. page_transform() maps that
// to a 0..1 screen-normalised space (X right, Y down) so the renderer can scale
// to any window while preserving the 16:9 aspect of the 854x480 design.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rco_parser.h"
#include "vsmx_vm.h"

namespace umdplay {

// The UMD menu design space (the "black" page's background plane is 854x480).
constexpr float kMenuDesignW = 854.0f;
constexpr float kMenuDesignH = 480.0f;

// One drawable item, resolved for the active page. `rect` is in normalised
// screen space ([0,1], Y down); `image_offset` keys into RcoFile::images.
struct RcoMenuItem {
    uint32_t    obj_offset = 0;         // OBJ entry offset (identity)
    std::string name;
    int         type = 0;               // RcoObjType
    bool        is_button = false;      // focusable
    bool        focused = false;        // the current focus ring target
    uint32_t    image_offset = 0xFFFFFFFF;
    uint32_t    image_sel_offset = 0xFFFFFFFF;  // "<name>_sel" variant, if any
    float       rect_x = 0, rect_y = 0, rect_w = 0, rect_h = 0;  // normalised
    float       red = 1, green = 1, blue = 1, alpha = 1;
    std::string on_push_event;          // "script:/main/chapterNPush"
};

// A page of the menu, ready to render.
struct RcoMenuPage {
    std::string              name;      // pageID -> page object name
    std::vector<RcoMenuItem> items;     // planes first, then buttons (draw order)
};

// What a button press / navigation asked the host to do.
enum class MenuActionKind {
    None,
    SwitchPage,     // show `page` (a page name, e.g. "chapter2_menu")
    PlayMovie,      // play track `track` starting at `chapter`
    BackToPlayer,   // return to live video playback
    ExitMenu,       // leave the disc menu (e.g. back to the hub)
};

struct MenuAction {
    MenuActionKind kind = MenuActionKind::None;
    std::string    page;       // SwitchPage target
    int            track = -1; // PlayMovie: 0-based playlist number
    int            chapter = -1;
    std::string    source;     // the event name that produced it (debug)

    explicit operator bool() const { return kind != MenuActionKind::None; }
};

// Result of running the menu's own VSMX loop (onMenu) after an interaction.
struct MenuStepResult {
    bool        ran = false;      // the script executed without error
    bool        play_requested = false;
    int         play_track = -1;  // playlist number the script asked to play
    int         play_chapter = -1;
    std::string error;
};

// A host object that records the methods/properties a menu script touches.
// It is a no-op stub: the model translates the recorded intent itself.
class RcoMenuHost : public VsmxNative {
public:
    RcoMenuHost(const std::string& n, int playlist_number, int chapter_number)
        : m_name(n), m_playlist(playlist_number), m_chapter(chapter_number) {}

    std::string name() const override { return m_name; }

    VsmxValue call(const std::string& method,
                   const std::vector<VsmxValue>& args) override;
    VsmxValue get(const std::string& prop) override;
    void      set(const std::string& prop, const VsmxValue& v) override;

    // Recorded actions (most recent interaction only).
    std::vector<std::string> calls;
    std::vector<std::string> gets;
    std::vector<std::string> sets;
    // movieplayer.play() arguments, when the script asks for playback.
    bool play_called = false;
    int  play_track = -1;
    int  play_chapter = -1;
    int  play_arg2 = -1;

    void clear() { calls.clear(); gets.clear(); sets.clear();
                   play_called = false; play_track = -1;
                   play_chapter = -1; play_arg2 = -1; }

private:
    std::string m_name;
    int         m_playlist;
    int         m_chapter;
};

// The disc menu model: owns a VSMX machine and the parsed RCO, tracks the
// active page + focus, and turns navigation/pointer input into MenuActions.
class RcoMenu {
public:
    RcoMenu();
    ~RcoMenu();

    // Build the model from a parsed RCO. Returns false when the RCO has no
    // usable VSMX program (the caller then hides the Disc Menu entry point).
    // `playlist_number`/`chapter_number` seed movieplayer.getPlayerStatus so
    // the script's state machine starts in the right place.
    bool build(const RcoFile& rco, int playlist_number = 3,
               int chapter_number = 1);

    bool valid() const { return m_valid; }

    // Run the menu's top-level script once (defines handlers + globals). Called
    // automatically by build(); exposed for tests.
    bool run_script();

    // Current page name + its resolved draw list.
    const std::string& page_name() const { return m_page_name; }
    const RcoMenuPage& page() const { return m_page; }
    int  page_index() const { return m_page_index; }
    int  index_1based() const { return m_page_index + 1; }
    int  page_count() const { return static_cast<int>(m_page_order.size()); }

    // Focus index within the current page's button list (-1 when none).
    int  focus_index() const { return m_focus; }
    // The focused item (nullptr when the page has no focusable items).
    const RcoMenuItem* focused_item() const;

    // --- Navigation (all re-evaluate focus / emit actions) -----------------
    // Move the focus ring one button in `dir` (-1 = up/left, +1 = down/right),
    // choosing the nearest neighbour by geometry. Returns the new focus index.
    int move_focus(int dx, int dy);
    // Move to a button whose centre is nearest the given normalised point
    // (mouse hover). Returns the focus index.
    int focus_at(float nx, float ny);
    // Activate the currently focused button (Enter / tap / click).
    MenuAction activate_focused();
    // Activate the button nearest the given normalised point (pointer click).
    MenuAction activate_at(float nx, float ny);
    // "Back"/cancel: step to the parent menu page if the disc provides one,
    // otherwise report ExitMenu. Returns the emitted action.
    MenuAction back();
    // Jump straight to a page by name (tests / screenshots). False if unknown.
    bool goto_page(const std::string& name);

    // Run the menu's onMenu loop and fold any requested playback into an
    // action. Used after a button press to let the script's state machine act.
    MenuStepResult step_script();

    // Latest recorded host interaction (for tests / diagnostics).
    const RcoMenuHost* host(const std::string& name) const;

private:
    void rebuild_page();
    void sync_focus_from_script();
    void sync_page_from_script();
    // Activate one specific item (used by back() to fire the page's own
    // "return" button). Returns the resulting action.
    MenuAction activate_item(const RcoMenuItem& it);
    std::vector<const RcoMenuItem*> buttons() const;

    RcoFile       m_rco;         // owned copy (the menu keeps it alive)
    VsmxProgram   m_prog;
    std::unique_ptr<VsmxMachine> m_vm;
    std::shared_ptr<RcoMenuHost> m_controller, m_movie, m_resource, m_system;

    std::vector<std::string> m_page_order;   // pageID 1..N -> page name
    std::string   m_page_name;
    RcoMenuPage   m_page;
    int           m_page_index = 0;          // 0-based index into m_page_order
    int           m_focus = -1;
    bool          m_valid = false;
};

}  // namespace umdplay
