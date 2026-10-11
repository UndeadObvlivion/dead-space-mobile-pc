#include "game_patches.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <mutex>

#include <SDL.h>

#include "config.h"
#include "cpu.h"
#include "elf_loader.h"
#include "guest_util.h"
#include "hle/gles.h"
#include "hle/hle.h"
#include "input.h"
#include "log.h"
#include "objects.h"
#include "render/native.h"
#include "sbin.h"
#include "settings.h"

namespace patches {

namespace {

// ---------------------------------------------------------------- menu data
// Anti-aliasing choices: label string id and sample count
struct AaChoice {
    const char* id;
    int samples;
};
const AaChoice kAa[] = {{"AA_OFF", 0}, {"AA_2X", 2}, {"AA_4X", 4}, {"AA_8X", 8}};

struct Lang {
    const char* dir;
    const char* label;  // "Anti-Aliasing"
    const char* off;    // "Off"
};
const Lang kLangs[] = {
    {"en-us", "Anti-Aliasing", "Off"},          {"de-de", "Kantenglättung", "Aus"},
    {"fr-fr", "Anticrénelage", "Désactivé"},     {"it-it", "Anti-aliasing", "Disattivato"},
    {"es-es", "Antialiasing", "Desactivado"},    {"ru-ru", "Сглаживание", "Выкл."},
    {"ja-jp", "アンチエイリアス", "オフ"},        {"ko-kr", "안티 앨리어싱", "끄기"},
    {"zh-cn", "抗锯齿", "关"},
};

// Port option rows. They sit on two category pages after the game's own three: "Graphics" and
// "Controls". A category page holds all of its rows one under the other and scrolls vertically.
struct PortRow {
    std::string key;    // entity names BTN_PORT_<key>; label id OPTIONS_PORT_<key>
    std::string label;  // English label (all languages)
    bool slider;
    int bind;           // input binding index, or -1
    int page;           // 4 = Graphics, 5 = Controls
    int row;            // position on that page, from the top
};
const int kFpsLimits[5] = {60, 120, 144, 240, 0};  // Frame Rate Limit options (0 = unlimited)
constexpr int kGamePages = 3, kPageGraphics = 4, kPageControls = 5, kPageController = 6;
constexpr float kRowPitch = 37.0f, kRowTop = 19.0f, kRowScale = 0.8f;  // layout units (the game spaces its rows 60 apart)
const std::vector<PortRow>& port_rows() {
    static const std::vector<PortRow> rows = [] {
        std::vector<PortRow> r = {{"RENDERER", "Renderer", false, -1, kPageGraphics, 0},
                                  {"TEXFILTER", "Texture Filtering", false, -1, kPageGraphics, 0},
                                  {"BLOOM", "Bloom", false, -1, kPageGraphics, 0},
                                  {"SHARPEN", "Sharpening", false, -1, kPageGraphics, 0},
                                  {"AO", "Ambient Occlusion", false, -1, kPageGraphics, 0},
                                  {"DLIGHTS", "Dynamic Lights", false, -1, kPageGraphics, 0},
                                  {"SPECULAR", "Specular Highlights", false, -1, kPageGraphics, 0},
                                  {"SHADOWS", "Shadows", false, -1, kPageGraphics, 0},
                                  {"LIGHT_SHADOWS", "Light Shadows", false, -1, kPageGraphics, 0},
                                  {"LIGHT_CONTRAST", "Light Contrast", false, -1, kPageGraphics, 0},
                                  {"VIGNETTE", "Vignette", false, -1, kPageGraphics, 0},
                                  {"VSYNC", "V-Sync", false, -1, kPageGraphics, 0},
                                  {"FPS_LIMIT", "Frame Rate Limit", false, -1, kPageGraphics, 0},
                                  {"SHOW_FPS", "Show FPS", false, -1, kPageGraphics, 0},
                                  {"MOUSE_SENS", "Mouse Sensitivity", true, -1, kPageControls, 0},
                                  {"SMOOTHING", "Mouse Smoothing", false, -1, kPageControls, 0},
                                  {"FREE_LOOK", "Free Look (Standing)", false, -1, kPageControls, 0}};
        for (int i = 0; i < input::binding_count(); i++)
            r.push_back({"BIND_" + std::to_string(i), input::binding(i).label, false, i, kPageControls, 0});
        r.push_back({"RESET_BINDS", "Reset Controls", false, -1, kPageControls, 0});
        // controller page: stick sensitivity, one row per remappable button, reset
        r.push_back({"PAD_SENS", "Stick Sensitivity", true, -1, kPageController, 0});
        for (int i = 0; i < input::pad_binding_count(); i++)
            r.push_back({"PADBIND_" + std::to_string(i), input::binding(input::kPadBindBase + i).label, false, input::kPadBindBase + i, kPageController, 0});
        r.push_back({"PAD_RESET", "Reset Controller", false, -1, kPageController, 0});
        int n[8] = {};
        n[kPageGraphics] = 1;  // (the first Graphics row is Anti-Aliasing, a widget of its own)
        for (auto& x : r) x.row = n[x.page]++;
        return r;
    }();
    return rows;
}
int page_count() { return kPageController; }
int rows_on_page(int page) {
    int n = 0;
    for (auto& r : port_rows()) n += r.page == page;
    if (page == kPageGraphics) n++;  // Anti-Aliasing
    return n;
}
// height of a category page's content, layout units
float page_height(int page) { return kRowTop + kRowPitch * float(rows_on_page(page)) + 14.0f; }
const char* page_title(int page) {
    static const char* t[] = {"", "General", "Game", "Camera", "Graphics", "Controls", "Controller"};
    return page >= 1 && page <= 6 ? t[page] : "";
}
std::string key_text_id(int code) { return "PORT_KEY_" + std::to_string(code); }

// Mouse sensitivity slider: 0..1 maps linearly to 10%..300%
constexpr int kSensMin = 10, kSensMax = 300;

std::vector<u8> read_file(const std::string& p) {
    std::vector<u8> v;
    if (std::FILE* f = std::fopen(p.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        v.resize(size_t(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        if (std::fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
        std::fclose(f);
    }
    return v;
}

template <class T> T get(const u8* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <class T> void put(u8* p, T v) { std::memcpy(p, &v, sizeof v); }

// Adds rows to layouts.xml.sb. Each row is a copy of a game row layout ("Look Sensitivity"
// slider = UI_HEADER_OPTIONS8, "Invert Y Axis" selector = UI_HEADER_OPTIONS9) with its own
// entity names and label, placed in both options panes (main menu and pause menu):
//   - "Anti-Aliasing" under "Invert Y Axis" on page 3
//   - the port rows on pages 4+ (pages are 480 units apart, rows 60 apart)
bool patch_layouts(sbin::Database& db) {
    int s_layout = db.find_struct("Layout"), s_entity = db.find_struct("Entity"), s_vertex = db.find_struct("Vertex");
    if (s_layout < 0 || s_entity < 0 || s_vertex < 0) return false;
    const int o_id = db.field_offset(s_layout, "layoutId"), o_ents = db.field_offset(s_layout, "entities");
    const int o_name = db.field_offset(s_entity, "entityName"), o_str = db.field_offset(s_entity, "stringId");
    const int o_lay = db.field_offset(s_entity, "layout"), o_verts = db.field_offset(s_entity, "vertices");
    const int o_x = db.field_offset(s_vertex, "x"), o_y = db.field_offset(s_vertex, "y");
    const int esize = db.struct_size(s_entity), lsize = db.struct_size(s_layout);

    auto find_layout = [&](const char* name) -> long {
        u16 id = db.find_string(name);
        for (size_t i = 0; id && i < db.object_count(); i++)
            if (db.kind(i) == 0 && db.struct_of(i) == s_layout && get<u16>(db.body(i) + o_id) == id) return long(i);
        return -1;
    };
    auto entities = [&](size_t layout) { return size_t(get<s32>(db.body(layout) + o_ents)); };
    auto clone_array = [&](size_t arr, float dx, float dy, float k = 1.0f) -> s32 {
        u32 n = db.array_count(arr), stride = db.array_stride(arr);
        std::vector<u8> d(db.body(arr), db.body(arr) + size_t(n) * stride);
        for (u32 i = 0; i < n; i++) {
            u8* v = &d[i * stride];
            put<float>(v + o_x, get<float>(v + o_x) * k + dx);
            put<float>(v + o_y, get<float>(v + o_y) * k + dy);
        }
        return s32(db.add_array(db.array_elem_type(arr), db.array_struct(arr), n, d));
    };
    auto first_vertex = [&](const u8* e, int off) {
        s32 v = get<s32>(e + o_verts);
        return v < 0 ? 0.0f : get<float>(db.body(size_t(v)) + off);
    };

    long row8 = find_layout("UI_HEADER_OPTIONS8"), row9 = find_layout("UI_HEADER_OPTIONS9");
    if (row8 < 0 || row9 < 0 || find_layout("UI_HEADER_OPTIONS10") >= 0) return false;

    // 1. row layouts: copies of a template row with "SENSITIVITY" / "INVERT_Y" in its entity
    //    names replaced, and its label text id swapped
    auto make_row = [&](long tmpl, const std::string& old_key, const std::string& new_key, const std::string& label_id,
                        const std::string& layout_id, float scale = 1.0f) -> s32 {
        size_t ents = entities(size_t(tmpl));
        u32 n = db.array_count(ents);
        std::vector<u8> row(db.body(ents), db.body(ents) + size_t(n) * esize);
        for (u32 i = 0; i < n; i++) {
            u8* e = &row[i * esize];
            std::string name = db.string(get<u16>(e + o_name));
            if (name == "HEADER_" + old_key) put<u16>(e + o_str, db.add_string(label_id));
            if (auto k = name.find(old_key); k != std::string::npos) name.replace(k, old_key.size(), new_key);
            put<u16>(e + o_name, db.add_string(name));
            s32 v = get<s32>(e + o_verts);
            // A scaled row shrinks toward its origin (the right end of a selector row, the left end of a
            // slider row): shift it so both kinds stay centred on the same column. The slider itself moves
            // a little further right, clear of its label (text does not shrink).
            bool is_slider_row = old_key == "SENSITIVITY";
            float dx = scale == 1.0f ? 0.0f : (is_slider_row ? 1.0f : -1.0f) * 338.0f * (1.0f - scale) * 0.5f;
            if (scale != 1.0f && is_slider_row && db.string(get<u16>(e + o_name)) == "BTN_" + new_key) dx += 22.0f;
            if (v >= 0) put<s32>(e + o_verts, clone_array(size_t(v), dx, 0, scale));
        }
        s32 row_ents = s32(db.add_array(16, u16(s_entity), n, row));
        std::vector<u8> lay(size_t(lsize), 0);
        put<u16>(&lay[size_t(o_id)], db.add_string(layout_id));
        put<s32>(&lay[size_t(o_ents)], row_ents);
        return s32(db.add_struct_object(u16(s_layout), lay));
    };
    std::vector<s32> new_layouts;
    new_layouts.push_back(make_row(row9, "INVERT_Y", "ANTI_ALIASING", "OPTIONS_ANTI_ALIASING", "UI_HEADER_OPTIONS10", kRowScale));
    for (auto& r : port_rows())
        new_layouts.push_back(make_row(r.slider ? row8 : row9, r.slider ? "SENSITIVITY" : "INVERT_Y", "PORT_" + r.key,
                                       "OPTIONS_PORT_" + r.key, "UI_HEADER_PORT_" + r.key, kRowScale));

    // 2. register them in the list of all layouts (the object array that contains row 9)
    for (size_t i = 0; i < db.object_count(); i++) {
        if (db.kind(i) != 2 || db.array_elem_type(i) != 15) continue;
        u32 n = db.array_count(i);
        std::vector<u8> d(db.body(i), db.body(i) + size_t(n) * 4);
        bool has = false;
        for (u32 k = 0; k < n; k++) has |= get<s32>(&d[k * 4]) == s32(row9);
        if (!has) continue;
        for (s32 l : new_layouts) {
            d.resize(d.size() + 4);
            put<s32>(&d[d.size() - 4], l);
        }
        db.add_array(15, db.array_struct(i), u32(d.size() / 4), d);
        db.replace_with_last(i);
        break;
    }

    // The tutorials' touch graphics (arrows, the hand, device pictures) are taken out: in every tutorial
    // layout everything except the text box is hidden and moved far off screen. The text stays.
    {
        const int o_vis = db.field_offset(s_entity, "isVisible");
        int hidden = 0;
        for (const char* name : {"HUD_TUTORIAL_MOVE", "HUD_TUTORIAL_LOOK", "HUD_TUTORIAL_ROTATE", "HUD_TUTORIAL_SHAKE", "HUD_TUTORIAL_STASIS",
                                 "HUD_TUTORIAL_KINESIS", "HUD_TUTORIAL_SWIPES", "HUD_TUTORIAL_SHOOT", "HUD_TUTORIAL_QUICKTURN", "HUD_TUTORIAL_RIG",
                                 "HUD_TUTORIAL_MOVE_BUTTON", "HUD_TUTORIAL_MOVE_ARROWS", "HUD_TUTORIAL_LOOK_BUTTON", "HUD_TUTORIAL_SHOOT_BUTTON",
                                 "HUD_TUTORIAL_KINESIS_BUTTON", "HUD_TUTORIAL_QUICKTURN_BUTTON", "HUD_TUTORIAL_RIG_BUTTON", "HUD_TUTORIAL_ROTATE_MAIN",
                                 "HUD_TUTORIAL_SHAKE_MAIN", "HUD_TUTORIAL_ROTATE_ARROWS", "HUD_TUTORIAL_ROTATE_ICON", "HUD_TUTORIAL_SHAKE_ARROWS",
                                 "HUD_TUTORIAL_SHAKE_ICON", "HUD_TUTORIAL_IPHONE_BASE"}) {
            long l = find_layout(name);
            if (l < 0) continue;
            size_t arr = entities(size_t(l));
            u32 n = db.array_count(arr);
            std::vector<u8> d(db.body(arr), db.body(arr) + size_t(n) * esize);
            for (u32 i = 0; i < n; i++) {
                u8* e = &d[i * esize];
                if (db.string(get<u16>(e + o_name)) == "TUTORIAL_TEXT") continue;
                if (o_vis >= 0) put<u8>(e + o_vis, 0);
                s32 v = get<s32>(e + o_verts);
                if (v >= 0) put<s32>(e + o_verts, clone_array(size_t(v), 0, 100000.0f));
                hidden++;
            }
            db.add_array(16, u16(s_entity), n, d);
            db.replace_with_last(arr);
        }
        LOGI("menu patch: %d tutorial touch graphics hidden", hidden);
    }

    // 3. place the rows in the main menu and pause menu options panes
    int added = 0;
    for (const char* pane : {"UI_OPTIONS_PANE", "HUD_OPTIONS_PANE"}) {
        long p = find_layout(pane);
        if (p < 0) continue;
        size_t arr = entities(size_t(p));
        u32 n = db.array_count(arr);
        std::vector<u8> d(db.body(arr), db.body(arr) + size_t(n) * esize);
        // The last page only spaces two rows; move them up so three fit the scroll window
        // (same 60-unit spacing as the other pages).
        const float kShift = -26.0f;
        long slider = -1, invert = -1;
        for (u32 i = 0; i < n; i++) {
            u8* e = &d[i * esize];
            std::string name = db.string(get<u16>(e + o_name));
            if (name != "BTN_SENSITIVITY" && name != "BTN_INVERT_Y") continue;
            put<s32>(e + o_verts, clone_array(size_t(get<s32>(e + o_verts)), 0, kShift));
            (name == "BTN_INVERT_Y" ? invert : slider) = long(i);
        }
        if (invert < 0 || slider < 0) continue;
        const std::vector<u8> tmpl_toggle(d.begin() + invert * esize, d.begin() + (invert + 1) * esize);
        const std::vector<u8> tmpl_slider(d.begin() + slider * esize, d.begin() + (slider + 1) * esize);
        // page 3 rows sit at y 19 / 79 / 139 after the shift
        auto add_row = [&](bool is_slider, const std::string& name, const std::string& layout_id, int page, int row) {
            std::vector<u8> e = is_slider ? tmpl_slider : tmpl_toggle;
            float pitch = page > kGamePages ? kRowPitch : 60.0f;
            float dx = 480.0f * float(page - 3), dy = kRowTop + pitch * float(row) - first_vertex(e.data(), o_y);
            put<u16>(&e[size_t(o_name)], db.add_string(name));
            put<u16>(&e[size_t(o_lay)], db.add_string(layout_id));
            put<s32>(&e[size_t(o_verts)], clone_array(size_t(get<s32>(&e[size_t(o_verts)])), dx, dy));
            d.insert(d.end(), e.begin(), e.end());
        };
        add_row(false, "BTN_ANTI_ALIASING", "UI_HEADER_OPTIONS10", kPageGraphics, 0);
        for (auto& r : port_rows()) add_row(r.slider, "BTN_PORT_" + r.key, "UI_HEADER_PORT_" + r.key, r.page, r.row);
        db.add_array(16, u16(s_entity), u32(d.size() / size_t(esize)), d);
        db.replace_with_last(arr);
        added++;
    }
    LOGI("menu patch: anti-aliasing and %zu port rows (%d pages) added to %d options panes", port_rows().size(),
         page_count(), added);
    return added > 0;
}

std::mutex g_pad_text_lock;
std::set<std::string> g_pad_text_ids;
std::map<std::string, u32> g_pad_text_ws;
// Adds the new text ids to a language's string table.
bool patch_strings(sbin::Database& db, const Lang& lang) {
    int s_table = db.find_struct("StringTable"), s_pair = db.find_struct("StringPair");
    if (s_table < 0 || s_pair < 0) return false;
    const int o_pairs = db.field_offset(s_table, "stringpairs");
    const int o_id = db.field_offset(s_pair, "stringid"), o_text = db.field_offset(s_pair, "string");
    for (size_t i = 0; i < db.object_count(); i++) {
        if (db.kind(i) != 0 || db.struct_of(i) != s_table) continue;
        size_t arr = size_t(get<s32>(db.body(i) + o_pairs));
        u32 n = db.array_count(arr), stride = db.array_stride(arr);
        std::vector<u8> d(db.body(arr), db.body(arr) + size_t(n) * stride);
        std::vector<u8> proto(d.begin(), d.begin() + stride);  // keeps halversion etc.
        auto add = [&](const std::string& id, const std::string& text) {
            std::vector<u8> e = proto;
            put<u16>(&e[size_t(o_id)], db.add_string(id));
            put<u16>(&e[size_t(o_text)], db.add_string(text));
            d.insert(d.end(), e.begin(), e.end());
        };
        // English: the game's touch instructions reworded for keyboard and mouse, naming the keys bound
        // when the game starts (a rebound key shows after a restart)
        if (std::string(lang.dir) == "en-us")
        for (int pad = 0; pad < 2; pad++) {
            auto key = [pad](const char* action) { return input::action_label(action, pad != 0); };
            const std::string fwd = key("move_forward"), back = key("move_back"), left = key("move_left"), right = key("move_right");
            std::string move = fwd.size() == 1 && back.size() == 1 && left.size() == 1 && right.size() == 1 ? fwd + left + back + right
                                                                                                                 : fwd + ", " + left + ", " + back + ", " + right;
            if (pad) move = "the Left Stick";
            const std::string look = pad ? "Use the Right Stick" : "Move the mouse";
            const std::string aim = key("aim"), fire = key("fire"), use = key("interact"), alt = key("alt_fire"), rig = key("rig");
            const std::pair<const char*, std::string> texts[] = {
                {"TAP_TO_CONTINUE", pad ? "Press A to continue" : "Press any key to continue"},
                // (the same words label the plasma saw prompt and the grab prompts: the melee key does both)
                {"SWIPE", "Press " + key("melee")},
                {"TAP", "Tap " + key("melee")},
                {"LOADING_TIP_06", "Tap " + use + " rapidly if a Swarm attaches to you to kill them."},
                {"SWIPE_HINT", pad ? "D-pad Left / Right for more hints" : "Left / Right arrow keys for more hints"},
                {"TUTORIAL_MOVE_TEXT", "Use " + move + " to walk and strafe. Hold " + key("run") + " to run."},
                {"TUTORIAL_LOOK_TEXT", look + " to look around."},
                {"TUTORIAL_AIM_TEXT", "Hold " + aim + " to raise your weapon. " + look + " to aim at the power box. Press " + fire + " to fire."},
                {"TUTORIAL_FIRE_TEXT", "Press " + fire + " with your weapon raised to fire. Target enemy's limbs to kill them quickly."},
                {"TUTORIAL_MELEE_TEXT", "Get close to the Power Box, press " + use + " to open it and press " + key("melee") + " to swing your Plasma Saw."},
                {"TUTORIAL_PLASMA_SAW_TEXT", "Press " + key("melee") + " to swing the Plasma Saw."},
                {"TUTORIAL_STOMP_TEXT", "Get close to the stomp box and press " + key("stomp") + " to smash it open."},
                {"TUTORIAL_QUICK_TURN_TEXT", "Press " + key("quick_turn") + " to quickly turn 180 degrees."},
                {"TUTORIAL_RELOAD_TEXT", "Press " + key("reload") + " to reload weapon."},
                {"TUTORIAL_STASIS_TEXT", "While aiming, press " + key("stasis") + " to fire a stasis blast at the door."},
                {"TUTORIAL_KINESIS_TEXT", "Press " + use + " near an object to use kinesis. Aim and press " + fire + " to launch the object."},
                {"TUTORIAL_LOCATOR_TEXT", "Press " + key("locator") + " to access your locator."},
                {"TUTORIAL_WEAPON_SELECT_TEXT", std::string(pad ? "Use the D-pad" : "Press 1-4 or use the mouse wheel") + " to select a weapon. " + rig + " opens the RIG."},
                {"TUTORIAL_ALT_FIRE_TEXT", "Outside Zero-G, aim and press " + alt + " to switch fire modes."},
                {"TUTORIAL_ALT_FIRE_PC_TEXT", "Outside Zero-G, aim and press " + alt + " to switch fire modes."},
                {"TUTORIAL_TILT_BUTTON", "Outside Zero-G, aim and press " + alt + " to switch fire modes."},
                {"TUTORIAL_ZERO_G_JUMP_TEXT", "Aim at a walkable surface and press " + key("jump") + " to perform a Zero-G jump."},
                {"TUTORIAL_BENCH_TEXT", "Approach the Bench and press " + use + " to access the upgrade menu. Upgrading your RIG and weapons with Power Nodes is vital!"},
                {"TUTORIAL_STORE_TEXT", "Approach the Store and press " + use + " to go shopping. You can buy new suits, ammo and extra Power Nodes."},
                {"TUTORIAL_CORER_TEXT", "Press " + fire + " to fire four sheets of supercharged plasma."},
                {"TUTORIAL_CORER_BEAM_TEXT", look + " to direct the concentrated plasma beam."},
                {"TUTORIAL_REMOTE_SAW_TEXT", look + " to steer the remote saw"},
                {"TUTORIAL_SAW_SHOT_TEXT", "Press " + fire + " to fire a powerful high velocity saw blade."},
                {"LOADING_TIP_02", "Outside Zero-G, aim and press " + alt + " to switch fire modes."},
                {"LOADING_TIP_06", "Press " + use + " rapidly if a Swarm attaches to you to kill them."},
                {"LOADING_TIP_07", "Press " + use + " near an object to use kinesis. Aim and press " + fire + " to launch the object."},
                {"LOADING_TIP_08", "Press " + key("locator") + " to access your locator."},
                {"LOADING_TIP_09", "While steering the Ripper's Remote Saw you can press " + fire + " at any time to launch the saw straight ahead."},
                {"LOADING_TIP_12", "Press " + key("quick_turn") + " to quickly turn 180 degrees."},
                {"LOADING_TIP_13", "Press " + key("reload") + " to reload your weapon."},
                {"LOADING_TIP_14", look + " to steer the Corer's secondary fire beam attack."},
                {"LOADING_TIP_15", "When using the Ripper's secondary fire, press " + fire + " to fire a powerful high velocity saw blade."},
                {"LOADING_TIP_16", "Press " + key("stasis") + " while aiming to fire a stasis blast. Stasis slows down enemies dramatically for a short period of time."},
                {"LOADING_TIP_17", std::string(pad ? "Use the D-pad" : "Press 1-4 or use the mouse wheel") + " to select a weapon."},
                {"LOADING_TIP_18", "In Zero-G, aim at a walkable surface and press " + key("jump") + " to jump."},
            };
            int changed = 0;
            if (pad) {
                std::lock_guard lk(g_pad_text_lock);
                for (auto& t : texts) {
                    add(std::string(t.first) + "_PAD", t.second);
                    g_pad_text_ids.insert(t.first);
                    changed++;
                }
            }
            for (u32 k = 0; k < n && !pad; k++) {
                u8* e = &d[size_t(k) * stride];
                std::string id = db.string(get<u16>(e + o_id));
                for (auto& t : texts)
                    if (id == t.first) put<u16>(e + o_text, db.add_string(t.second)), changed++;
            }
            LOGI("strings: %d touch instructions reworded for %s", changed, pad ? "a controller" : "keyboard and mouse");
        }
        add("OPTIONS_ANTI_ALIASING", lang.label);
        add("AA_OFF", lang.off);
        add("AA_2X", "2x");
        add("AA_4X", "4x");
        add("AA_8X", "8x");
        // port rows (English only)
        for (auto& r : port_rows()) add("OPTIONS_PORT_" + r.key, r.label);
        for (int code : input::bindable_codes()) add(key_text_id(code), input::code_name(code));
        add("PORT_PRESS_KEY", "Press...");
        add("PORT_DASH", "-");
        add("PORT_RESET", "Reset");
        add("PORT_LEGACY", "Legacy");
        add("PORT_16X", "16x");
        add("PORT_LOW", "Low");
        add("PORT_FPS_60", "60");
        add("PORT_FPS_120", "120");
        add("PORT_FPS_144", "144");
        add("PORT_FPS_240", "240");
        add("PORT_FPS_OFF", "Unlimited");
        add("PORT_MEDIUM", "Medium");
        add("PORT_HIGH", "High");
        add("PORT_NATIVE", "Native");
        db.add_array(16, u16(s_pair), u32(d.size() / stride), d);
        db.replace_with_last(arr);
        return true;
    }
    return false;
}

// Text ids that have a controller wording (id + "_PAD" in the string table). While a controller is the
// device in use, a lookup of one of them is answered with the controller wording.
void on_get_string(cpu::Ctx& c) {
    if (!input::using_pad()) return;
    u32 ws = c.reg(1);
    if (ws < 0x10000) return;
    u32 b = mem::r32(ws), e = mem::r32(ws + 4);
    if (b < 0x10000 || e <= b || e - b > 96) return;
    std::string id;
    for (u32 p = b; p < e; p += 2) id += char(mem::r16(p));
    u32 g = 0;
    {
        std::lock_guard lk(g_pad_text_lock);
        if (!g_pad_text_ids.count(id)) return;
        g = g_pad_text_ws[id];
    }
    if (!g) {
        g = guest::wstring(id + "_PAD");
        std::lock_guard lk(g_pad_text_lock);
        g_pad_text_ws[id] = g;
    }
    c.set_reg(1, g);
}

// The tutorial box keeps the text it was given when it opened. Its last title / text ids are remembered,
// and when the device in use changes the box is given them again, so it shows the other wording at once.
u32 g_tut = 0, g_tut_vt = 0, fn_set_tut = 0;
std::string g_tut_title, g_tut_text;
std::string read_wstring(u32 ws) {
    std::string out;
    if (ws < 0x10000) return out;
    u32 b = mem::r32(ws), e = mem::r32(ws + 4);
    if (b < 0x10000 || e <= b || e - b > 200) return out;
    for (u32 p = b; p < e; p += 2) out += char(mem::r16(p));
    return out;
}
void on_set_tutorial_text(cpu::Ctx& c) {
    g_tut = c.reg(0);
    g_tut_vt = mem::r32(g_tut);
    g_tut_title = read_wstring(c.reg(1));
    g_tut_text = read_wstring(c.reg(2));
}
void refresh_tutorial_text() {
    static int last = -1;
    int pad = input::using_pad() ? 1 : 0;
    if (pad == last) return;
    bool first = last < 0;
    last = pad;
    if (first || !g_tut || !fn_set_tut || g_tut_text.empty()) return;
    input::run_on_game_thread([t = g_tut, vt = g_tut_vt, title = g_tut_title, text = g_tut_text](unsigned) {
        if (mem::r32(t) != vt) return;  // the tutorial object is gone
        cpu::call(fn_set_tut, {t, guest::wstring(title), guest::wstring(text)});
    });
}

std::mutex g_asset_lock;
std::map<std::string, std::shared_ptr<const std::vector<u8>>> g_assets;

// ---------------------------------------------------------------- options panel hooks
u32 fn_add_toggle = 0, fn_add_slider = 0, fn_set_index = 0, fn_add_option = 0, fn_set_pct = 0, fn_alloc_ctor = 0,
    fn_wstring_ctor = 0;
u32 g_panel = 0, g_panel_vtable = 0, g_selector = 0;
int g_shown_index = -1;

// widgets of the port rows on the current panel
struct PortWidget {
    u32 w = 0;
    int shown = -1;  // last seen selector index / slider percent
};
std::vector<PortWidget> g_port;
bool g_was_capturing = false;

// guest eastl wstring built by the game's own constructor (leaked; a few bytes per menu visit)
u32 make_wstring(const char* ascii) {
    static u32 alloc = 0, alloc_name = 0;
    if (!alloc) {
        alloc_name = mem::data_strdup("EASTL basic_string");
        alloc = mem::data_alloc(16);
        cpu::call(fn_alloc_ctor, {alloc, alloc_name});
    }
    size_t n = std::strlen(ascii);
    u32 text = mem::heap_alloc(u32(n * 2 + 2));
    for (size_t i = 0; i <= n; i++) mem::w16(text + u32(2 * i), u16(u8(ascii[i])));
    u32 s = mem::heap_alloc(32);
    std::memset(mem::host(s), 0, 32);
    cpu::call(fn_wstring_ctor, {s, text, alloc});
    return s;
}

int choice_for_samples(int samples) {
    int best = 0;
    for (int i = 0; i < 4; i++)
        if (kAa[i].samples <= samples) best = i;
    return best;
}

// Appends a widget for the named row to the panel; returns it (the panel's widget vector at
// +0x5c/+0x60 holds 12-byte shared_ptrs).
u32 add_widget(u32 panel, u32 fn, const std::string& name) {
    u32 before = mem::r32(panel + 0x60);
    cpu::call(fn, {panel, make_wstring(name.c_str())});
    u32 end = mem::r32(panel + 0x60);
    if (end == before) {
        LOGW("options: widget %s was not created", name.c_str());
        return 0;
    }
    return mem::r32(end - 12);
}

// Selector: options vector at +0xa0/+0xa4 (24-byte Option {int value; wstring text}), selected
// index at +0x9c. Replaces the options with the given text ids.
void set_options(u32 sel, const std::vector<std::string>& ids) {
    mem::w32(sel + 0xa4, mem::r32(sel + 0xa0));
    int value = 0;
    for (auto& id : ids) {
        u32 opt = mem::heap_alloc(24);
        u32 ws = make_wstring(id.c_str());
        mem::w32(opt, u32(value++));
        std::memcpy(mem::host(opt + 4), mem::host(ws), 20);
        cpu::call(fn_add_option, {sel, opt});
    }
}
void select(u32 sel, int idx) { cpu::call(fn_set_index, {sel, u32(idx)}); }
int selected(u32 sel) { return int(mem::r32(sel + 0x9c)); }

void show_binding(PortWidget& pw, int bind) {
    set_options(pw.w, {key_text_id(input::binding_code(bind)), "PORT_PRESS_KEY"});
    select(pw.w, 0);
    pw.shown = 0;
}

int mouse_sens() { return std::clamp(settings::get_int("mouse_sensitivity", 100), kSensMin, kSensMax); }

void on_panel_init(cpu::Ctx& c) {
    u32 panel = c.reg(0);
    if (panel == g_panel) return;  // already augmented
    u32 sel = add_widget(panel, fn_add_toggle, "BTN_ANTI_ALIASING");
    if (!sel) return;
    // Anti-aliasing: replace the default No/Yes with Off/2x/4x/8x (option value = samples)
    mem::w32(sel + 0xa4, mem::r32(sel + 0xa0));
    int maxs = hle::max_msaa();
    for (auto& ch : kAa) {
        if (ch.samples > maxs) continue;
        u32 opt = mem::heap_alloc(24);
        u32 ws = make_wstring(ch.id);
        mem::w32(opt, u32(ch.samples));
        std::memcpy(mem::host(opt + 4), mem::host(ws), 20);
        cpu::call(fn_add_option, {sel, opt});
    }
    int idx = choice_for_samples(hle::msaa_samples());
    select(sel, idx);
    g_panel = panel;
    g_panel_vtable = mem::r32(panel);
    g_selector = sel;
    g_shown_index = idx;

    // port rows
    g_port.assign(port_rows().size(), {});
    for (size_t i = 0; i < port_rows().size(); i++) {
        auto& r = port_rows()[i];
        auto& pw = g_port[i];
        pw.w = add_widget(panel, r.slider ? fn_add_slider : fn_add_toggle, "BTN_PORT_" + r.key);
        if (!pw.w) continue;
        if (r.key == "PAD_SENS") {
            pw.shown = std::clamp(settings::get_int("pad_sensitivity", 100), kSensMin, kSensMax);
            float v = float(pw.shown - kSensMin) / float(kSensMax - kSensMin);
            u32 bits;
            std::memcpy(&bits, &v, 4);
            cpu::call(fn_set_pct, {pw.w, bits});
        } else if (r.key == "MOUSE_SENS") {
            pw.shown = mouse_sens();
            float v = float(pw.shown - kSensMin) / float(kSensMax - kSensMin);
            u32 bits;
            std::memcpy(&bits, &v, 4);
            cpu::call(fn_set_pct, {pw.w, bits});
        } else if (r.key == "BLOOM" || r.key == "SHARPEN" || r.key == "AO" || r.key == "DLIGHTS" || r.key == "SPECULAR" || r.key == "SHADOWS" || r.key == "LIGHT_CONTRAST") {  // native renderer only
            set_options(pw.w, {"AA_OFF", "PORT_LOW", "PORT_MEDIUM", "PORT_HIGH"});
            pw.shown = r.key == "BLOOM" ? render::bloom() : r.key == "AO" ? render::ao() : r.key == "DLIGHTS" ? render::lights_level() : r.key == "SPECULAR" ? render::specular() : r.key == "SHADOWS" ? render::shadows() : r.key == "LIGHT_CONTRAST" ? render::env_light() : render::sharpen();
            select(pw.w, pw.shown);
        } else if (r.key == "FPS_LIMIT") {
            set_options(pw.w, {"PORT_FPS_60", "PORT_FPS_120", "PORT_FPS_144", "PORT_FPS_240", "PORT_FPS_OFF"});
            int v = settings::get_int("fps_limit", 60);
            pw.shown = 0;
            for (int i = 0; i < 5; i++)
                if (kFpsLimits[i] == v) pw.shown = i;
            select(pw.w, pw.shown);
        } else if (r.key == "VSYNC") {
            set_options(pw.w, {"NO", "YES"});
            pw.shown = settings::get_int("vsync", 1) ? 1 : 0;
            select(pw.w, pw.shown);
        } else if (r.key == "VIGNETTE") {
            set_options(pw.w, {"NO", "YES"});
            pw.shown = render::vignette() ? 1 : 0;
            select(pw.w, pw.shown);
        } else if (r.key == "LIGHT_SHADOWS") {
            set_options(pw.w, {"NO", "YES"});
            pw.shown = render::light_shadows() ? 1 : 0;
            select(pw.w, pw.shown);
        } else if (r.key == "SHOW_FPS") {
            set_options(pw.w, {"NO", "YES"});
            pw.shown = settings::get_int("show_fps", 0) ? 1 : 0;
            select(pw.w, pw.shown);
        } else if (r.key == "TEXFILTER") {
            // anisotropy levels up to what the GPU offers (native renderer only)
            std::vector<std::string> ids = {"AA_OFF"};
            const char* names[] = {"AA_2X", "AA_4X", "AA_8X", "PORT_16X"};
            for (int lv = 2, i = 0; lv <= render::max_anisotropy() && i < 4; lv *= 2, i++) ids.push_back(names[i]);
            set_options(pw.w, ids);
            int idx = 0;
            for (int lv = render::anisotropy(); lv > 1; lv /= 2) idx++;
            pw.shown = std::min(idx, int(ids.size()) - 1);
            select(pw.w, pw.shown);
        } else if (r.key == "RENDERER") {
            set_options(pw.w, {"PORT_LEGACY", "PORT_NATIVE"});
            pw.shown = render::enabled() ? 1 : 0;
            select(pw.w, pw.shown);
        } else if (r.key == "SMOOTHING" || r.key == "FREE_LOOK") {
            set_options(pw.w, {"NO", "YES"});  // the defaults are Yes, No
            pw.shown = settings::get_int(r.key == "SMOOTHING" ? "mouse_smoothing" : "free_look", r.key == "FREE_LOOK") ? 1 : 0;
            select(pw.w, pw.shown);
        } else if (r.bind >= 0) {
            show_binding(pw, r.bind);
        } else {  // reset
            set_options(pw.w, {"PORT_DASH", "PORT_RESET"});
            select(pw.w, 0);
            pw.shown = 0;
        }
    }
}

void refresh_bindings() {
    for (size_t i = 0; i < port_rows().size(); i++)
        if (port_rows()[i].bind >= 0 && g_port[i].w) show_binding(g_port[i], port_rows()[i].bind);
}

// Polls the port rows (main thread, once per frame).
void poll_port_rows() {
    bool capturing = input::capturing();
    if (g_was_capturing && !capturing) refresh_bindings();  // bound (others may have swapped) or cancelled
    g_was_capturing = capturing;
    for (size_t i = 0; i < port_rows().size(); i++) {
        auto& r = port_rows()[i];
        auto& pw = g_port[i];
        if (!pw.w) continue;
        if (r.slider) {
            float v;
            u32 bits = mem::r32(pw.w + 0x88);
            std::memcpy(&v, &bits, 4);
            int pct = kSensMin + int(std::lround(std::clamp(v, 0.0f, 1.0f) * float(kSensMax - kSensMin)));
            if (pct != pw.shown) {
                pw.shown = pct;
                settings::set_int(r.key == "PAD_SENS" ? "pad_sensitivity" : "mouse_sensitivity", pct);
            }
            continue;
        }
        int idx = selected(pw.w);
        if (idx == pw.shown) continue;
        pw.shown = idx;
        if (r.key == "SMOOTHING") {
            settings::set_int("mouse_smoothing", idx ? 1 : 0);
        } else if (r.key == "FREE_LOOK") {
            settings::set_int("free_look", idx ? 1 : 0);
        } else if (r.key == "BLOOM") {
            render::set_bloom(idx);
            settings::set_int("bloom", render::bloom());
        } else if (r.key == "SHARPEN") {
            render::set_sharpen(idx);
            settings::set_int("sharpen", render::sharpen());
        } else if (r.key == "LIGHT_CONTRAST") {
            render::set_env_light(idx);
            settings::set_int("light_contrast", render::env_light());
        } else if (r.key == "VSYNC") {
            settings::set_int("vsync", idx ? 1 : 0);
            SDL_GL_SetSwapInterval(idx ? 1 : 0);
        } else if (r.key == "VIGNETTE") {
            render::set_vignette(idx != 0);
            settings::set_int("vignette", idx ? 1 : 0);
        } else if (r.key == "LIGHT_SHADOWS") {
            render::set_light_shadows(idx != 0);
            settings::set_int("light_shadows", idx ? 1 : 0);
        } else if (r.key == "SHADOWS") {
            render::set_shadows(idx);
            settings::set_int("shadows", render::shadows());
        } else if (r.key == "SPECULAR") {
            render::set_specular(idx);
            settings::set_int("specular", render::specular());
        } else if (r.key == "DLIGHTS") {
            render::set_lights(idx);
            settings::set_int("dynamic_lights", render::lights_level());
        } else if (r.key == "AO") {
            render::set_ao(idx);
            settings::set_int("ambient_occlusion", render::ao());
        } else if (r.key == "FPS_LIMIT") {
            settings::set_int("fps_limit", kFpsLimits[std::clamp(idx, 0, 4)]);
        } else if (r.key == "SHOW_FPS") {
            settings::set_int("show_fps", idx ? 1 : 0);
        } else if (r.key == "TEXFILTER") {
            render::set_anisotropy(1 << idx);
            settings::set_int("anisotropy", render::anisotropy());
        } else if (r.key == "RENDERER") {
            if (render::available()) render::set_enabled(idx == 1);
            settings::set_string("renderer", render::enabled() ? "native" : "legacy");
            if (!render::enabled() && idx == 1) { select(pw.w, 0); pw.shown = 0; }  // not available
        } else if (r.bind >= 0) {
            if (idx == 1 && !capturing) {
                input::begin_capture(r.bind);
                g_was_capturing = true;
                LOGI("options: waiting for a key for %s", r.label.c_str());
            } else if (idx == 0 && capturing) {
                input::begin_capture(-1);  // arrow pressed again: cancel
            }
        } else if (idx == 1 && r.key == "PAD_RESET") {
            input::reset_pad_bindings();
            refresh_bindings();
            select(pw.w, 0);
            pw.shown = 0;
            LOGI("options: controller buttons reset to defaults");
        } else if (idx == 1) {  // reset controls
            input::reset_bindings();
            refresh_bindings();
            select(pw.w, 0);
            pw.shown = 0;
            LOGI("options: controls reset to defaults");
        }
    }
}

// ---------------------------------------------------------------- options paging
// OptionsWindow scrolls a panel three pages wide; the game hard-codes the three pages in
// OptionsPanel::getWidth (3 x page width), OptionsWindow::canNext, the page label "{0}/3" in
// OptionsWindow::draw and the page snapping in OptionsWindow::onEvent. Those are patched for
// page_count() pages. The page arithmetic in canNext/canPrev/onEvent/draw assumes a three-page
// panel, so getWidth still reports three pages to them; everyone else (scroll limits) sees all.
struct Range {
    u32 lo, hi;
};
Range g_page_math[4];
bool window_open_soon();
u32 fn_window_rect = 0, fn_scroll_event = 0, fn_play_sound = 0;

void on_panel_width(cpu::Ctx& c) {
    u32 pw = mem::r32(c.reg(0) + 0x70), lr = c.lr();
    int pages = page_count();
    for (auto& r : g_page_math)
        if (lr >= r.lo && lr < r.hi) pages = kGamePages;
    cpu::skip_function(c, pw * u32(pages));
}

// OptionsWindow::onEvent while not already snapping (+0xbc mode 0): the game's logic for any
// page count. A drag of more than 49 units past the current page snaps to the next/previous.
std::atomic<u32> g_loading_wait_ticks{0};  // LayerLoadingWait seen updating
u32 g_window = 0, g_window_ticks = 0;  // options window seen recently (its events run every frame)
int g_cur_page = 1;                    // page the window is on
u32 g_label_str = 0;                   // the page label's format string (guest wide string, 48 chars)

void set_page_label(int page) {
    if (!g_label_str) return;
    std::string label = std::string(page_title(page)) + "  {0}/" + std::to_string(page_count());
    for (size_t i = 0; i <= label.size() && i < 47; i++) mem::w16(g_label_str + u32(2 * i), i < label.size() ? u16(label[i]) : 0);
}

// A category page is as tall as its rows, which makes the window scroll it vertically (ScrollingWindow
// handles both axes; the game's own pages are exactly one window tall). The panel's height (+0x74,
// what OptionsPanel::getHeight returns) is set for the page the window is on.
u32 g_height_panel = 0, g_height_orig = 0;
int g_height_page = 0;
void set_panel_height(u32 win, int page) {
    u32 panel = mem::r32(win + 0x7c);
    if (!panel) return;
    if (panel != g_height_panel) g_height_panel = panel, g_height_orig = mem::r32(panel + 0x74);
    mem::w32(panel + 0x74, page > kGamePages ? std::max(g_height_orig, u32(page_height(page))) : g_height_orig);
}

void on_window_event(cpu::Ctx& c) {
    u32 win = c.reg(0), ev = c.reg(1);
    g_window = win;
    g_window_ticks = SDL_GetTicks();
    if (mem::r32(win + 0xbc) != 0) return;  // snapping animation: the game's own code
    static u32 rect = mem::heap_alloc(16);
    cpu::call(fn_window_rect, {rect, win});
    float w;
    u32 bits = mem::r32(rect + 8);
    std::memcpy(&w, &bits, 4);
    int pw = int(mem::r32(mem::r32(win + 0x7c) + 0x70));
    int half = int(-(float(kGamePages * pw) - w)) / 2;  // -(page width), as the game computes it
    if (half == 0) return;
    int scroll = s32(mem::r32(win + 0x9c));
    int p = int(std::ceil(float(scroll) / float(half) - 0.5f));
    int page = p + 1, pages = page_count();
    if (page != g_cur_page) {  // arrived on another page: start at its top
        g_cur_page = std::clamp(page, 1, pages);
        mem::w32(win + 0xa0, 0);
        set_page_label(g_cur_page);
    }
    if (mem::r32(win + 0x7c) != g_height_panel || page != g_height_page) {
        g_height_page = page;
        set_panel_height(win, std::clamp(page, 1, pages));
    }
    u32 res = cpu::call(fn_scroll_event, {win, ev}) & 0xff;
    int moved = p * half - scroll;
    int mode = 0, target = 0;
    if (moved > 49 && page < pages) {
        mode = 1;
        target = (p + 1) * half;
    } else if (moved < -49 && page > 1) {
        mode = 2;
        target = (p - 1) * half;
    }
    if (mode) {
        float now = float(s32(mem::r32(win + 0x9c))), zero = 0;
        std::memcpy(&bits, &zero, 4);
        mem::w32(win + 0x8c, bits);
        mem::w32(win + 0xbc, u32(mode));
        std::memcpy(&bits, &now, 4);
        mem::w32(win + 0xc0, bits);
        mem::w32(win + 0xc4, u32(target));
        mem::w32(win + 0xa0, 0);
        static u32 swipe = guest::wstring("deadspace/ui/swipe");
        cpu::call(fn_play_sound, {swipe});
    }
    cpu::skip_function(c, u32(s32(int8_t(res))));
}

// Starts the page-turn animation of `win` by `dir` pages, as a swipe would (OptionsWindow snap mode
// +0xbc 1 = next / 2 = previous, from scroll +0xc0 to target +0xc4).
bool turn_page(u32 win, int dir) {
    if (mem::r32(win + 0xbc) != 0) return false;  // already turning
    static u32 rect = mem::heap_alloc(16);
    cpu::call(fn_window_rect, {rect, win});
    float w;
    u32 bits = mem::r32(rect + 8);
    std::memcpy(&w, &bits, 4);
    int pw = int(mem::r32(mem::r32(win + 0x7c) + 0x70));
    int half = int(-(float(kGamePages * pw) - w)) / 2;  // -(page width), as the game computes it
    if (half == 0) return false;
    int scroll = s32(mem::r32(win + 0x9c));
    int p = int(std::ceil(float(scroll) / float(half) - 0.5f)), page = p + 1;
    if ((dir > 0 && page >= page_count()) || (dir < 0 && page <= 1)) return false;
    float now = float(scroll), zero = 0;
    std::memcpy(&bits, &zero, 4);
    mem::w32(win + 0x8c, bits);
    mem::w32(win + 0xbc, dir > 0 ? 1u : 2u);
    std::memcpy(&bits, &now, 4);
    mem::w32(win + 0xc0, bits);
    mem::w32(win + 0xc4, u32((p + dir) * half));
    mem::w32(win + 0xa0, 0);
    static u32 swipe = guest::wstring("deadspace/ui/swipe");
    cpu::call(fn_play_sound, {swipe});
    return true;
}

bool window_open() { return g_window && SDL_GetTicks() - g_window_ticks < 200; }
bool window_open_soon() { return window_open(); }

// Patches one instruction after checking it is the expected one.
bool patch_insn(u32 off, u32 expect, u32 insn) {
    u32 a = mem::ELF_BASE + off;
    if (mem::r32(a) != expect) {
        LOGW("options paging: unexpected instruction at %08x (%08x)", a, mem::r32(a));
        return false;
    }
    mem::w32(a, insn);
    return true;
}

void install_paging() {
    u32 width = elf::find("_ZN12OptionsPanel8getWidthEv");
    u32 ev = elf::find("_ZN13OptionsWindow7onEventERKN2im5EventE");
    u32 next = elf::find("_ZN13OptionsWindow7canNextEv"), prev = elf::find("_ZN13OptionsWindow7canPrevEv");
    u32 draw = elf::find(
        "_ZN13OptionsWindow4drawERN2im14SpriteGraphicsERN5eastl10shared_ptrINS0_11TexturePackENS3_9allocatorENS3_"
        "17smart_ptr_deleterIS5_EEEEffff");
    fn_window_rect = elf::find("_ZN15ScrollingWindow13getWindowRectEv");
    fn_scroll_event = elf::find("_ZN15ScrollingWindow7onEventERKN2im5EventE");
    fn_play_sound = elf::find("_ZN2im12SoundManager9playSoundERKN5eastl12basic_stringIwNS_20StringEASTLAllocatorEEE");
    // these offsets are for this exact library build
    if (!width || ev != mem::ELF_BASE + 0xe3880 || next != mem::ELF_BASE + 0xda4f4 || prev != mem::ELF_BASE + 0xda44c ||
        draw != mem::ELF_BASE + 0xe4560 || !fn_window_rect || !fn_scroll_event || !fn_play_sound) {
        LOGW("options paging: game functions not found; extra option pages disabled");
        return;
    }
    const u32 n = u32(page_count());
    // canNext: page <= 2  ->  page <= n-1
    // draw: clamp the shown page to 1..3  ->  1..n, and the label "{0}/3" -> "{0}/n"
    if (!patch_insn(0xda584, 0xE3500002, 0xE3500000 | (n - 1)) || !patch_insn(0xe4608, 0xE3530002, 0xE3530000 | n) ||
        !patch_insn(0xe460c, 0x13A03003, 0xC3A03000 | n))
        return;
    u32 str = g_label_str = mem::data_alloc(96, 4);
    set_page_label(1);
    // the string is addressed PC-relatively: literal = string - (add instruction + 8)
    for (auto [lit, add] : {std::pair{0xe4738u, 0xe463cu}, {0xe473cu, 0xe4678u}, {0xe4740u, 0xe4694u}})
        mem::w32(mem::ELF_BASE + lit, str - (mem::ELF_BASE + add + 8));
    g_page_math[0] = {next, next + 0xa8};
    g_page_math[1] = {prev, prev + 0xa8};
    g_page_math[2] = {ev, ev + 0x31c};
    g_page_math[3] = {draw, draw + 0x1e4};
    cpu::hook_entry(width, "OptionsPanel::getWidth", on_panel_width);
    cpu::hook_entry(ev, "OptionsWindow::onEvent", on_window_event);
    LOGI("options: %u pages", n);
}

// ---------------------------------------------------------------- glass reflection
// DementiaDoppleganger (the "reflection" of the player behind glass, chapter 2) is placed each
// frame by onPlayerSetWorldTransform at 2 * mirror point - player position: a reflection through a
// point, so moving along the glass moves the reflection the opposite way. Replaced with a
// reflection across the glass plane through the mirror point (+0x1dc). The glass (all four in the
// game are axis-aligned) faces the side the player is on when the effect starts: the plane normal
// is the dominant horizontal axis of player - mirror point at that moment.
std::map<u32, objs::Vec3> g_mirror_normal;

void on_doppleganger_follow(cpu::Ctx& c) {
    u32 d = c.reg(0);
    if (mem::r32(d + 0x1a4) != 1) return;  // not active: the game's code does nothing either
    static u32 fn_player = elf::find("_ZNK10GameObject9getPlayerEv"),
               fn_face = elf::find("_ZN10GameObject13aiFaceTowardsERK9btVector3"),
               fn_moved = elf::find("_ZN10GameObject16registerMovementEv"),
               fn_update = elf::find("_ZN10GameObject20updateWorldTransformEv");
    static u32 buf = mem::heap_alloc(16, 16);
    u32 player = cpu::call(fn_player, {d}), pb = mem::r32(player + 0x58), db = mem::r32(d + 0x58);
    if (!player || !pb || !db) return;
    objs::Vec3 cpt{objs::rf(d + 0x1dc), objs::rf(d + 0x1e0), objs::rf(d + 0x1e4)};
    objs::Vec3 p{objs::rf(pb + 0x34), objs::rf(pb + 0x38), objs::rf(pb + 0x3c)};
    auto it = g_mirror_normal.find(d);
    if (it == g_mirror_normal.end()) {
        float dx = p.x - cpt.x, dz = p.z - cpt.z;
        objs::Vec3 n = std::fabs(dx) >= std::fabs(dz) ? objs::Vec3{dx < 0 ? -1.0f : 1.0f, 0, 0}
                                                      : objs::Vec3{0, 0, dz < 0 ? -1.0f : 1.0f};
        it = g_mirror_normal.emplace(d, n).first;
        LOGI("glass reflection %08x: mirror plane through %.2f %.2f %.2f, normal %.2f %.2f", d, objs::rf(d + 0x1dc),
             objs::rf(d + 0x1e0), objs::rf(d + 0x1e4), n.x, n.z);
    }
    objs::Vec3 n = it->second;
    float k = 2 * ((p.x - cpt.x) * n.x + (p.z - cpt.z) * n.z);
    objs::Vec3 q{p.x - k * n.x, p.y, p.z - k * n.z};
    objs::wf(db + 0x34, q.x);
    objs::wf(db + 0x38, q.y);
    objs::wf(db + 0x3c, q.z);
    objs::wf(db + 0x40, 0);
    // facing: the player's, mirrored across the plane
    objs::Vec3 f = objs::facing(player);
    float kf = 2 * (f.x * n.x + f.z * n.z);
    objs::wf(buf, q.x + f.x - kf * n.x);
    objs::wf(buf + 4, q.y);
    objs::wf(buf + 8, q.z + f.z - kf * n.z);
    objs::wf(buf + 12, 0);
    cpu::call(fn_face, {d, buf});
    cpu::call(fn_moved, {d});
    cpu::call(fn_update, {d});
    cpu::skip_function(c, 0);
}

// debug: --level MAP_ID (e.g. 01_gov_02) replaces the first level loaded (the menu backdrop is
// also the world a new game continues in)
std::string g_level_override;
void on_load_level(cpu::Ctx& c) {
    std::string id;
    for (u32 p = mem::r32(c.reg(1)), e = mem::r32(c.reg(1) + 4); p && p < e && id.size() < 64; p += 2) id += char(mem::r16(p));
    LOGI("loading level %s", id.c_str());
    render::scene_changed();
    if (g_level_override.empty() || id == g_level_override) return;
    LOGI("debug: loading %s instead", g_level_override.c_str());
    c.set_reg(1, guest::wstring(g_level_override));
    g_level_override.clear();
}

// ---------------------------------------------------------------- portal culling
// PortalCulling::updateViewCulling finds the sector containing the camera point; if the point is
// outside every sector's box (seen after elevator rides) it finds none and every room is hidden,
// leaving only moving objects such as the elevator car drawn over black. Hooked right after the
// lookup (r0 = sector found, r6 = PortalCulling, point at sp+0xb0): fall back to the nearest sector.
float fval(u32 a) {
    float f;
    u32 v = mem::r32(a);
    std::memcpy(&f, &v, 4);
    return f;
}
bool g_cull_lost = false;
void on_sector_found(cpu::Ctx& c) {
    if (c.reg(0)) {
        if (g_cull_lost) LOGI("culling: camera back inside a room");
        g_cull_lost = false;
        return;
    }
    u32 pc = c.reg(6), pt = c.sp() + 0xb0;
    float p[3] = {fval(pt), fval(pt + 4), fval(pt + 8)};
    u32 best = 0;
    float best_d = 1e30f;
    for (u32 q = mem::r32(pc + 0x14), e = mem::r32(pc + 0x18); q < e; q += 4) {
        u32 sec = mem::r32(q), box = mem::r32(sec) + 0x28;  // CullNode AABB min at +0x28, max at +0x38
        float d = 0;
        for (int k = 0; k < 3; k++) {
            float lo = fval(box + 4 * k), hi = fval(box + 16 + 4 * k);
            float o = p[k] < lo ? lo - p[k] : p[k] > hi ? p[k] - hi : 0;
            d += o * o;
        }
        if (d < best_d) {
            best_d = d;
            best = sec;
        }
    }
    if (!g_cull_lost)
        LOGW("culling: camera point %.2f %.2f %.2f is outside every room; using the nearest (%.2f away)", p[0], p[1],
             p[2], std::sqrt(best_d));
    g_cull_lost = true;
    if (best) c.set_reg(0, best);
}

void on_panel_delete(cpu::Ctx& c) {
    if (c.reg(0) != g_panel) return;
    g_panel = g_selector = 0;
    g_port.clear();
    input::begin_capture(-1);
}

}  // namespace

void set_level_override(const std::string& id) { g_level_override = id; }

bool options_page(int dir) { return window_open() && turn_page(g_window, dir); }
bool options_open() { return window_open(); }
bool loading_wait() { return g_loading_wait_ticks && SDL_GetTicks() - g_loading_wait_ticks < 200; }

// Mouse wheel over the options window: scrolls a category page.
bool options_wheel(int dy) {
    if (!window_open() || g_cur_page <= kGamePages || mem::r32(g_window + 0xbc) != 0) return false;
    static u32 rect = mem::heap_alloc(16);
    cpu::call(fn_window_rect, {rect, g_window});
    float wh;
    u32 bits = mem::r32(rect + 12);
    std::memcpy(&wh, &bits, 4);
    int lo = -std::max(0, int(page_height(g_cur_page) - wh));
    int old = s32(mem::r32(g_window + 0xa0)), y = std::clamp(old + dy * int(kRowPitch), lo, 0);
    mem::w32(g_window + 0xa0, u32(y));
    return y != old;  // false at either end
}

bool options_click(float x, float y) {
    if (!window_open()) return false;
    // the arrows beside "//Options" in the window header (decoration in the game's layout), in
    // layout units from the top centre of the screen
    float k = std::min(g_config.screen_w / 480.0f, g_config.screen_h / 320.0f);
    float lx = (x - g_config.screen_w * 0.5f) / k, ly = y / k;
    if (ly < 15 || ly > 58) return false;
    if (lx > -205 && lx < -150) return turn_page(g_window, -1), true;
    if (lx > 145 && lx < 200) return turn_page(g_window, 1), true;
    return false;
}

std::shared_ptr<const std::vector<u8>> asset(const std::string& path) {
    std::string p = path;
    while (!p.empty() && p[0] == '/') p.erase(0, 1);
    bool layouts = p == "published/layoutdata/layouts.xml.sb";
    const Lang* lang = nullptr;
    for (auto& l : kLangs)
        if (p == std::string("published/stringdata/") + l.dir + "/deadspace.bin") lang = &l;
    if (!layouts && !lang) return nullptr;

    std::lock_guard lk(g_asset_lock);
    auto it = g_assets.find(p);
    if (it != g_assets.end()) return it->second;
    std::shared_ptr<const std::vector<u8>> result;
    sbin::Database db;
    if (db.parse(read_file(hle::g_assets_dir + "/" + p))) {
        bool ok = layouts ? patch_layouts(db) : patch_strings(db, *lang);
        if (ok) result = std::make_shared<const std::vector<u8>>(db.serialize());
    }
    if (!result) LOGW("could not patch %s; serving the original", p.c_str());
    g_assets[p] = result;
    return result;
}

void install() {
    // GameObjectPlayable::onUpdate shows the navigation line by itself after the player has been idle for
    // a while (which also tilts the camera down). Skip that: the line only appears when asked for.
    if (!patch_insn(0x19b9b0, 0xda000014, 0xea000014)) LOGW("idle navigation line: not patched");
    fn_add_toggle = elf::find("_ZN12OptionsPanel15addToggleWidgetEN5eastl12basic_stringIwN2im20StringEASTLAllocatorEEE");
    fn_add_slider = elf::find("_ZN12OptionsPanel15addSliderWidgetEN5eastl12basic_stringIwN2im20StringEASTLAllocatorEEE");
    fn_set_index = elf::find("_ZN8Selector16setSelectedIndexEi");
    fn_add_option = elf::find("_ZN8Selector9addOptionERKNS_6OptionE");
    fn_set_pct = elf::find("_ZN6Slider11setSlidePctEf");
    fn_alloc_ctor = elf::find("_ZN2im20StringEASTLAllocatorC1EPKc");
    fn_wstring_ctor = elf::find("_ZN5eastl12basic_stringIwN2im20StringEASTLAllocatorEEC1EPKwRKS2_");
    u32 init = elf::find("_ZN12OptionsPanel4initEffff");
    u32 del = elf::find("_ZN12OptionsPanelD0Ev");
    if (u32 ll = elf::find("_ZN11Application9loadLevelERKN5eastl12basic_stringIwN2im20StringEASTLAllocatorEEEi"))
        cpu::hook_entry(ll, "Application::loadLevel", on_load_level);
    if (!fn_add_toggle || !fn_add_slider || !fn_set_index || !fn_add_option || !fn_set_pct || !fn_alloc_ctor ||
        !fn_wstring_ctor || !init || !del) {
        LOGW("options: game functions not found, port menu entries disabled");
        return;
    }
    cpu::hook_entry(init, "OptionsPanel::init", on_panel_init);
    cpu::hook_entry(del, "OptionsPanel::~OptionsPanel", on_panel_delete);
    install_paging();
    fn_set_tut = elf::find("_ZN12TutorialAnim15setTutorialTextERKN5eastl12basic_stringIwN2im20StringEASTLAllocatorEEES6_");
    if (fn_set_tut) cpu::hook_entry(fn_set_tut, "TutorialAnim::setTutorialText", on_set_tutorial_text);
    if (u32 f = elf::find("_ZNK2im11StringTable17getStringIfExistsERKN5eastl12basic_stringIwNS_20StringEASTLAllocatorEEERS4_")) {
        if (!cpu::hook_entry(f, "im::StringTable::getStringIfExists", on_get_string)) LOGW("controller prompts: cannot hook the text lookup");
    }
    // the loading screen that waits for a tap: noted while it runs, so any key can stand in for the tap
    if (u32 f = elf::find("_ZN16LayerLoadingWait6updateEi"))
        cpu::hook_entry(f, "LayerLoadingWait::update", [](cpu::Ctx&) { g_loading_wait_ticks = SDL_GetTicks(); });
    if (u32 f = elf::find("_ZN20DementiaDoppleganger25onPlayerSetWorldTransformEv"))
        cpu::hook_entry(f, "DementiaDoppleganger::onPlayerSetWorldTransform", on_doppleganger_follow);
    // PortalCulling::updateViewCulling, after its updateCurrentSector call (this exact build)
    if (elf::find("_ZN13PortalCulling17updateViewCullingERKN5eastl10shared_ptrI16CameraControllerNS0_9allocatorENS0_"
                  "17smart_ptr_deleterIS2_EEEE") == mem::ELF_BASE + 0x44f0c &&
        mem::r32(mem::ELF_BASE + 0x44fd8) == 0xE3500000 && mem::r32(mem::ELF_BASE + 0x44fdc) == 0xE5860024)
        cpu::hook_entry(mem::ELF_BASE + 0x44fd8, "PortalCulling::updateViewCulling (sector)", on_sector_found);
    else
        LOGW("culling: updateViewCulling not as expected; lost-room fallback disabled");
}

// Scroll bar at the right edge of the options window on a category page: where the view is in the page.
void draw_scrollbar() {
    if (!window_open() || g_cur_page <= kGamePages || mem::r32(g_window + 0xbc) != 0) return;
    static u32 rect = mem::heap_alloc(16);
    cpu::call(fn_window_rect, {rect, g_window});
    float r[4];
    std::memcpy(r, mem::host(rect), 16);  // x, y, w, h in layout units
    float content = page_height(g_cur_page);
    if (content <= r[3] + 1.0f) return;
    // window layouts sit in a 480-unit frame centred on the (wider) screen
    float k = std::min(g_config.screen_w / 480.0f, g_config.screen_h / 320.0f), margin = (g_config.screen_w / k - 480.0f) * 0.5f;
    float scroll = std::clamp(-float(s32(mem::r32(g_window + 0xa0))), 0.0f, content - r[3]);
    float tx = (margin + 240.0f + 158.0f) * k,  // just inside the window frame's right edge
           ty = (r[1] + 2.0f) * k, tw = 3.5f * k, th = (r[3] - 4.0f) * k;
    float thumb = std::max(th * r[3] / content, 10.0f * k), ty2 = ty + (th - thumb) * scroll / (content - r[3]);
    hle::overlay_rect(tx, ty, tw, th, 0x0c2a3ec0u);        // track
    hle::overlay_rect(tx, ty2, tw, thumb, 0x8fdcffffu);    // thumb
}

void frame() {
    refresh_tutorial_text();
    draw_scrollbar();
    if (!g_selector) return;
    if (mem::r32(g_panel) != g_panel_vtable) {  // panel went away without us noticing
        g_panel = g_selector = 0;
        g_port.clear();
        return;
    }
    poll_port_rows();
    int idx = selected(g_selector);
    if (idx == g_shown_index) return;
    g_shown_index = idx;
    // option values hold the sample count
    u32 opts = mem::r32(g_selector + 0xa0), count = (mem::r32(g_selector + 0xa4) - opts) / 24;
    if (idx < 0 || u32(idx) >= count) return;
    int samples = int(mem::r32(opts + u32(idx) * 24));
    hle::set_msaa(samples);
    settings::set_int("msaa", samples);
}

}  // namespace patches
