#include "menu_nav.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include "config.h"
#include "cpu.h"
#include "elf_loader.h"
#include "game_patches.h"
#include "hle/gles.h"
#include "log.h"

namespace menu_nav {

namespace {

// LayoutButtons that received an UpdateEvent (type 10) in the current / previous game frame.
// Every button of every live layer updates, so the list is filtered by the button's enabled flag
// (+0x24; off for buttons of layers that are hidden or covered, e.g. the HUD under the pause
// menu), its sub-layout's visibility and the screen bounds (off-screen options pages).
struct Seen {
    u64 frame;
    u32 window;  // ScrollingWindow whose content the button is (0 = none)
};
std::map<u32, Seen> g_seen;  // button -> last update
// ScrollingWindow::onEvent calls in progress (main thread): the window and the caller's return
// address, restored by the return stub. Buttons updated inside belong to that window's content.
std::vector<std::pair<u32, u32>> g_windows;
u32 g_return_stub = 0;
u32 fn_window_rect = 0;
u64 g_frame = 1;
u32 fn_rect = 0;
int g_screen_w = 1, g_screen_h = 1;

u32 g_focus = 0;          // highlighted button
float g_fx = -1, g_fy = -1;  // its last centre
int g_repick_tries = 0;   // frames spent waiting for the new page's rows
int g_repick = 0;         // frames until the highlight is placed again (page turning)
bool g_keyboard = false;  // highlight shown (a navigation key was used since the last mouse use)
std::function<void(float, float)> g_tap;

void on_button_event(cpu::Ctx& c) {
    u32 ev = c.reg(1);
    if (mem::r32(ev + 4) == 10) g_seen[c.reg(0)] = {g_frame, g_windows.empty() ? 0 : g_windows.back().first};
}
void on_window_event(cpu::Ctx& c) {
    if (g_windows.size() > 16) return;  // runaway (should not happen)
    g_windows.push_back({c.reg(0), c.lr()});
    c.set_reg(14, g_return_stub);  // the window's onEvent returns through on_window_return
}
void on_window_return(cpu::Ctx& c) {
    if (g_windows.empty()) return;
    c.set_reg(14, g_windows.back().second);  // the stub's BX LR continues at the real caller
    g_windows.pop_back();
}
// Content offset and visible area of a ScrollingWindow, in layout units: a touch at screen x
// reaches content at x - rect.x - scroll (ScrollingWindow::onEvent)
bool window_view(u32 win, float& ox, float& oy, float r[4]) {
    static u32 buf = mem::heap_alloc(16, 8);
    if (!fn_window_rect || mem::r32(win) < 0x10000) return false;
    cpu::call(fn_window_rect, {buf, win});
    std::memcpy(r, mem::host(buf), 16);  // float x, y, w, h
    // window layouts sit in a 480-unit frame centred on the (wider) screen
    float k = std::min(g_screen_w / 480.0f, g_screen_h / 320.0f), margin = (g_screen_w / k - 480.0f) * 0.5f;
    r[0] += margin;
    ox = r[0] + float(s32(mem::r32(win + 0x9c)));
    s32 sy = s32(mem::r32(win + 0xa0));  // vertical scroll (INT_MIN-like in horizontal-only windows)
    oy = r[1] + float(sy > -100000 && sy < 100000 ? sy : 0);
    return true;
}
// The entries of the store's list (rows, scrolling vertically) and of the bench's list (pictures, side
// by side) are not buttons: their panel hit-tests touches itself. Each one drawn in a frame is noted
// with the place it was drawn at (layout units, scrolling included), so it can be highlighted and
// tapped like a button.
struct Item {
    u64 frame;
    float x, y, pw, ph;  // where it was drawn; width and height of its panel
    bool across;         // laid out side by side (bench)
};
std::map<u32, Item> g_items;
u32 g_item_window = 0;  // the ScrollingWindow showing them
void note_item(cpu::Ctx& c, bool across) {
    float v[4];
    const u32 bits[4] = {c.reg(3), mem::r32(c.reg(13)), mem::r32(c.reg(13) + 4), mem::r32(c.reg(13) + 8)};
    std::memcpy(v, bits, 16);
    g_items[c.reg(0)] = {g_frame, v[0], v[1], v[2], v[3], across};
}
void on_store_item_draw(cpu::Ctx& c) { note_item(c, false); }
void on_bench_item_draw(cpu::Ctx& c) { note_item(c, true); }
void on_panel_event(cpu::Ctx&) {
    if (!g_windows.empty()) g_item_window = g_windows.back().first;
}
// Size of an entry: its panel's extent across the list, the distance to the next entry along it
void item_size(const Item& it, float& w, float& h) {
    float pitch = 1e9f;
    for (auto& [o, other] : g_items) {
        if (other.frame + 1 < g_frame || other.across != it.across) continue;
        float d = std::fabs(it.across ? other.x - it.x : other.y - it.y);
        if (d > 1.0f) pitch = std::min(pitch, d);
    }
    if (pitch > 1e8f) pitch = it.across ? 138.0f : 27.0f;  // (a single entry: the game's layouts)
    w = it.across ? pitch : it.pw - 24.0f;  // (the store's scroll bar is at the right)
    h = it.across ? it.ph : pitch;
}
// The part of the screen the list's window shows, in the same units
bool item_view(float r[4]) {
    static u32 buf = mem::heap_alloc(16, 8);
    if (!g_item_window || !fn_window_rect || mem::r32(g_item_window) < 0x10000) return false;
    cpu::call(fn_window_rect, {buf, g_item_window});
    std::memcpy(r, mem::host(buf), 16);
    return r[2] > 1.0f && r[3] > 1.0f;
}
// 1 when all of the entry is in view, 0 when it is cut off or scrolled out
bool item_in_view(const Item& it) {
    float r[4], w, h;
    if (!item_view(r)) return true;
    item_size(it, w, h);
    return it.across ? it.x > r[0] - 3 && it.x + w < r[0] + r[2] + 3 : it.y > r[1] - 3 && it.y + h < r[1] + r[3] + 3;
}

std::string wname(u32 ws) {
    std::string s;
    for (u32 p = mem::r32(ws), e = mem::r32(ws + 4); p && p < e && s.size() < 64; p += 2) s += char(mem::r16(p));
    return s;
}

std::vector<Button> all_buttons() {
    std::vector<Button> out;
    static u32 rect = mem::heap_alloc(16, 8);
    float k = std::min(g_screen_w / 480.0f, g_screen_h / 320.0f);  // layout units -> pixels
    for (auto& [b, seen] : g_seen) {
        if (seen.frame + 1 < g_frame) continue;
        cpu::call(fn_rect, {rect, b});
        float r[4];
        std::memcpy(r, mem::host(rect), 16);
        if (seen.window) {
            float ox, oy, w[4];
            if (!window_view(seen.window, ox, oy, w)) continue;
            r[0] += ox;
            r[1] += oy;
            // only the part of the content the window shows (other options pages are hidden)
            float cx = r[0] + r[2] * 0.5f, cy = r[1] + r[3] * 0.5f;
            if (cx < w[0] || cx > w[0] + w[2] || cy < w[1] || cy > w[1] + w[3]) continue;
        }
        out.push_back({b, wname(b + 0x10), int(r[0] * k), int(r[1] * k), int(r[2] * k), int(r[3] * k), seen.window != 0});
    }
    for (auto& [o, it] : g_items) {
        if (it.frame + 1 < g_frame || !item_in_view(it)) continue;
        float w, h;
        item_size(it, w, h);
        Button b{o, wname(o + 0x10), int(it.x * k), int(it.y * k), int(w * k), int(h * k), false};
        b.item = true;
        out.push_back(b);
    }
    return out;
}

// The list entry after `from` (dir +1) or before it (-1), in view or not; 0 at the end of the list
u32 next_item(u32 from, int dir) {
    auto f = g_items.find(from);
    if (f == g_items.end()) return 0;
    u32 best = 0;
    float bd = 1e9f;
    for (auto& [o, it] : g_items) {
        if (it.frame + 1 < g_frame || it.across != f->second.across) continue;
        float d = (it.across ? it.x - f->second.x : it.y - f->second.y) * float(dir);
        if (d > 1.0f && d < bd) bd = d, best = o;
    }
    return best;
}
// Scrolls the list so that the entry is in view (it is drawn there from the next frame on)
void scroll_to_item(u32 obj) {
    Item& it = g_items[obj];
    float r[4], w, h;
    if (!item_view(r)) return;
    item_size(it, w, h);
    float lo = it.across ? r[0] : r[1], hi = lo + (it.across ? r[2] : r[3]), at = it.across ? it.x : it.y, len = it.across ? w : h;
    float d = at < lo ? lo - at : at + len > hi ? hi - (at + len) : 0.0f;
    if (d == 0.0f) return;
    const u32 field = g_item_window + (it.across ? 0x9c : 0xa0);
    mem::w32(field, u32(s32(mem::r32(field)) + s32(std::lround(d))));
    for (auto& [o, other] : g_items)  // (so that the highlight and the tap are right at once)
        if (other.across == it.across) (it.across ? other.x : other.y) += d;
}

// Buttons the player can use right now
std::vector<Button> usable() {
    std::vector<Button> out;
    bool options = patches::options_open();
    for (auto& b : all_buttons()) {
        if (b.item) {
            out.push_back(b);
            continue;
        }
        u32 sub = mem::r32(b.obj + 0x38);
        if (!mem::r8(b.obj + 0x24) || (sub && !mem::r8(sub + 0x54))) continue;  // disabled / hidden
        // the options screens keep two invisible, inactive side arrows from the chapter-select layout
        if (options && b.name.rfind("BTN_ARROW_", 0) == 0) continue;
        // title screen: invisible links to EA's other games / store
        if (b.name == "BTN_DS" || b.name == "BTN_EA") continue;
        if (b.w <= 0 || b.h <= 0) continue;
        float cx = b.x + b.w * 0.5f, cy = b.y + b.h * 0.5f;
        if (cx < 0 || cy < 0 || cx > g_screen_w || cy > g_screen_h) continue;  // off screen
        out.push_back(b);
    }
    // the lists stay drawn (faded out) under a confirmation dialog: their entries count only while
    // their screen's own Back button is in use
    bool back = false;
    for (auto& b : out) back |= !b.item && b.name == "BTN_BACK";
    if (!back) out.erase(std::remove_if(out.begin(), out.end(), [](const Button& b) { return b.item; }), out.end());
    return out;
}

const Button* find(const std::vector<Button>& v, u32 obj) {
    for (auto& b : v)
        if (b.obj == obj) return &b;
    return nullptr;
}

// The button nearest (fx, fy), or the top-left one
const Button* nearest(const std::vector<Button>& v, float fx, float fy) {
    const Button* best = nullptr;
    float bd = 1e30f;
    for (auto& b : v) {
        float cx = b.x + b.w * 0.5f, cy = b.y + b.h * 0.5f;
        float d = fx < 0 ? cy * 4 + cx : (cx - fx) * (cx - fx) + (cy - fy) * (cy - fy);
        if (d < bd) bd = d, best = &b;
    }
    return best;
}

// Next button from `from` in direction (dx, dy): must lie in that direction; prefers small
// sideways offset
const Button* step(const std::vector<Button>& v, const Button& from, int dx, int dy, bool same_row = false) {
    float fx = from.x + from.w * 0.5f, fy = from.y + from.h * 0.5f;
    const Button* best = nullptr;
    float bd = 1e30f;
    for (auto& b : v) {
        if (b.obj == from.obj) continue;
        if (same_row && (b.y >= from.y + from.h || b.y + b.h <= from.y)) continue;  // no vertical overlap
        float ox = b.x + b.w * 0.5f - fx, oy = b.y + b.h * 0.5f - fy;
        // sideways offset: how far apart the two are across the direction of travel (none when one is
        // above / beside the other, however wide they are: a list row under a tab)
        float gap = dy ? std::fabs(ox) - (b.w + from.w) * 0.5f : std::fabs(oy) - (b.h + from.h) * 0.5f;
        float along = ox * dx + oy * dy, side = std::max(0.0f, gap) + (std::fabs(ox * dy) + std::fabs(oy * dx)) * 0.02f;
        if (along <= 1) continue;
        float d = along + side * 2.5f;
        if (d < bd) bd = d, best = &b;
    }
    return best;
}

// The top-left button among the rows of a scrolling window (an options page), or null if none is shown.
const Button* first_row(const std::vector<Button>& v) {
    std::vector<Button> rows;
    for (auto& b : v)
        if (b.in_window) rows.push_back(b);
    const Button* r = nearest(rows, -1, -1);
    return r ? find(v, r->obj) : nullptr;
}

void set_focus(const Button* b) {
    if (!b) return;
    const bool moved = g_focus != b->obj;
    g_focus = b->obj;
    g_fx = b->x + b->w * 0.5f;
    g_fy = b->y + b->h * 0.5f;
    // a list entry is selected as the highlight reaches it (its details show, as when it is touched);
    // one that is selected already (state 4) is left alone: touching it again is the same as the
    // Purchase / Upgrade button, which is what the confirm key then does
    if (moved && b->item && g_tap && mem::r32(b->obj + 0x64) != 4) g_tap(g_fx, g_fy);
}

}  // namespace

void install() {
    u32 ev = elf::find("_ZN12LayoutButton7onEventERKN2im5EventE");
    fn_rect = elf::find("_ZN12LayoutButton13getActiveRectEv");
    if (!ev || !fn_rect || !cpu::hook_entry(ev, "LayoutButton::onEvent", on_button_event)) {
        LOGW("menu navigation: game functions not found");
        return;
    }
    fn_window_rect = elf::find("_ZN15ScrollingWindow13getWindowRectEv");
    u32 wev = elf::find("_ZN15ScrollingWindow7onEventERKN2im5EventE");
    g_return_stub = cpu::register_hle("ScrollingWindow::onEvent return", on_window_return);
    if (wev) cpu::hook_entry(wev, "ScrollingWindow::onEvent", on_window_event);
    const std::string draw = "4drawERN2im14SpriteGraphicsERN5eastl10shared_ptrINS0_11TexturePackENS3_9allocatorENS3_17smart_ptr_deleterIS5_EEEEffff";
    if (u32 f = elf::find(("_ZN9StoreItem" + draw).c_str())) cpu::hook_entry(f, "StoreItem::draw", on_store_item_draw);
    if (u32 f = elf::find(("_ZN9BenchItem" + draw).c_str())) cpu::hook_entry(f, "BenchItem::draw", on_bench_item_draw);
    for (const char* sym : {"_ZN10StorePanel7onEventERKN2im5EventE", "_ZN10BenchPanel7onEventERKN2im5EventE"})
        if (u32 f = elf::find(sym)) cpu::hook_entry(f, "list panel onEvent", on_panel_event);

}

void set_tap(std::function<void(float, float)> tap) { g_tap = std::move(tap); }

std::vector<Button> buttons() { return all_buttons(); }
void hide() { g_keyboard = false; }

bool event(const SDL_Event& e, bool gameplay_active) {
    if (gameplay_active) {
        g_keyboard = false;
        return false;
    }
    // mouse in use: hide the highlight (small motions, e.g. the window gaining focus, do not count)
    if (e.type == SDL_MOUSEBUTTONDOWN || (e.type == SDL_MOUSEMOTION && std::abs(e.motion.xrel) + std::abs(e.motion.yrel) > 8)) {
        if (g_keyboard) LOGD("menu nav: highlight hidden (mouse)");
        g_keyboard = false;
        return false;
    }
    if (e.type != SDL_KEYDOWN) return false;
    int dx = 0, dy = 0;
    bool confirm = false;
    switch (e.key.keysym.scancode) {
    case SDL_SCANCODE_W: case SDL_SCANCODE_UP: dy = -1; break;
    case SDL_SCANCODE_S: case SDL_SCANCODE_DOWN: dy = 1; break;
    case SDL_SCANCODE_A: case SDL_SCANCODE_LEFT: dx = -1; break;
    case SDL_SCANCODE_D: case SDL_SCANCODE_RIGHT: dx = 1; break;
    case SDL_SCANCODE_SPACE: case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: confirm = true; break;
    default: return false;
    }
    auto v = usable();
    if (v.empty()) return false;
    if (g_repick > 0) return true;  // page still turning
    const Button* cur = g_keyboard ? find(v, g_focus) : nullptr;
    if (!cur) {
        // first key (or the highlighted button went away): highlight the top-left button
        set_focus(nearest(v, -1, -1));  // top-left button
        g_keyboard = true;
        if (!confirm) return true;
        cur = find(v, g_focus);
    }
    if (cur && cur->item && !confirm) {
        // along the list: the next entry, scrolled into view if need be
        const bool across = g_items[cur->obj].across;
        if (int dir = across ? dx : dy) {
            if (u32 to = next_item(cur->obj, dir)) {
                scroll_to_item(to);
                v = usable();
                set_focus(find(v, to));
                return true;
            }
        }
    }
    if (confirm) {
        if (e.key.repeat || !cur || !g_tap) return true;
        g_tap(cur->x + cur->w * 0.5f, cur->y + cur->h * 0.5f);
        return true;
    }
    // left / right: stay in the row; at its end an options page turns, elsewhere move to the nearest
    // button that way
    const Button* n = step(v, *cur, dx, dy, dx != 0);
    // up / down in a list of options rows: when the next row that way is not on screen, the page scrolls
    // one row and the highlight moves onto the row that comes into view
    if (dy && cur->in_window && patches::options_open() && (!n || !n->in_window)) {
        if (patches::options_wheel(-dy)) {
            const u32 obj = cur->obj;
            v = usable();
            cur = find(v, obj);
            const Button* m = cur ? step(v, *cur, 0, dy) : nullptr;
            if (m && m->in_window) set_focus(m);
            else if (!cur) {  // the highlighted row itself scrolled out: the row now in its place
                std::vector<Button> rows;
                for (auto& b : v)
                    if (b.in_window) rows.push_back(b);
                if (const Button* r = nearest(rows, g_fx, g_fy)) set_focus(find(v, r->obj));
            }
            return true;
        }
    }
    if (!n && dx && patches::options_open()) {
        if (patches::options_page(dx)) {
            g_focus = 0;
            g_repick = 25;  // after the page has slid in: the first row of the new page
            g_repick_tries = 0;
        }
        return true;
    }
    if (!n) n = step(v, *cur, dx, dy);
    if (n) set_focus(n);
    return true;
}

void frame(bool gameplay_active) {
    g_screen_w = g_config.screen_w;
    g_screen_h = g_config.screen_h;
    g_frame++;
    for (auto it = g_seen.begin(); it != g_seen.end();)
        it = it->second.frame + 2 < g_frame ? g_seen.erase(it) : std::next(it);
    g_windows.clear();  // (between game frames nothing is in progress)
    for (auto it = g_items.begin(); it != g_items.end();)
        it = it->second.frame + 2 < g_frame ? g_items.erase(it) : std::next(it);
    if (gameplay_active || !g_keyboard) return;
    if (g_repick > 0 && --g_repick > 0) return;
    auto v = usable();
    const Button* b = find(v, g_focus);
    if (!b) {
        if (v.empty()) return;
        // highlighted button gone (new screen / page): the top-left one; on an options page its first
        // row, waiting a little if the page's rows have not slid into view yet
        const Button* first = patches::options_open() ? first_row(v) : nullptr;
        if (!first && patches::options_open() && g_repick_tries++ < 45) {
            g_repick = 1;
            return;
        }
        set_focus(first ? first : nearest(v, -1, -1));
        b = find(v, g_focus);
    }
    // outline, pulsing
    float t = (SDL_GetTicks() % 1000) / 1000.0f, a = 0.6f + 0.4f * std::sin(t * 6.2832f);
    unsigned col = 0x40d0ff00u | unsigned(a * 255);
    float th = std::max(3.0f, g_screen_h / 240.0f), x = float(b->x), y = float(b->y), w = float(b->w), h = float(b->h);
    hle::overlay_rect(x - th, y - th, w + 2 * th, th, col);
    hle::overlay_rect(x - th, y + h, w + 2 * th, th, col);
    hle::overlay_rect(x - th, y, th, h, col);
    hle::overlay_rect(x + w, y, th, h, col);
}

}  // namespace menu_nav
