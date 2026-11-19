#include "wm/window_manager.hpp"
#include "common/image_utils.hpp"

#include <X11/keysym.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <xcb/xcb_icccm.h>

namespace lunar {

namespace {
constexpr uint16_t kMinClientWidth = 160;
constexpr uint16_t kMinClientHeight = 90;
constexpr uint16_t kDefaultClientWidth = 760;
constexpr uint16_t kDefaultClientHeight = 480;
constexpr uint16_t kTitlebarHeight = 28;
constexpr int kMaxCascadeSteps = 6;
constexpr int16_t kCascadeStepPx = 28;

// Titlebar button geometry (offsets from right edge of frame)
constexpr int kBtnSize = 18;
constexpr int kBtnGap = 8;
constexpr int kBtnRightPad = 10;

bool client_supports_protocol(xcb_connection_t* conn, xcb_window_t win,
                              xcb_atom_t wm_protocols_atom, xcb_atom_t proto) {
    if (!conn || win == XCB_WINDOW_NONE || wm_protocols_atom == XCB_ATOM_NONE ||
        proto == XCB_ATOM_NONE) {
        return false;
    }
    xcb_icccm_get_wm_protocols_reply_t protocols{};
    auto cookie = xcb_icccm_get_wm_protocols(conn, win, wm_protocols_atom);
    if (!xcb_icccm_get_wm_protocols_reply(conn, cookie, &protocols, nullptr)) {
        return false;
    }
    bool supported = false;
    for (uint32_t i = 0; i < protocols.atoms_len; ++i) {
        if (protocols.atoms[i] == proto) {
            supported = true;
            break;
        }
    }
    xcb_icccm_get_wm_protocols_reply_wipe(&protocols);
    return supported;
}
} // namespace

WindowManager::~WindowManager() {
    if (conn_) {
        if (drag_mode_ != DragMode::None) {
            xcb_ungrab_pointer(conn_, XCB_CURRENT_TIME);
        }
        if (screen_) {
            xcb_ungrab_key(conn_, XCB_GRAB_ANY, screen_->root, XCB_MOD_MASK_ANY);
            for (auto& [win, cw] : clients_) {
                if (cw.frame != XCB_WINDOW_NONE) {
                    xcb_reparent_window(conn_, win, screen_->root, cw.rect.x, cw.rect.y);
                    xcb_map_window(conn_, win);
                    if (cw.title_gc != XCB_NONE) {
                        xcb_free_gc(conn_, cw.title_gc);
                    }
                    xcb_destroy_window(conn_, cw.frame);
                }
            }
        }
        if (wm_check_win_ != XCB_WINDOW_NONE) {
            xcb_destroy_window(conn_, wm_check_win_);
        }
        if (snap_preview_win_ != XCB_WINDOW_NONE) {
            xcb_destroy_window(conn_, snap_preview_win_);
        }
        if (root_pixmap_ != XCB_PIXMAP_NONE) {
            xcb_free_pixmap(conn_, root_pixmap_);
        }
        if (ewmh_initialized_) {
            xcb_ewmh_connection_wipe(&ewmh_);
        }
        if (keysyms_) {
            xcb_key_symbols_free(keysyms_);
        }
        if (screen_) {
            const uint32_t zero_mask = 0;
            xcb_change_window_attributes(conn_, screen_->root, XCB_CW_EVENT_MASK, &zero_mask);
        }
        xcb_flush(conn_);
        xcb_disconnect(conn_);
        conn_ = nullptr;
    }
}

bool WindowManager::init(const char* display_name) {
    conn_ = xcb_connect(display_name, &screen_idx_);
    if (!conn_ || xcb_connection_has_error(conn_)) {
        std::cerr << "lunar-wm: failed to connect to X display\n";
        if (conn_) {
            xcb_disconnect(conn_);
            conn_ = nullptr;
        }
        return false;
    }
    fcntl(xcb_get_file_descriptor(conn_), F_SETFD, FD_CLOEXEC);

    screen_ = get_screen(conn_, screen_idx_);
    if (!screen_) {
        std::cerr << "lunar-wm: failed to locate X screen " << screen_idx_ << '\n';
        return false;
    }

    screen_width_ = screen_->width_in_pixels;
    screen_height_ = screen_->height_in_pixels;

    const uint32_t root_mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT |
                               XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                               XCB_EVENT_MASK_PROPERTY_CHANGE;
    auto cookie = xcb_change_window_attributes_checked(
        conn_, screen_->root, XCB_CW_EVENT_MASK, &root_mask);
    UniqueMalloc<xcb_generic_error_t> err(xcb_request_check(conn_, cookie));
    if (err) {
        std::cerr << "lunar-wm: cannot claim SubstructureRedirect on root window "
                  << "(error code " << static_cast<int>(err->error_code)
                  << "). Is another window manager already running?\n";
        return false;
    }

    const uint32_t root_full_mask = root_mask | XCB_EVENT_MASK_BUTTON_PRESS;
    xcb_change_window_attributes(conn_, screen_->root, XCB_CW_EVENT_MASK, &root_full_mask);

    has_title_font_ = title_font_.init(false);

    setup_root_visuals();
    setup_ewmh();
    setup_randr();
    setup_keys();

    wm_protocols_atom_ = intern_atom(conn_, "WM_PROTOCOLS");
    wm_delete_window_atom_ = intern_atom(conn_, "WM_DELETE_WINDOW");
    wm_take_focus_atom_ = intern_atom(conn_, "WM_TAKE_FOCUS");
    wm_state_atom_ = intern_atom(conn_, "WM_STATE");
    wm_change_state_atom_ = intern_atom(conn_, "WM_CHANGE_STATE");
    motif_wm_hints_atom_ = intern_atom(conn_, "_MOTIF_WM_HINTS");
    gtk_frame_extents_atom_ = intern_atom(conn_, "_GTK_FRAME_EXTENTS");
    lunar_panel_win_atom_ = intern_atom(conn_, ipc::kPanelWindowAtom);
    lunar_panel_action_atom_ = intern_atom(conn_, ipc::kPanelActionAtom);
    lunar_wm_action_atom_ = intern_atom(conn_, ipc::kWmActionAtom);

    recompute_workarea();
    scan_existing_windows();
    xcb_flush(conn_);

    {
        display_tag_ = "0";
        if (const char* disp_env = std::getenv("DISPLAY"); disp_env && *disp_env) {
            std::string raw(disp_env);
            const size_t colon = raw.find(':');
            if (colon != std::string::npos) raw = raw.substr(colon + 1);
            const size_t dot = raw.find('.');
            if (dot != std::string::npos) raw.erase(dot);
            std::string clean;
            for (unsigned char c : raw) {
                if (std::isalnum(c) || c == '-' || c == '_') {
                    clean.push_back(static_cast<char>(c));
                }
            }
            if (!clean.empty()) display_tag_ = clean;
        }
        std::ofstream pid_out("/tmp/lunar-wm-" + display_tag_ + ".pid");
        if (pid_out) {
            pid_out << getpid() << '\n';
        }
    }

    std::cout << "lunar-wm: initialized on screen " << screen_width_ << "x" << screen_height_
              << " (workarea " << workarea_.x << "," << workarea_.y << " "
              << workarea_.width << "x" << workarea_.height << ")\n";
    return true;
}

void WindowManager::setup_root_visuals() {
    monitors_ = query_monitors(conn_, screen_);
    app_registry_.load();
    desktop_icons_ = load_desktop_icons(app_registry_);
    const uint32_t bg_pixel = theme::kRootBg;
    xcb_change_window_attributes(conn_, screen_->root, XCB_CW_BACK_PIXEL, &bg_pixel);
    xcb_clear_area(conn_, 0, screen_->root, 0, 0, 0, 0);
    prev_wallpaper_pixels_.assign(static_cast<size_t>(screen_width_) * screen_height_, 0x0b101b);
    apply_wallpaper(true);

    xcb_font_t cursor_font = xcb_generate_id(conn_);
    auto font_cookie = xcb_open_font_checked(conn_, cursor_font, 6, "cursor");
    UniqueMalloc<xcb_generic_error_t> font_err(xcb_request_check(conn_, font_cookie));
    if (!font_err) {
        xcb_cursor_t cursor = xcb_generate_id(conn_);
        xcb_create_glyph_cursor(conn_, cursor, cursor_font, cursor_font,
                                68, 69, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000);
        xcb_change_window_attributes(conn_, screen_->root, XCB_CW_CURSOR, &cursor);
        xcb_free_cursor(conn_, cursor);
        xcb_close_font(conn_, cursor_font);
    }
}

void WindowManager::apply_wallpaper(bool animate) {
    const std::string wp_path = resolve_wallpaper_path();
    if (wp_path.empty()) return;

    if (monitors_.empty()) {
        monitors_ = query_monitors(conn_, screen_);
    }

    std::string wp_key = wp_path + "|" + std::to_string(screen_width_) + "x" +
                         std::to_string(screen_height_);
    for (const auto& m : monitors_) {
        wp_key += "|" + std::to_string(m.rect.x) + "," + std::to_string(m.rect.y) + "," +
                  std::to_string(m.rect.width) + "x" + std::to_string(m.rect.height);
    }

    if (cached_wp_key_ != wp_key ||
        cached_wp_base_.pixels.size() != static_cast<size_t>(screen_width_) * screen_height_) {
        ImageBuffer base;
        base.width = screen_width_;
        base.height = screen_height_;
        base.pixels.assign(static_cast<size_t>(screen_width_) * screen_height_, theme::kRootBg);

        bool any_drawn = false;
        for (const auto& m : monitors_) {
            if (m.rect.width == 0 || m.rect.height == 0) continue;
            ImageBuffer mon_img = load_image_cover_scaled(wp_path, m.rect.width, m.rect.height);
            if (mon_img.pixels.size() < static_cast<size_t>(m.rect.width) * m.rect.height) continue;
            any_drawn = true;
            const int dst_x0 = std::max<int>(0, m.rect.x);
            const int dst_y0 = std::max<int>(0, m.rect.y);
            const int dst_x1 = std::min<int>(screen_width_, m.rect.x + m.rect.width);
            const int dst_y1 = std::min<int>(screen_height_, m.rect.y + m.rect.height);
            for (int dy = dst_y0; dy < dst_y1; ++dy) {
                const int sy = dy - m.rect.y;
                const int sx_off = dst_x0 - m.rect.x;
                const int copy_w = dst_x1 - dst_x0;
                if (copy_w <= 0) continue;
                std::copy_n(mon_img.pixels.begin() + static_cast<size_t>(sy) * m.rect.width + sx_off,
                            copy_w,
                            base.pixels.begin() + static_cast<size_t>(dy) * screen_width_ + dst_x0);
            }
        }
        if (!any_drawn) return;
        cached_wp_base_ = std::move(base);
        cached_wp_key_ = std::move(wp_key);
    }

    ImageBuffer img = cached_wp_base_;
    draw_desktop_icons(img);

    if (root_pixmap_ != XCB_PIXMAP_NONE) {
        xcb_free_pixmap(conn_, root_pixmap_);
        root_pixmap_ = XCB_PIXMAP_NONE;
    }

    root_pixmap_ = xcb_generate_id(conn_);
    xcb_create_pixmap(conn_, screen_->root_depth, root_pixmap_, screen_->root,
                      screen_width_, screen_height_);

    if (animate && prev_wallpaper_pixels_.size() == img.pixels.size()) {
        std::vector<uint32_t> frame_px(img.pixels.size());
        constexpr int kSteps = 5;
        for (int s = 1; s < kSteps; ++s) {
            const float lin = static_cast<float>(s) / static_cast<float>(kSteps);
            const float t = lin * lin * (3.0f - 2.0f * lin);
            const uint32_t a_w = static_cast<uint32_t>(t * 256.0f);
            const uint32_t inv_w = 256 - a_w;
            for (size_t i = 0; i < img.pixels.size(); ++i) {
                const uint32_t p0 = prev_wallpaper_pixels_[i];
                const uint32_t p1 = img.pixels[i];
                const uint32_t r = (((p0 >> 16) & 0xFF) * inv_w + ((p1 >> 16) & 0xFF) * a_w) >> 8;
                const uint32_t g = (((p0 >> 8) & 0xFF) * inv_w + ((p1 >> 8) & 0xFF) * a_w) >> 8;
                const uint32_t b = ((p0 & 0xFF) * inv_w + (p1 & 0xFF) * a_w) >> 8;
                frame_px[i] = (r << 16) | (g << 8) | b;
            }
            if (upload_pixels_to_pixmap(conn_, screen_, root_pixmap_,
                                        screen_width_, screen_height_, frame_px)) {
                xcb_change_window_attributes(conn_, screen_->root, XCB_CW_BACK_PIXMAP, &root_pixmap_);
                xcb_clear_area(conn_, 0, screen_->root, 0, 0, 0, 0);
                xcb_flush(conn_);
                usleep(10000);
            }
        }
    }

    if (upload_pixels_to_pixmap(conn_, screen_, root_pixmap_,
                                screen_width_, screen_height_, img.pixels)) {
        xcb_change_window_attributes(conn_, screen_->root, XCB_CW_BACK_PIXEL, &theme::kRootBg);
        xcb_change_window_attributes(conn_, screen_->root, XCB_CW_BACK_PIXMAP, &root_pixmap_);
        xcb_clear_area(conn_, 0, screen_->root, 0, 0, 0, 0);

        const xcb_atom_t xrootpmap = intern_atom(conn_, "_XROOTPMAP_ID");
        const xcb_atom_t esetroot = intern_atom(conn_, "ESETROOT_PMAP_ID");
        if (xrootpmap != XCB_ATOM_NONE) {
            xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, screen_->root,
                                xrootpmap, XCB_ATOM_PIXMAP, 32, 1, &root_pixmap_);
        }
        if (esetroot != XCB_ATOM_NONE) {
            xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, screen_->root,
                                esetroot, XCB_ATOM_PIXMAP, 32, 1, &root_pixmap_);
        }
    }
    prev_wallpaper_pixels_ = std::move(img.pixels);
}

void WindowManager::reload_desktop_icons() {
    desktop_icons_ = load_desktop_icons(app_registry_);
    if (selected_desktop_icon_ >= static_cast<int>(desktop_icons_.size())) {
        selected_desktop_icon_ = -1;
    }
    apply_wallpaper();
    xcb_flush(conn_);
}

int WindowManager::hit_test_desktop_icon(int16_t x, int16_t y) const {
    const Rect pmon = primary_monitor(monitors_).rect;
    const int kStartX = pmon.x + 22;
    const int kStartY = pmon.y + 48;
    constexpr int kCellW = 84;
    constexpr int kCellH = 92;
    const int max_rows = std::max(1, (static_cast<int>(pmon.height) - 48 - 32) / kCellH);

    for (size_t i = 0; i < desktop_icons_.size(); ++i) {
        const int col = static_cast<int>(i) / max_rows;
        const int row = static_cast<int>(i) % max_rows;
        const int cx = kStartX + col * kCellW;
        const int cy = kStartY + row * kCellH;
        if (x >= cx && x < cx + kCellW && y >= cy && y < cy + kCellH) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void WindowManager::draw_desktop_icons(ImageBuffer& img) {
    if (desktop_icons_.empty()) return;
    const Rect pmon = primary_monitor(monitors_).rect;
    const int kStartX = pmon.x + 22;
    const int kStartY = pmon.y + 48;
    constexpr int kCellW = 84;
    constexpr int kCellH = 92;
    constexpr int kIconSz = 44;
    const int max_rows = std::max(1, (static_cast<int>(pmon.height) - 48 - 32) / kCellH);

    for (size_t i = 0; i < desktop_icons_.size(); ++i) {
        const int col = static_cast<int>(i) / max_rows;
        const int row = static_cast<int>(i) % max_rows;
        const int cx = kStartX + col * kCellW;
        const int cy = kStartY + row * kCellH;
        if (cx + kCellW > pmon.x + static_cast<int>(pmon.width)) break;

        if (static_cast<int>(i) == selected_desktop_icon_) {
            fill_rounded_rect_aa(img, static_cast<float>(cx + 2), static_cast<float>(cy + 2),
                                 static_cast<float>(kCellW - 4), static_cast<float>(kCellH - 4),
                                 10.0f, 0x2563eb, 0.38f);
            stroke_rounded_rect_aa(img, static_cast<float>(cx + 2.5f), static_cast<float>(cy + 2.5f),
                                   static_cast<float>(kCellW - 5), static_cast<float>(kCellH - 5),
                                   10.0f, 1.2f, 0x38bdf8, 0.85f);
        }

        const int ix = cx + (kCellW - kIconSz) / 2;
        const int iy = cy + 8;
        fill_rounded_rect_aa(img, static_cast<float>(ix - 3), static_cast<float>(iy - 3),
                             static_cast<float>(kIconSz + 6), static_cast<float>(kIconSz + 6),
                             12.0f, 0x0f172a, 0.35f);
        app_registry_.draw_app_icon(img, desktop_icons_[i], ix, iy, kIconSz, title_font_);

        if (has_title_font_) {
            std::string label = desktop_icons_[i].name;
            if (label.size() > 11) {
                label = label.substr(0, 10) + "..";
            }
            const int tw = title_font_.measure_text(label, 11);
            const int tx = cx + (kCellW - tw) / 2;
            const int ty = cy + 72;
            title_font_.draw_text(img, tx + 1, ty + 1, label, 11, 0x000000);
            title_font_.draw_text(img, tx, ty, label, 11, 0xf8fafc);
        }
    }
}

void WindowManager::terminate_session() {
    const pid_t self_pid = getpid();
    const char* xephyr = std::getenv("LUNAR_XEPHYR");
    const bool is_xephyr = (xephyr && *xephyr == '1');
    const char* pid_path = is_xephyr ? "/tmp/lunar-de-x2.pids" : "/tmp/lunar-de-pids.txt";
    std::ifstream pids_in(pid_path);
    std::string line;
    while (std::getline(pids_in, line)) {
        const long p = std::strtol(line.c_str(), nullptr, 10);
        if (p > 1 && static_cast<pid_t>(p) != self_pid) {
            kill(static_cast<pid_t>(p), SIGTERM);
        }
    }
    if (is_xephyr) {
        std::ifstream x_in("/tmp/lunar-xephyr.pid");
        if (std::getline(x_in, line)) {
            const long xp = std::strtol(line.c_str(), nullptr, 10);
            if (xp > 1) {
                kill(static_cast<pid_t>(xp), SIGTERM);
            }
        }
    }
}

void WindowManager::setup_ewmh() {
    xcb_intern_atom_cookie_t* cookies = xcb_ewmh_init_atoms(conn_, &ewmh_);
    if (!xcb_ewmh_init_atoms_replies(&ewmh_, cookies, nullptr)) {
        std::cerr << "lunar-wm: warning: partial failure initializing EWMH atoms\n";
    }
    ewmh_initialized_ = true;

    wm_check_win_ = xcb_generate_id(conn_);
    xcb_create_window(conn_, XCB_COPY_FROM_PARENT, wm_check_win_, screen_->root,
                      -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                      XCB_COPY_FROM_PARENT, 0, nullptr);

    xcb_ewmh_set_supporting_wm_check(&ewmh_, screen_->root, wm_check_win_);
    xcb_ewmh_set_supporting_wm_check(&ewmh_, wm_check_win_, wm_check_win_);
    xcb_ewmh_set_wm_name(&ewmh_, wm_check_win_, 8, "lunar-wm");

    std::array<xcb_atom_t, 22> supported = {
        ewmh_._NET_SUPPORTED,
        ewmh_._NET_ACTIVE_WINDOW,
        ewmh_._NET_CLIENT_LIST,
        ewmh_._NET_CLIENT_LIST_STACKING,
        ewmh_._NET_SUPPORTING_WM_CHECK,
        ewmh_._NET_WM_NAME,
        ewmh_._NET_WM_DESKTOP,
        ewmh_._NET_WORKAREA,
        ewmh_._NET_NUMBER_OF_DESKTOPS,
        ewmh_._NET_CURRENT_DESKTOP,
        ewmh_._NET_WM_WINDOW_TYPE,
        ewmh_._NET_WM_WINDOW_TYPE_DOCK,
        ewmh_._NET_WM_WINDOW_TYPE_NORMAL,
        ewmh_._NET_WM_STRUT_PARTIAL,
        ewmh_._NET_WM_STRUT,
        ewmh_._NET_WM_STATE,
        ewmh_._NET_WM_STATE_MAXIMIZED_VERT,
        ewmh_._NET_WM_STATE_MAXIMIZED_HORZ,
        ewmh_._NET_WM_STATE_FULLSCREEN,
        ewmh_._NET_WM_STATE_HIDDEN,
        ewmh_._NET_CLOSE_WINDOW,
        ewmh_._NET_WM_MOVERESIZE,
    };

    xcb_ewmh_set_supported(&ewmh_, screen_idx_,
                           static_cast<uint32_t>(supported.size()), supported.data());
    xcb_ewmh_set_number_of_desktops(&ewmh_, screen_idx_, 2);
    xcb_ewmh_set_current_desktop(&ewmh_, screen_idx_, current_desktop_);
    xcb_ewmh_set_active_window(&ewmh_, screen_idx_, XCB_WINDOW_NONE);
    xcb_ewmh_set_client_list(&ewmh_, screen_idx_, 0, nullptr);
    xcb_ewmh_set_client_list_stacking(&ewmh_, screen_idx_, 0, nullptr);
}

void WindowManager::setup_randr() {
    const xcb_query_extension_reply_t* ext =
        xcb_get_extension_data(conn_, &xcb_randr_id);
    if (ext && ext->present) {
        randr_available_ = true;
        randr_first_event_ = ext->first_event;
        xcb_randr_select_input(conn_, screen_->root, XCB_RANDR_NOTIFY_MASK_SCREEN_CHANGE);
    }
}

void WindowManager::setup_keys() {
    keysyms_ = xcb_key_symbols_alloc(conn_);
    if (!keysyms_) {
        std::cerr << "lunar-wm: warning: failed to allocate xcb_key_symbols\n";
        return;
    }

    UniqueMalloc<xcb_keycode_t> numlock_codes(
        xcb_key_symbols_get_keycode(keysyms_, XK_Num_Lock));
    if (numlock_codes && numlock_codes.get()[0] != XCB_NO_SYMBOL) {
        numlock_mask_ = detect_numlock_mask(conn_, numlock_codes.get()[0]);
    }

    grab_keys();
}

void WindowManager::grab_keys() {
    if (!keysyms_) return;
    xcb_ungrab_key(conn_, XCB_GRAB_ANY, screen_->root, XCB_MOD_MASK_ANY);

    const std::array<uint32_t, 15> keysyms = {
        XK_Return, XK_q, XK_Escape, XK_m, XK_f, XK_g, XK_Tab, XK_1, XK_2,
        XK_Left, XK_Right, XK_Up, XK_Down, XK_space, XK_a
    };
    const std::array<uint16_t, 8> mod_variants = {
        XCB_MOD_MASK_4,
        static_cast<uint16_t>(XCB_MOD_MASK_4 | XCB_MOD_MASK_LOCK),
        static_cast<uint16_t>(XCB_MOD_MASK_4 | numlock_mask_),
        static_cast<uint16_t>(XCB_MOD_MASK_4 | XCB_MOD_MASK_LOCK | numlock_mask_),
        static_cast<uint16_t>(XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT),
        static_cast<uint16_t>(XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_LOCK),
        static_cast<uint16_t>(XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT | numlock_mask_),
        static_cast<uint16_t>(XCB_MOD_MASK_4 | XCB_MOD_MASK_SHIFT | XCB_MOD_MASK_LOCK | numlock_mask_),
    };

    for (uint32_t ks : keysyms) {
        UniqueMalloc<xcb_keycode_t> codes(xcb_key_symbols_get_keycode(keysyms_, ks));
        if (!codes) continue;
        for (xcb_keycode_t* kc = codes.get(); *kc != XCB_NO_SYMBOL; ++kc) {
            for (uint16_t mod : mod_variants) {
                xcb_grab_key(conn_, 1, screen_->root, mod, *kc,
                             XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
            }
        }
    }

    const std::array<uint16_t, 4> bare_mods = {
        0,
        XCB_MOD_MASK_LOCK,
        numlock_mask_,
        static_cast<uint16_t>(XCB_MOD_MASK_LOCK | numlock_mask_),
    };
    for (uint32_t ks : {static_cast<uint32_t>(XK_Super_L), static_cast<uint32_t>(XK_Super_R)}) {
        UniqueMalloc<xcb_keycode_t> codes(xcb_key_symbols_get_keycode(keysyms_, ks));
        if (!codes) continue;
        for (xcb_keycode_t* kc = codes.get(); *kc != XCB_NO_SYMBOL; ++kc) {
            for (uint16_t mod : bare_mods) {
                xcb_grab_key(conn_, 1, screen_->root, mod, *kc,
                             XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
            }
        }
    }

    // Standard Alt+Tab (window cycle), Alt+F4 (close window), and Alt+F2 (run menu)
    for (uint32_t ks : {static_cast<uint32_t>(XK_Tab), static_cast<uint32_t>(XK_F4),
                        static_cast<uint32_t>(XK_F2)}) {
        UniqueMalloc<xcb_keycode_t> codes(xcb_key_symbols_get_keycode(keysyms_, ks));
        if (!codes) continue;
        for (xcb_keycode_t* kc = codes.get(); *kc != XCB_NO_SYMBOL; ++kc) {
            for (uint16_t mod : bare_mods) {
                xcb_grab_key(conn_, 1, screen_->root,
                             static_cast<uint16_t>(XCB_MOD_MASK_1 | mod), *kc,
                             XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
            }
        }
    }
}

void WindowManager::grab_client_buttons(xcb_window_t win) {
    const std::array<uint16_t, 4> extra_masks = {
        0,
        XCB_MOD_MASK_LOCK,
        numlock_mask_,
        static_cast<uint16_t>(XCB_MOD_MASK_LOCK | numlock_mask_),
    };

    for (uint16_t extra : extra_masks) {
        xcb_grab_button(conn_, 0, win,
                        XCB_EVENT_MASK_BUTTON_PRESS,
                        XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                        XCB_WINDOW_NONE, XCB_CURSOR_NONE,
                        XCB_BUTTON_INDEX_1, extra);
        xcb_grab_button(conn_, 0, win,
                        XCB_EVENT_MASK_BUTTON_PRESS,
                        XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
                        XCB_WINDOW_NONE, XCB_CURSOR_NONE,
                        XCB_BUTTON_INDEX_3, extra);

        xcb_grab_button(conn_, 0, win,
                        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                            XCB_EVENT_MASK_POINTER_MOTION,
                        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                        XCB_WINDOW_NONE, XCB_CURSOR_NONE,
                        XCB_BUTTON_INDEX_1, static_cast<uint16_t>(XCB_MOD_MASK_4 | extra));

        xcb_grab_button(conn_, 0, win,
                        XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                            XCB_EVENT_MASK_POINTER_MOTION,
                        XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                        XCB_WINDOW_NONE, XCB_CURSOR_NONE,
                        XCB_BUTTON_INDEX_3, static_cast<uint16_t>(XCB_MOD_MASK_4 | extra));
    }
}

void WindowManager::scan_existing_windows() {
    auto tree_cookie = xcb_query_tree(conn_, screen_->root);
    UniqueMalloc<xcb_query_tree_reply_t> tree(
        xcb_query_tree_reply(conn_, tree_cookie, nullptr));
    if (!tree) return;

    xcb_window_t* children = xcb_query_tree_children(tree.get());
    const int len = xcb_query_tree_children_length(tree.get());

    std::vector<xcb_window_t> normal_candidates;
    for (int i = 0; i < len; ++i) {
        xcb_window_t w = children[i];
        if (w == wm_check_win_) continue;

        auto attr_cookie = xcb_get_window_attributes(conn_, w);
        UniqueMalloc<xcb_get_window_attributes_reply_t> attr(
            xcb_get_window_attributes_reply(conn_, attr_cookie, nullptr));
        if (!attr || attr->override_redirect || attr->map_state != XCB_MAP_STATE_VIEWABLE) {
            continue;
        }
        if (is_dock_window(w)) {
            manage_dock(w);
        } else {
            normal_candidates.push_back(w);
        }
    }

    for (xcb_window_t w : normal_candidates) {
        manage_client(w);
    }
}

int WindowManager::run() {
    running_ = 1;
    restart_requested_ = 0;
    const int xfd = xcb_get_file_descriptor(conn_);
    std::time_t last_panel_check = std::time(nullptr);

    while (running_) {
        UniqueMalloc<xcb_generic_event_t> pending_motion;
        while (xcb_generic_event_t* raw_ev = xcb_poll_for_event(conn_)) {
            if ((raw_ev->response_type & ~0x80) == XCB_MOTION_NOTIFY) {
                pending_motion.reset(raw_ev);
                continue;
            }
            if (pending_motion) {
                handle_event(pending_motion.get());
                pending_motion.reset();
            }
            UniqueMalloc<xcb_generic_event_t> ev(raw_ev);
            handle_event(ev.get());
        }
        if (pending_motion) {
            handle_event(pending_motion.get());
        }
        xcb_flush(conn_);

        if (xcb_connection_has_error(conn_)) {
            std::cerr << "lunar-wm: X connection lost\n";
            return 1;
        }
        if (!running_) break;

        if (docks_.empty() && running_ && !restart_requested_) {
            const std::time_t now = std::time(nullptr);
            if (now - last_panel_check >= 2) {
                last_panel_check = now;
                if (!is_panel_alive()) {
                    ensure_panel_alive();
                }
            }
        }

        struct pollfd pfd{};
        pfd.fd = xfd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 250);
        if (ret < 0 && errno != EINTR) {
            break;
        }
    }
    if (!restart_requested_) {
        terminate_session();
    }
    return 0;
}

void WindowManager::handle_event(xcb_generic_event_t* event) {
    const uint8_t type = event->response_type & ~0x80;

    if (randr_available_ && type == randr_first_event_ + XCB_RANDR_SCREEN_CHANGE_NOTIFY) {
        on_randr_screen_change();
        return;
    }

    switch (type) {
    case 0: {
        const auto* err = reinterpret_cast<const xcb_generic_error_t*>(event);
        if (err->error_code != XCB_WINDOW && err->error_code != XCB_DRAWABLE &&
            err->error_code != XCB_MATCH) {
            std::cerr << "lunar-wm: X11 error code=" << static_cast<int>(err->error_code)
                      << " major=" << static_cast<int>(err->major_code)
                      << " minor=" << static_cast<int>(err->minor_code) << '\n';
        }
        break;
    }
    case XCB_MAP_REQUEST:
        on_map_request(reinterpret_cast<const xcb_map_request_event_t*>(event));
        break;
    case XCB_CONFIGURE_REQUEST:
        on_configure_request(reinterpret_cast<const xcb_configure_request_event_t*>(event));
        break;
    case XCB_DESTROY_NOTIFY:
        on_destroy_notify(reinterpret_cast<const xcb_destroy_notify_event_t*>(event));
        break;
    case XCB_UNMAP_NOTIFY:
        on_unmap_notify(reinterpret_cast<const xcb_unmap_notify_event_t*>(event));
        break;
    case XCB_ENTER_NOTIFY:
        on_enter_notify(reinterpret_cast<const xcb_enter_notify_event_t*>(event));
        break;
    case XCB_LEAVE_NOTIFY:
        on_leave_notify(reinterpret_cast<const xcb_leave_notify_event_t*>(event));
        break;
    case XCB_EXPOSE:
        on_expose(reinterpret_cast<const xcb_expose_event_t*>(event));
        break;
    case XCB_BUTTON_PRESS:
        on_button_press(reinterpret_cast<const xcb_button_press_event_t*>(event));
        break;
    case XCB_MOTION_NOTIFY:
        on_motion_notify(reinterpret_cast<const xcb_motion_notify_event_t*>(event));
        break;
    case XCB_BUTTON_RELEASE:
        on_button_release(reinterpret_cast<const xcb_button_release_event_t*>(event));
        break;
    case XCB_KEY_PRESS:
        on_key_press(reinterpret_cast<const xcb_key_press_event_t*>(event));
        break;
    case XCB_KEY_RELEASE:
        on_key_release(reinterpret_cast<const xcb_key_release_event_t*>(event));
        break;
    case XCB_PROPERTY_NOTIFY:
        on_property_notify(reinterpret_cast<const xcb_property_notify_event_t*>(event));
        break;
    case XCB_CLIENT_MESSAGE:
        on_client_message(reinterpret_cast<const xcb_client_message_event_t*>(event));
        break;
    case XCB_CONFIGURE_NOTIFY:
        on_configure_notify(reinterpret_cast<const xcb_configure_notify_event_t*>(event));
        break;
    case XCB_MAPPING_NOTIFY: {
        auto* map_ev = reinterpret_cast<xcb_mapping_notify_event_t*>(event);
        if (keysyms_) {
            xcb_refresh_keyboard_mapping(keysyms_, map_ev);
            grab_keys();
        }
        break;
    }
    default:
        break;
    }
}

bool WindowManager::is_dock_window(xcb_window_t win) const {
    if (!ewmh_initialized_) return false;
    xcb_ewmh_get_atoms_reply_t types{};
    auto cookie = xcb_ewmh_get_wm_window_type(&ewmh_, win);
    if (xcb_ewmh_get_wm_window_type_reply(&ewmh_, cookie, &types, nullptr)) {
        bool dock = false;
        for (uint32_t i = 0; i < types.atoms_len; ++i) {
            if (types.atoms[i] == ewmh_._NET_WM_WINDOW_TYPE_DOCK) {
                dock = true;
                break;
            }
        }
        xcb_ewmh_get_atoms_reply_wipe(&types);
        if (dock) return true;
    }
    Strut s{};
    return read_window_strut(win, s);
}

bool WindowManager::should_decorate(xcb_window_t win) const {
    if (ewmh_initialized_) {
        xcb_ewmh_get_atoms_reply_t types{};
        auto cookie = xcb_ewmh_get_wm_window_type(&ewmh_, win);
        if (xcb_ewmh_get_wm_window_type_reply(&ewmh_, cookie, &types, nullptr)) {
            bool suppress = false;
            for (uint32_t i = 0; i < types.atoms_len; ++i) {
                const xcb_atom_t t = types.atoms[i];
                if (t == ewmh_._NET_WM_WINDOW_TYPE_SPLASH ||
                    t == ewmh_._NET_WM_WINDOW_TYPE_UTILITY ||
                    t == ewmh_._NET_WM_WINDOW_TYPE_NOTIFICATION ||
                    t == ewmh_._NET_WM_WINDOW_TYPE_TOOLTIP ||
                    t == ewmh_._NET_WM_WINDOW_TYPE_DROPDOWN_MENU ||
                    t == ewmh_._NET_WM_WINDOW_TYPE_POPUP_MENU) {
                    suppress = true;
                    break;
                }
            }
            xcb_ewmh_get_atoms_reply_wipe(&types);
            if (suppress) return false;
        }
    }

    if (gtk_frame_extents_atom_ != XCB_ATOM_NONE) {
        auto gc = xcb_get_property(conn_, 0, win, gtk_frame_extents_atom_, XCB_ATOM_CARDINAL, 0, 4);
        UniqueMalloc<xcb_get_property_reply_t> gr(xcb_get_property_reply(conn_, gc, nullptr));
        if (gr && xcb_get_property_value_length(gr.get()) >= 16) {
            const auto* vals = static_cast<const uint32_t*>(xcb_get_property_value(gr.get()));
            if (vals[0] > 0 || vals[1] > 0 || vals[2] > 0 || vals[3] > 0) {
                return false;
            }
        }
    }

    if (motif_wm_hints_atom_ != XCB_ATOM_NONE) {
        auto mc = xcb_get_property(conn_, 0, win, motif_wm_hints_atom_, XCB_GET_PROPERTY_TYPE_ANY, 0, 5);
        UniqueMalloc<xcb_get_property_reply_t> mr(xcb_get_property_reply(conn_, mc, nullptr));
        if (mr && xcb_get_property_value_length(mr.get()) >= 12) {
            const auto* hints = static_cast<const uint32_t*>(xcb_get_property_value(mr.get()));
            constexpr uint32_t kMwmHintsDecorations = 1u << 1;
            if ((hints[0] & kMwmHintsDecorations) && hints[2] == 0) {
                return false;
            }
        }
    }
    return true;
}

bool WindowManager::read_window_strut(xcb_window_t win, Strut& out) const {
    if (!ewmh_initialized_) return false;

    xcb_ewmh_wm_strut_partial_t sp{};
    auto partial_cookie = xcb_ewmh_get_wm_strut_partial(&ewmh_, win);
    if (xcb_ewmh_get_wm_strut_partial_reply(&ewmh_, partial_cookie, &sp, nullptr)) {
        out.left = sp.left;
        out.right = sp.right;
        out.top = sp.top;
        out.bottom = sp.bottom;
        out.left_start_y = sp.left_start_y;
        out.left_end_y = sp.left_end_y;
        out.right_start_y = sp.right_start_y;
        out.right_end_y = sp.right_end_y;
        out.top_start_x = sp.top_start_x;
        out.top_end_x = sp.top_end_x;
        out.bottom_start_x = sp.bottom_start_x;
        out.bottom_end_x = sp.bottom_end_x;
        return (out.left || out.right || out.top || out.bottom);
    }

    xcb_ewmh_get_extents_reply_t ext{};
    auto strut_cookie = xcb_ewmh_get_wm_strut(&ewmh_, win);
    if (xcb_ewmh_get_wm_strut_reply(&ewmh_, strut_cookie, &ext, nullptr)) {
        out.left = ext.left;
        out.right = ext.right;
        out.top = ext.top;
        out.bottom = ext.bottom;
        return (out.left || out.right || out.top || out.bottom);
    }
    return false;
}

std::string WindowManager::read_window_title(xcb_window_t win) const {
    if (ewmh_initialized_) {
        xcb_ewmh_get_utf8_strings_reply_t name_reply{};
        auto cookie = xcb_ewmh_get_wm_name(&ewmh_, win);
        if (xcb_ewmh_get_wm_name_reply(&ewmh_, cookie, &name_reply, nullptr)) {
            std::string t(name_reply.strings, name_reply.strings_len);
            xcb_ewmh_get_utf8_strings_reply_wipe(&name_reply);
            if (!t.empty()) return t;
        }
    }
    xcb_icccm_get_text_property_reply_t icccm_name{};
    auto icccm_cookie = xcb_icccm_get_wm_name(conn_, win);
    if (xcb_icccm_get_wm_name_reply(conn_, icccm_cookie, &icccm_name, nullptr)) {
        std::string t(icccm_name.name, icccm_name.name_len);
        xcb_icccm_get_text_property_reply_wipe(&icccm_name);
        if (!t.empty()) return t;
    }
    return "Window";
}

uint16_t WindowManager::titlebar_height_for(const ClientWindow& cw) const noexcept {
    return (cw.decorated && !cw.fullscreen) ? kTitlebarHeight : 0;
}

int WindowManager::hit_test_titlebar_button(const ClientWindow& cw, int16_t x, int16_t y) const {
    if (!cw.decorated || cw.fullscreen) return 0;
    if (y < 0 || y >= kTitlebarHeight) return 0;

    const int close_x = static_cast<int>(cw.rect.width) - kBtnRightPad - kBtnSize;
    const int max_x = close_x - kBtnGap - kBtnSize;
    const int min_x = max_x - kBtnGap - kBtnSize;

    if (x >= close_x - 3 && x <= close_x + kBtnSize + 3) return 3;
    if (x >= max_x - 3 && x <= max_x + kBtnSize + 3) return 2;
    if (x >= min_x - 3 && x <= min_x + kBtnSize + 3) return 1;
    return 0;
}

void WindowManager::draw_titlebar(ClientWindow& cw) {
    if (!cw.decorated || cw.fullscreen || cw.frame == XCB_WINDOW_NONE ||
        cw.title_gc == XCB_NONE || cw.rect.width == 0) {
        return;
    }

    const bool focused = (cw.window == focused_window_);
    ImageBuffer bar;
    bar.width = cw.rect.width;
    bar.height = kTitlebarHeight;
    bar.pixels.assign(static_cast<size_t>(bar.width) * bar.height,
                      focused ? 0x1a202c : 0x141820);

    // Subtle bottom divider
    fill_rect_alpha(bar, 0, kTitlebarHeight - 1, bar.width, 1, 0xffffff, 0.08f);

    // Title text
    const int close_x = static_cast<int>(cw.rect.width) - kBtnRightPad - kBtnSize;
    const int max_x = close_x - kBtnGap - kBtnSize;
    const int min_x = max_x - kBtnGap - kBtnSize;

    if (has_title_font_ && min_x > 36) {
        std::string t = cw.title.empty() ? "Window" : cw.title;
        const int max_text_w = min_x - 24;
        while (t.size() > 3 && title_font_.measure_text(t, 12) > max_text_w) {
            t.pop_back();
            if (t.size() > 3) t.replace(t.size() - 3, 3, "...");
        }
        title_font_.draw_text(bar, 14, kTitlebarHeight / 2 + 4, t, 12,
                              focused ? 0xf1f5f9 : 0x94a3b8);
    }

    const float cy = static_cast<float>(kTitlebarHeight) * 0.5f;
    const float r = static_cast<float>(kBtnSize) * 0.5f;

    // Minimize button (-)
    {
        const float cx = static_cast<float>(min_x) + r;
        const uint32_t bg = (cw.hover_btn == 1) ? 0x334155 : 0x252c3a;
        fill_circle_aa(bar, cx, cy, r, bg, 1.0f);
        draw_line_aa(bar, cx - 4.0f, cy, cx + 4.0f, cy, 1.5f, 0xe2e8f0, 0.92f);
    }

    // Maximize button ([])
    {
        const float cx = static_cast<float>(max_x) + r;
        const uint32_t bg = (cw.hover_btn == 2) ? 0x2563eb : 0x252c3a;
        fill_circle_aa(bar, cx, cy, r, bg, 1.0f);
        stroke_rounded_rect_aa(bar, cx - 4.0f, cy - 4.0f, 8.0f, 8.0f, 1.5f, 1.4f, 0xe2e8f0, 0.92f);
    }

    // Close button (X)
    {
        const float cx = static_cast<float>(close_x) + r;
        const uint32_t bg = (cw.hover_btn == 3) ? 0xef4444 : 0x3b232a;
        fill_circle_aa(bar, cx, cy, r, bg, 1.0f);
        const uint32_t fg = (cw.hover_btn == 3) ? 0xffffff : 0xfca5a5;
        draw_line_aa(bar, cx - 3.6f, cy - 3.6f, cx + 3.6f, cy + 3.6f, 1.6f, fg, 0.95f);
        draw_line_aa(bar, cx + 3.6f, cy - 3.6f, cx - 3.6f, cy + 3.6f, 1.6f, fg, 0.95f);
    }

    xcb_pixmap_t pxm = xcb_generate_id(conn_);
    xcb_create_pixmap(conn_, screen_->root_depth, pxm, cw.frame, bar.width, bar.height);
    upload_pixels_to_pixmap(conn_, screen_, pxm, bar.width, bar.height, bar.pixels);
    xcb_copy_area(conn_, pxm, cw.frame, cw.title_gc, 0, 0, 0, 0, bar.width, bar.height);
    xcb_free_pixmap(conn_, pxm);
}

void WindowManager::redraw_all_titlebars() {
    for (auto& [win, cw] : clients_) {
        (void)win;
        if (cw.decorated && !cw.fullscreen && cw.mapped) {
            draw_titlebar(cw);
        }
    }
}

Rect WindowManager::workarea_for_monitor(const Rect& mon) const noexcept {
    int x0 = mon.x;
    int y0 = mon.y;
    int x1 = mon.x + mon.width;
    int y1 = mon.y + mon.height;

    for (const auto& [win, strut] : docks_) {
        (void)win;
        if (strut.top > 0) {
            const bool overlaps_x = (strut.top_end_x == 0) ||
                (static_cast<int>(strut.top_start_x) < x1 && static_cast<int>(strut.top_end_x) >= x0);
            if (overlaps_x) {
                y0 = std::max(y0, static_cast<int>(strut.top));
            }
        }
        if (strut.bottom > 0) {
            const bool overlaps_x = (strut.bottom_end_x == 0) ||
                (static_cast<int>(strut.bottom_start_x) < x1 && static_cast<int>(strut.bottom_end_x) >= x0);
            if (overlaps_x) {
                y1 = std::min(y1, static_cast<int>(screen_height_) - static_cast<int>(strut.bottom));
            }
        }
        if (strut.left > 0) {
            const bool overlaps_y = (strut.left_end_y == 0) ||
                (static_cast<int>(strut.left_start_y) < y1 && static_cast<int>(strut.left_end_y) >= y0);
            if (overlaps_y) {
                x0 = std::max(x0, static_cast<int>(strut.left));
            }
        }
        if (strut.right > 0) {
            const bool overlaps_y = (strut.right_end_y == 0) ||
                (static_cast<int>(strut.right_start_y) < y1 && static_cast<int>(strut.right_end_y) >= y0);
            if (overlaps_y) {
                x1 = std::min(x1, static_cast<int>(screen_width_) - static_cast<int>(strut.right));
            }
        }
    }

    return Rect{
        static_cast<int16_t>(x0),
        static_cast<int16_t>(y0),
        static_cast<uint16_t>(std::max<int>(1, x1 - x0)),
        static_cast<uint16_t>(std::max<int>(1, y1 - y0))
    };
}

Rect WindowManager::workarea_for_rect(const Rect& r) const noexcept {
    if (monitors_.empty()) return workarea_;
    return workarea_for_monitor(monitor_for_rect(monitors_, r));
}

void WindowManager::recompute_workarea() {
    monitors_ = query_monitors(conn_, screen_);
    const Rect pmon = primary_monitor(monitors_).rect;
    workarea_ = workarea_for_monitor(pmon);

    if (ewmh_initialized_) {
        xcb_ewmh_geometry_t geom{
            static_cast<uint32_t>(workarea_.x),
            static_cast<uint32_t>(workarea_.y),
            static_cast<uint32_t>(workarea_.width),
            static_cast<uint32_t>(workarea_.height),
        };
        xcb_ewmh_set_workarea(&ewmh_, screen_idx_, 1, &geom);
    }

    for (auto& [win, client] : clients_) {
        const Rect mon = monitors_.empty() ? Rect{0, 0, screen_width_, screen_height_}
                                           : monitor_for_rect(monitors_, client.rect);
        const Rect mon_wa = workarea_for_monitor(mon);
        if (client.fullscreen) {
            client.rect = mon;
            configure_client_rect(win, client.rect, 0);
        } else if (client.maximized) {
            Rect max_rect{
                mon_wa.x,
                mon_wa.y,
                std::max<uint16_t>(kMinClientWidth, mon_wa.width),
                std::max<uint16_t>(kMinClientHeight, mon_wa.height)
            };
            client.rect = max_rect;
            configure_client_rect(win, max_rect, 0);
        } else {
            Rect clamped = client.rect;
            clamp_rect_to_workarea(clamped);
            if (clamped.x != client.rect.x || clamped.y != client.rect.y ||
                clamped.width != client.rect.width || clamped.height != client.rect.height) {
                client.rect = clamped;
                configure_client_rect(win, clamped);
            }
        }
    }
    raise_docks();
}

void WindowManager::clamp_rect_to_workarea(Rect& rect) const {
    const Rect wa = workarea_for_rect(rect);
    const int max_w = std::max<int>(kMinClientWidth,
                                    static_cast<int>(wa.width) - 2 * theme::kBorderWidth);
    const int max_h = std::max<int>(kMinClientHeight,
                                    static_cast<int>(wa.height) - 2 * theme::kBorderWidth);

    rect.width = static_cast<uint16_t>(std::clamp<int>(rect.width, kMinClientWidth, max_w));
    rect.height = static_cast<uint16_t>(std::clamp<int>(rect.height, kMinClientHeight, max_h));

    const int min_x = wa.x;
    const int min_y = wa.y;
    const int max_x = wa.x + wa.width - (rect.width + 2 * theme::kBorderWidth);
    const int max_y = wa.y + wa.height - (rect.height + 2 * theme::kBorderWidth);

    rect.x = static_cast<int16_t>(std::clamp<int>(rect.x, min_x, std::max(min_x, max_x)));
    rect.y = static_cast<int16_t>(std::clamp<int>(rect.y, min_y, std::max(min_y, max_y)));
}

void WindowManager::set_icccm_wm_state(xcb_window_t win, uint32_t state) {
    if (wm_state_atom_ == XCB_ATOM_NONE) return;
    const uint32_t wm_state_data[2] = {state, XCB_WINDOW_NONE};
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, win, wm_state_atom_,
                        wm_state_atom_, 32, 2, wm_state_data);
}

void WindowManager::send_configure_notify(const ClientWindow& cw) {
    const uint16_t title_h = titlebar_height_for(cw);
    const uint16_t client_h = (cw.rect.height > title_h)
                                  ? static_cast<uint16_t>(cw.rect.height - title_h)
                                  : 1;
    xcb_configure_notify_event_t ev{};
    ev.response_type = XCB_CONFIGURE_NOTIFY;
    ev.event = cw.window;
    ev.window = cw.window;
    ev.above_sibling = XCB_WINDOW_NONE;
    ev.x = cw.rect.x;
    ev.y = static_cast<int16_t>(cw.rect.y + title_h);
    ev.width = cw.rect.width;
    ev.height = client_h;
    ev.border_width = 0;
    ev.override_redirect = 0;
    xcb_send_event(conn_, 0, cw.window, XCB_EVENT_MASK_STRUCTURE_NOTIFY,
                   reinterpret_cast<const char*>(&ev));
}

void WindowManager::configure_client_rect(xcb_window_t win, const Rect& rect, uint16_t border_w) {
    auto it = clients_.find(win);
    if (it == clients_.end()) return;
    ClientWindow& cw = it->second;

    const uint16_t title_h = titlebar_height_for(cw);
    const uint16_t client_h = (rect.height > title_h)
                                  ? static_cast<uint16_t>(rect.height - title_h)
                                  : 1;

    if (cw.frame != XCB_WINDOW_NONE) {
        const uint32_t frame_vals[] = {
            static_cast<uint32_t>(static_cast<int32_t>(rect.x)),
            static_cast<uint32_t>(static_cast<int32_t>(rect.y)),
            static_cast<uint32_t>(rect.width),
            static_cast<uint32_t>(rect.height),
            static_cast<uint32_t>(border_w)
        };
        const uint16_t mask = XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                              XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                              XCB_CONFIG_WINDOW_BORDER_WIDTH;
        xcb_configure_window(conn_, cw.frame, mask, frame_vals);

        const uint32_t client_vals[] = {
            0u,
            static_cast<uint32_t>(title_h),
            static_cast<uint32_t>(rect.width),
            static_cast<uint32_t>(client_h),
            0u
        };
        xcb_configure_window(conn_, win, mask, client_vals);

        if (border_w == 0) {
            apply_rounded_shape(conn_, cw.frame, rect.width, rect.height, 0);
        } else {
            apply_rounded_shape(conn_, cw.frame,
                                static_cast<uint16_t>(rect.width + 2 * border_w),
                                static_cast<uint16_t>(rect.height + 2 * border_w),
                                12, static_cast<int16_t>(-static_cast<int16_t>(border_w)));
        }
        draw_titlebar(cw);
        send_configure_notify(cw);
    }
}

void WindowManager::raise_docks() {
    const uint32_t stack_mode = XCB_STACK_MODE_ABOVE;
    for (const auto& [dock_win, strut] : docks_) {
        (void)strut;
        xcb_configure_window(conn_, dock_win, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
    }
    for (xcb_window_t w : client_order_) {
        auto it = clients_.find(w);
        if (it != clients_.end() && it->second.fullscreen &&
            it->second.desktop == current_desktop_ && it->second.mapped) {
            const xcb_window_t target =
                (it->second.frame != XCB_WINDOW_NONE) ? it->second.frame : w;
            xcb_configure_window(conn_, target, XCB_CONFIG_WINDOW_STACK_MODE, &stack_mode);
        }
    }
}

void WindowManager::manage_dock(xcb_window_t win) {
    Strut strut{};
    read_window_strut(win, strut);
    docks_[win] = strut;

    const uint32_t ev_mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(conn_, win, XCB_CW_EVENT_MASK, &ev_mask);

    const uint32_t bw = 0;
    xcb_configure_window(conn_, win, XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);
    xcb_map_window(conn_, win);

    recompute_workarea();
}

void WindowManager::manage_client(xcb_window_t win) {
    if (clients_.contains(win) || docks_.contains(win) || frame_to_client_.contains(win)) return;

    auto geom_cookie = xcb_get_geometry(conn_, win);
    UniqueMalloc<xcb_get_geometry_reply_t> geom(
        xcb_get_geometry_reply(conn_, geom_cookie, nullptr));

    const bool decorated = should_decorate(win);
    const uint16_t title_h = decorated ? kTitlebarHeight : 0;

    Rect rect{};
    if (geom) {
        rect.x = geom->x;
        rect.y = geom->y;
        rect.width = geom->width > 0 ? geom->width : kDefaultClientWidth;
        rect.height = static_cast<uint16_t>(
            (geom->height > 0 ? geom->height : kDefaultClientHeight) + title_h);
    } else {
        rect.width = kDefaultClientWidth;
        rect.height = static_cast<uint16_t>(kDefaultClientHeight + title_h);
    }

    if ((rect.x <= 0 && rect.y <= 0) ||
        rect.x >= static_cast<int>(screen_width_) || rect.y >= static_cast<int>(screen_height_)) {
        const int16_t step = static_cast<int16_t>((cascade_offset_ % kMaxCascadeSteps) * kCascadeStepPx);
        ++cascade_offset_;
        rect.x = static_cast<int16_t>(workarea_.x + 120 + step);
        rect.y = static_cast<int16_t>(workarea_.y + 36 + step);
    }
    clamp_rect_to_workarea(rect);

    const xcb_window_t frame = xcb_generate_id(conn_);
    const uint32_t frame_mask = XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_EVENT_MASK;
    const uint32_t frame_vals[] = {
        0x1a202c,
        theme::kBorderUnfocused,
        XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
            XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
            XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION |
            XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW
    };
    xcb_create_window(conn_, XCB_COPY_FROM_PARENT, frame, screen_->root,
                      rect.x, rect.y, rect.width, rect.height, theme::kBorderWidth,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen_->root_visual,
                      frame_mask, frame_vals);

    const xcb_gcontext_t title_gc = xcb_generate_id(conn_);
    const uint32_t gc_vals[] = {0xf1f5f9, 0x1a202c, 0};
    xcb_create_gc(conn_, title_gc, frame,
                  XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES, gc_vals);

    xcb_change_save_set(conn_, XCB_SET_MODE_INSERT, win);
    const uint32_t client_ev_mask =
        XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_FOCUS_CHANGE |
        XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(conn_, win, XCB_CW_EVENT_MASK, &client_ev_mask);

    xcb_reparent_window(conn_, win, frame, 0, static_cast<int16_t>(title_h));

    ClientWindow cw;
    cw.window = win;
    cw.frame = frame;
    cw.title_gc = title_gc;
    cw.rect = rect;
    cw.saved_rect = rect;
    cw.title = read_window_title(win);
    cw.desktop = current_desktop_;
    cw.decorated = decorated;
    cw.mapped = true;
    clients_[win] = cw;
    frame_to_client_[frame] = win;
    client_order_.push_back(win);

    configure_client_rect(win, rect, theme::kBorderWidth);
    grab_client_buttons(win);
    xcb_map_window(conn_, win);
    xcb_map_window(conn_, frame);

    set_icccm_wm_state(win, 1 /*NormalState*/);

    if (ewmh_initialized_) {
        xcb_ewmh_set_wm_desktop(&ewmh_, win, cw.desktop);
        xcb_ewmh_get_atoms_reply_t states{};
        auto st_cookie = xcb_ewmh_get_wm_state(&ewmh_, win);
        if (xcb_ewmh_get_wm_state_reply(&ewmh_, st_cookie, &states, nullptr)) {
            bool want_fs = false;
            bool want_max = false;
            for (uint32_t i = 0; i < states.atoms_len; ++i) {
                if (states.atoms[i] == ewmh_._NET_WM_STATE_FULLSCREEN) {
                    want_fs = true;
                } else if (states.atoms[i] == ewmh_._NET_WM_STATE_MAXIMIZED_HORZ ||
                           states.atoms[i] == ewmh_._NET_WM_STATE_MAXIMIZED_VERT) {
                    want_max = true;
                }
            }
            xcb_ewmh_get_atoms_reply_wipe(&states);
            if (want_fs) {
                set_fullscreen(win, true);
            } else if (want_max) {
                set_maximized(win, true);
            }
        }
    }

    update_client_list();
    focus_window(win);
    raise_docks();
}

void WindowManager::unmanage_window(xcb_window_t win, bool destroyed) {
    if (docks_.erase(win) > 0) {
        recompute_workarea();
        return;
    }

    auto it = clients_.find(win);
    if (it == clients_.end()) {
        return;
    }

    const xcb_window_t frame = it->second.frame;
    const xcb_gcontext_t title_gc = it->second.title_gc;
    const Rect rect = it->second.rect;
    clients_.erase(it);
    if (frame != XCB_WINDOW_NONE) {
        frame_to_client_.erase(frame);
        if (!destroyed) {
            xcb_grab_server(conn_);
            const uint32_t zero_mask = 0;
            xcb_change_window_attributes(conn_, win, XCB_CW_EVENT_MASK, &zero_mask);
            xcb_ungrab_button(conn_, XCB_BUTTON_INDEX_ANY, win, XCB_MOD_MASK_ANY);
            xcb_unmap_window(conn_, win);
            set_icccm_wm_state(win, 0 /*WithdrawnState*/);
            xcb_reparent_window(conn_, win, screen_->root, rect.x, rect.y);
            xcb_change_save_set(conn_, XCB_SET_MODE_DELETE, win);
            xcb_destroy_window(conn_, frame);
            xcb_ungrab_server(conn_);
            xcb_flush(conn_);
        } else {
            xcb_destroy_window(conn_, frame);
        }
        if (title_gc != XCB_NONE) {
            xcb_free_gc(conn_, title_gc);
        }
    }
    ignore_unmap_.erase(win);

    if (drag_window_ == win && drag_mode_ != DragMode::None) {
        xcb_ungrab_pointer(conn_, XCB_CURRENT_TIME);
        drag_mode_ = DragMode::None;
        drag_window_ = XCB_WINDOW_NONE;
    }

    std::erase(client_order_, win);
    update_client_list();

    if (focused_window_ == win) {
        focused_window_ = XCB_WINDOW_NONE;
        xcb_window_t next_focus = XCB_WINDOW_NONE;
        for (auto rit = client_order_.rbegin(); rit != client_order_.rend(); ++rit) {
            auto cit = clients_.find(*rit);
            if (cit != clients_.end() && cit->second.desktop == current_desktop_ &&
                !cit->second.minimized) {
                next_focus = *rit;
                break;
            }
        }
        if (next_focus != XCB_WINDOW_NONE) {
            focus_window(next_focus);
        } else {
            xcb_set_input_focus(conn_, XCB_INPUT_FOCUS_POINTER_ROOT,
                                screen_->root, XCB_CURRENT_TIME);
            if (ewmh_initialized_) {
                xcb_ewmh_set_active_window(&ewmh_, screen_idx_, XCB_WINDOW_NONE);
            }
        }
    }
}

void WindowManager::on_map_request(const xcb_map_request_event_t* ev) {
    if (frame_to_client_.contains(ev->window)) return;

    auto attr_cookie = xcb_get_window_attributes(conn_, ev->window);
    UniqueMalloc<xcb_get_window_attributes_reply_t> attr(
        xcb_get_window_attributes_reply(conn_, attr_cookie, nullptr));
    if (attr && attr->override_redirect) {
        xcb_map_window(conn_, ev->window);
        return;
    }

    if (is_dock_window(ev->window)) {
        manage_dock(ev->window);
    } else if (clients_.contains(ev->window)) {
        restore_window(ev->window);
    } else {
        manage_client(ev->window);
    }
}

void WindowManager::on_configure_request(const xcb_configure_request_event_t* ev) {
    auto it = clients_.find(ev->window);
    if (it == clients_.end()) {
        uint32_t values[7];
        uint32_t idx = 0;
        if (ev->value_mask & XCB_CONFIG_WINDOW_X) values[idx++] = static_cast<uint32_t>(ev->x);
        if (ev->value_mask & XCB_CONFIG_WINDOW_Y) values[idx++] = static_cast<uint32_t>(ev->y);
        if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH) values[idx++] = ev->width;
        if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT) values[idx++] = ev->height;
        if (ev->value_mask & XCB_CONFIG_WINDOW_BORDER_WIDTH) values[idx++] = ev->border_width;
        if (ev->value_mask & XCB_CONFIG_WINDOW_SIBLING) values[idx++] = ev->sibling;
        if (ev->value_mask & XCB_CONFIG_WINDOW_STACK_MODE) values[idx++] = ev->stack_mode;
        xcb_configure_window(conn_, ev->window, ev->value_mask, values);
        if (docks_.contains(ev->window)) {
            raise_docks();
        }
        return;
    }

    ClientWindow& cw = it->second;
    if (cw.fullscreen) {
        configure_client_rect(ev->window, cw.rect, 0);
        raise_docks();
        return;
    }

    const uint16_t title_h = titlebar_height_for(cw);
    Rect next = cw.rect;
    if (ev->value_mask & XCB_CONFIG_WINDOW_X) next.x = ev->x;
    if (ev->value_mask & XCB_CONFIG_WINDOW_Y) next.y = ev->y;
    if (ev->value_mask & XCB_CONFIG_WINDOW_WIDTH) next.width = ev->width;
    if (ev->value_mask & XCB_CONFIG_WINDOW_HEIGHT) {
        next.height = static_cast<uint16_t>(ev->height + title_h);
    }

    if (cw.maximized) {
        next = cw.rect;
    } else {
        clamp_rect_to_workarea(next);
    }
    cw.rect = next;
    configure_client_rect(ev->window, next);
    raise_docks();
}

void WindowManager::on_destroy_notify(const xcb_destroy_notify_event_t* ev) {
    unmanage_window(ev->window, true);
}

void WindowManager::on_unmap_notify(const xcb_unmap_notify_event_t* ev) {
    if (ev->from_configure) return;
    if (frame_to_client_.contains(ev->window)) return;
    auto it = clients_.find(ev->window);
    if (it == clients_.end()) return;
    const bool is_synthetic = (ev->response_type & 0x80) != 0;
    if (!is_synthetic && ev->event != it->second.frame) return;

    auto ig_it = ignore_unmap_.find(ev->window);
    if (ig_it != ignore_unmap_.end() && ig_it->second > 0) {
        --ig_it->second;
        if (ig_it->second == 0) {
            ignore_unmap_.erase(ig_it);
        }
        return;
    }
    unmanage_window(ev->window, false);
}

void WindowManager::on_enter_notify(const xcb_enter_notify_event_t* ev) {
    if (drag_mode_ != DragMode::None) return;
    if (ev->mode != XCB_NOTIFY_MODE_NORMAL || ev->detail == XCB_NOTIFY_DETAIL_INFERIOR) {
        return;
    }
    auto fit = frame_to_client_.find(ev->event);
    if (fit != frame_to_client_.end()) {
        focus_window(fit->second);
        return;
    }
    if (clients_.contains(ev->event)) {
        focus_window(ev->event);
    }
}

void WindowManager::on_leave_notify(const xcb_leave_notify_event_t* ev) {
    auto fit = frame_to_client_.find(ev->event);
    if (fit != frame_to_client_.end()) {
        auto cit = clients_.find(fit->second);
        if (cit != clients_.end() && cit->second.hover_btn != 0) {
            cit->second.hover_btn = 0;
            draw_titlebar(cit->second);
        }
    }
}

void WindowManager::on_expose(const xcb_expose_event_t* ev) {
    if (ev->count > 0) return;
    auto fit = frame_to_client_.find(ev->window);
    if (fit != frame_to_client_.end()) {
        auto cit = clients_.find(fit->second);
        if (cit != clients_.end()) {
            draw_titlebar(cit->second);
        }
    }
}

void WindowManager::on_button_press(const xcb_button_press_event_t* ev) {
    super_tap_pending_ = false;
    const xcb_window_t ev_win = ev->event;

    if (ev_win == screen_->root) {
        const int hit_idx = hit_test_desktop_icon(ev->root_x, ev->root_y);
        if (ev->detail == XCB_BUTTON_INDEX_3) {
            if (hit_idx >= 0) {
                selected_desktop_icon_ = hit_idx;
                apply_wallpaper();
            }
            send_panel_action(ipc::kPanelOpenContextMenu,
                              static_cast<uint32_t>(ev->root_x),
                              static_cast<uint32_t>(ev->root_y));
        } else if (ev->detail == XCB_BUTTON_INDEX_2 && hit_idx >= 0) {
            toggle_desktop_icon(app_registry_, desktop_icons_[hit_idx]);
            reload_desktop_icons();
        } else if (ev->detail == XCB_BUTTON_INDEX_1) {
            send_panel_action(ipc::kPanelDismissPopups);
            if (hit_idx >= 0) {
                const bool dbl = (last_desktop_click_idx_ == hit_idx &&
                                  (ev->time - last_desktop_click_time_) < 450);
                last_desktop_click_idx_ = hit_idx;
                last_desktop_click_time_ = ev->time;
                if (selected_desktop_icon_ != hit_idx) {
                    selected_desktop_icon_ = hit_idx;
                    apply_wallpaper();
                }
                if (dbl) {
                    last_desktop_click_idx_ = -1;
                    if (desktop_icons_[hit_idx].id == "zorin-appearance.desktop") {
                        send_panel_action(ipc::kPanelOpenAppearance);
                    } else {
                        AppRegistry::launch_app(desktop_icons_[hit_idx]);
                    }
                }
            } else if (selected_desktop_icon_ != -1) {
                selected_desktop_icon_ = -1;
                apply_wallpaper();
            }
        }
        return;
    }

    // Check if click landed on a window frame titlebar
    auto fit = frame_to_client_.find(ev_win);
    if (fit != frame_to_client_.end()) {
        const xcb_window_t cwin = fit->second;
        auto cit = clients_.find(cwin);
        if (cit == clients_.end()) return;
        ClientWindow& cw = cit->second;

        send_panel_action(ipc::kPanelDismissPopups);
        focus_window(cwin);
        const uint32_t stack_above = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(conn_, cw.frame, XCB_CONFIG_WINDOW_STACK_MODE, &stack_above);
        raise_docks();

        if (ev->detail == XCB_BUTTON_INDEX_1) {
            const int btn = hit_test_titlebar_button(cw, ev->event_x, ev->event_y);
            if (btn == 3) {
                close_window(cwin);
                return;
            }
            if (btn == 2) {
                toggle_maximize(cwin);
                return;
            }
            if (btn == 1) {
                minimize_window(cwin);
                return;
            }

            // Double-click titlebar toggles maximize
            if (last_title_click_win_ == cwin && (ev->time - last_title_click_time_) < 300) {
                last_title_click_win_ = XCB_WINDOW_NONE;
                toggle_maximize(cwin);
                return;
            }
            last_title_click_win_ = cwin;
            last_title_click_time_ = ev->time;

            if (!cw.fullscreen) {
                begin_window_move(cwin, ev->root_x, ev->root_y, ev->time);
            }
        }
        return;
    }

    auto it = clients_.find(ev_win);
    if (it == clients_.end()) {
        xcb_allow_events(conn_, XCB_ALLOW_REPLAY_POINTER, ev->time);
        return;
    }

    send_panel_action(ipc::kPanelDismissPopups);
    focus_window(ev_win);
    const uint32_t stack_above = XCB_STACK_MODE_ABOVE;
    const xcb_window_t raise_win =
        (it->second.frame != XCB_WINDOW_NONE) ? it->second.frame : ev_win;
    xcb_configure_window(conn_, raise_win, XCB_CONFIG_WINDOW_STACK_MODE, &stack_above);
    raise_docks();

    const uint16_t clean_state =
        ev->state & static_cast<uint16_t>(~(XCB_MOD_MASK_LOCK | numlock_mask_));

    if ((clean_state & XCB_MOD_MASK_4) &&
        (ev->detail == XCB_BUTTON_INDEX_1 || ev->detail == XCB_BUTTON_INDEX_3)) {
        if (it->second.fullscreen) {
            return;
        }
        if (ev->detail == XCB_BUTTON_INDEX_1) {
            begin_window_move(ev_win, ev->root_x, ev->root_y, ev->time);
        } else {
            begin_window_resize(ev_win, ev->root_x, ev->root_y, 4, ev->time);
        }
        return;
    }

    xcb_allow_events(conn_, XCB_ALLOW_REPLAY_POINTER, ev->time);
}

void WindowManager::on_motion_notify(const xcb_motion_notify_event_t* ev) {
    if (drag_mode_ == DragMode::None) {
        auto fit = frame_to_client_.find(ev->event);
        if (fit != frame_to_client_.end()) {
            auto cit = clients_.find(fit->second);
            if (cit != clients_.end()) {
                const int next_btn = hit_test_titlebar_button(cit->second, ev->event_x, ev->event_y);
                if (next_btn != cit->second.hover_btn) {
                    cit->second.hover_btn = next_btn;
                    draw_titlebar(cit->second);
                }
            }
        }
        return;
    }

    auto it = clients_.find(drag_window_);
    if (it == clients_.end()) return;
    ClientWindow& cw = it->second;

    const int dx = static_cast<int>(ev->root_x) - static_cast<int>(drag_start_root_x_);
    const int dy = static_cast<int>(ev->root_y) - static_cast<int>(drag_start_root_y_);

    Rect next = drag_start_rect_;
    if (drag_mode_ == DragMode::Move) {
        const Rect cur_wa = workarea_for_monitor(monitor_for_point(monitors_, ev->root_x, ev->root_y));
        next.x = static_cast<int16_t>(drag_start_rect_.x + dx);
        next.y = static_cast<int16_t>(std::max<int>(cur_wa.y, drag_start_rect_.y + dy));
        const uint32_t values[] = {
            static_cast<uint32_t>(static_cast<int32_t>(next.x)),
            static_cast<uint32_t>(static_cast<int32_t>(next.y))
        };
        xcb_configure_window(conn_, cw.frame,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, values);
        update_snap_preview(ev->root_x, ev->root_y);
    } else if (drag_mode_ == DragMode::Resize) {
        const Rect mon_wa = workarea_for_rect(drag_start_rect_);
        const uint16_t title_h = titlebar_height_for(cw);
        const int min_h = kMinClientHeight + title_h;

        int left = drag_start_rect_.x;
        int top = drag_start_rect_.y;
        int right = drag_start_rect_.x + drag_start_rect_.width;
        int bottom = drag_start_rect_.y + drag_start_rect_.height;

        const bool resize_left = (resize_dir_ == 0 || resize_dir_ == 6 || resize_dir_ == 7);
        const bool resize_right = (resize_dir_ == 2 || resize_dir_ == 3 || resize_dir_ == 4);
        const bool resize_top = (resize_dir_ == 0 || resize_dir_ == 1 || resize_dir_ == 2);
        const bool resize_bottom = (resize_dir_ == 4 || resize_dir_ == 5 || resize_dir_ == 6);

        if (resize_right) {
            const int max_r = (mon_wa.x + mon_wa.width) - 2 * theme::kBorderWidth;
            right = std::clamp<int>(right + dx, left + kMinClientWidth, std::max(left + kMinClientWidth, max_r));
        }
        if (resize_left) {
            left = std::clamp<int>(left + dx, mon_wa.x, right - kMinClientWidth);
        }
        if (resize_bottom) {
            const int max_b = (mon_wa.y + mon_wa.height) - 2 * theme::kBorderWidth;
            bottom = std::clamp<int>(bottom + dy, top + min_h, std::max(top + min_h, max_b));
        }
        if (resize_top) {
            top = std::clamp<int>(top + dy, mon_wa.y, bottom - min_h);
        }

        next.x = static_cast<int16_t>(left);
        next.y = static_cast<int16_t>(top);
        next.width = static_cast<uint16_t>(right - left);
        next.height = static_cast<uint16_t>(bottom - top);

        const uint32_t frame_vals[] = {
            static_cast<uint32_t>(static_cast<int32_t>(next.x)),
            static_cast<uint32_t>(static_cast<int32_t>(next.y)),
            next.width,
            next.height
        };
        xcb_configure_window(conn_, cw.frame,
                             XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                 XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                             frame_vals);
        const uint32_t client_vals[] = {
            next.width,
            static_cast<uint32_t>(std::max<int>(1, static_cast<int>(next.height) - title_h))
        };
        xcb_configure_window(conn_, cw.window,
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, client_vals);
        draw_titlebar(cw);
    }
    cw.rect = next;
}

void WindowManager::on_button_release(const xcb_button_release_event_t* ev) {
    (void)ev;
    if (drag_mode_ != DragMode::None) {
        const xcb_window_t dwin = drag_window_;
        const DragMode dmode = drag_mode_;
        const SnapZone final_snap = snap_zone_;

        hide_snap_preview();
        xcb_ungrab_pointer(conn_, XCB_CURRENT_TIME);
        drag_mode_ = DragMode::None;
        drag_window_ = XCB_WINDOW_NONE;

        if (dmode == DragMode::Move) {
            if (final_snap != SnapZone::None) {
                auto cit = clients_.find(dwin);
                if (cit != clients_.end()) {
                    // Anchor window center to target monitor before maximizing/snapping
                    cit->second.rect.x = static_cast<int16_t>(snap_mon_wa_.x + 24);
                    cit->second.rect.y = static_cast<int16_t>(snap_mon_wa_.y + 24);
                }
            }
            if (final_snap == SnapZone::TopMaximize) {
                set_maximized(dwin, true);
            } else if (final_snap == SnapZone::LeftHalf) {
                snap_window(dwin, true);
            } else if (final_snap == SnapZone::RightHalf) {
                snap_window(dwin, false);
            }
        } else if (dmode == DragMode::Resize) {
            auto it = clients_.find(dwin);
            if (it != clients_.end() && !it->second.fullscreen) {
                apply_rounded_shape(conn_, it->second.frame,
                                    static_cast<uint16_t>(it->second.rect.width + 2 * theme::kBorderWidth),
                                    static_cast<uint16_t>(it->second.rect.height + 2 * theme::kBorderWidth),
                                    12, static_cast<int16_t>(-static_cast<int16_t>(theme::kBorderWidth)));
                draw_titlebar(it->second);
            }
        }
        raise_docks();
    }
}

void WindowManager::on_key_press(const xcb_key_press_event_t* ev) {
    if (!keysyms_) return;
    const xcb_keysym_t ks = xcb_key_symbols_get_keysym(keysyms_, ev->detail, 0);

    if (ks == XK_Super_L || ks == XK_Super_R) {
        super_tap_pending_ = true;
        super_keycode_ = ev->detail;
        return;
    }

    const bool super_held = (super_keycode_ != 0);
    super_tap_pending_ = false;

    const uint16_t clean_state =
        ev->state & static_cast<uint16_t>(~(XCB_MOD_MASK_LOCK | numlock_mask_));

    if ((clean_state & XCB_MOD_MASK_1) && !(clean_state & XCB_MOD_MASK_4)) {
        if (ks == XK_Tab) {
            cycle_focus();
        } else if (ks == XK_F4 && focused_window_ != XCB_WINDOW_NONE) {
            close_window(focused_window_);
        } else if (ks == XK_F2) {
            xcb_ungrab_keyboard(conn_, XCB_CURRENT_TIME);
            xcb_flush(conn_);
            send_panel_action(ipc::kPanelToggleRunDialog);
        }
        return;
    }

    if (!(clean_state & XCB_MOD_MASK_4) && !super_held) return;

    const bool shifted = (clean_state & XCB_MOD_MASK_SHIFT) != 0;
    switch (ks) {
    case XK_Return:
        spawn_terminal();
        break;
    case XK_q:
    case XK_Q:
        if (focused_window_ != XCB_WINDOW_NONE) {
            close_window(focused_window_);
        }
        break;
    case XK_Escape:
        std::cout << "lunar-wm: Super+Escape pressed, exiting cleanly\n";
        running_ = false;
        break;
    case XK_m:
    case XK_M:
        if (focused_window_ != XCB_WINDOW_NONE) {
            toggle_maximize(focused_window_);
        }
        break;
    case XK_f:
    case XK_F:
        if (focused_window_ != XCB_WINDOW_NONE) {
            toggle_fullscreen(focused_window_);
        }
        break;
    case XK_g:
    case XK_G:
        xcb_ungrab_keyboard(conn_, XCB_CURRENT_TIME);
        xcb_flush(conn_);
        send_panel_action(ipc::kPanelToggleGeminiLive);
        break;
    case XK_Left:
        if (focused_window_ != XCB_WINDOW_NONE) {
            snap_window(focused_window_, true);
        }
        break;
    case XK_Right:
        if (focused_window_ != XCB_WINDOW_NONE) {
            snap_window(focused_window_, false);
        }
        break;
    case XK_Up:
        if (focused_window_ != XCB_WINDOW_NONE) {
            set_maximized(focused_window_, true);
        }
        break;
    case XK_Down:
        if (focused_window_ != XCB_WINDOW_NONE) {
            set_maximized(focused_window_, false);
        }
        break;
    case XK_Tab:
        cycle_focus();
        break;
    case XK_1:
        if (shifted && focused_window_ != XCB_WINDOW_NONE) {
            move_client_to_desktop(focused_window_, 0);
        } else {
            switch_desktop(0);
        }
        break;
    case XK_2:
        if (shifted && focused_window_ != XCB_WINDOW_NONE) {
            move_client_to_desktop(focused_window_, 1);
        } else {
            switch_desktop(1);
        }
        break;
    case XK_space:
    case XK_a:
    case XK_A:
        send_panel_action(ipc::kPanelToggleAppMenu);
        break;
    default:
        break;
    }
}

void WindowManager::on_key_release(const xcb_key_release_event_t* ev) {
    if (!keysyms_) return;
    const xcb_keysym_t ks = xcb_key_symbols_get_keysym(keysyms_, ev->detail, 0);
    if (ks == XK_Super_L || ks == XK_Super_R || ev->detail == super_keycode_) {
        const bool was_tap = super_tap_pending_;
        super_tap_pending_ = false;
        super_keycode_ = 0;
        if (was_tap) {
            send_panel_action(ipc::kPanelToggleAppMenu);
        }
    }
}

void WindowManager::on_property_notify(const xcb_property_notify_event_t* ev) {
    if (!ewmh_initialized_) return;
    if (ev->atom == ewmh_._NET_WM_STRUT_PARTIAL || ev->atom == ewmh_._NET_WM_STRUT ||
        ev->atom == ewmh_._NET_WM_WINDOW_TYPE) {
        if (docks_.contains(ev->window) || is_dock_window(ev->window)) {
            Strut s{};
            read_window_strut(ev->window, s);
            docks_[ev->window] = s;
            recompute_workarea();
        }
        return;
    }

    if (ev->atom == ewmh_._NET_WM_NAME || ev->atom == XCB_ATOM_WM_NAME) {
        auto it = clients_.find(ev->window);
        if (it != clients_.end()) {
            it->second.title = read_window_title(ev->window);
            draw_titlebar(it->second);
        }
    }
}

void WindowManager::on_client_message(const xcb_client_message_event_t* ev) {
    if (!ewmh_initialized_) return;

    if (ev->type == ewmh_._NET_ACTIVE_WINDOW) {
        if (clients_.contains(ev->window)) {
            restore_window(ev->window);
        }
    } else if (ev->type == ewmh_._NET_CLOSE_WINDOW) {
        if (clients_.contains(ev->window)) {
            close_window(ev->window);
        }
    } else if (ev->type == ewmh_._NET_CURRENT_DESKTOP) {
        switch_desktop(ev->data.data32[0]);
    } else if (ev->type == wm_change_state_atom_ && clients_.contains(ev->window)) {
        if (ev->data.data32[0] == 3 /*IconicState*/) {
            minimize_window(ev->window);
        }
    } else if (ev->type == lunar_wm_action_atom_) {
        if (ev->data.data32[0] == ipc::kWmExit) {
            std::cout << "lunar-wm: exit requested via panel action\n";
            running_ = 0;
        } else if (ev->data.data32[0] == ipc::kWmRestart) {
            std::cout << "lunar-wm: restart requested via panel action\n";
            request_restart();
        } else if (ev->data.data32[0] == ipc::kWmReloadWallpaper) {
            apply_wallpaper(true);
        } else if (ev->data.data32[0] == ipc::kWmReloadDesktopIcons) {
            reload_desktop_icons();
        } else if (ev->data.data32[0] == ipc::kWmToggleMinimize) {
            const xcb_window_t target = ev->data.data32[1];
            auto it = clients_.find(target);
            if (it != clients_.end()) {
                if (it->second.minimized) {
                    restore_window(target);
                } else {
                    minimize_window(target);
                }
            }
        }
    } else if (ev->type == ewmh_._NET_WM_STATE && clients_.contains(ev->window)) {
        const uint32_t action = ev->data.data32[0];
        const xcb_atom_t prop1 = ev->data.data32[1];
        const xcb_atom_t prop2 = ev->data.data32[2];
        const bool is_fs_prop =
            prop1 == ewmh_._NET_WM_STATE_FULLSCREEN ||
            prop2 == ewmh_._NET_WM_STATE_FULLSCREEN;
        if (is_fs_prop) {
            const auto& cw = clients_[ev->window];
            const bool want_fs = (action == 1) || (action == 2 && !cw.fullscreen);
            set_fullscreen(ev->window, want_fs);
        }

        const bool is_max_prop =
            prop1 == ewmh_._NET_WM_STATE_MAXIMIZED_HORZ ||
            prop1 == ewmh_._NET_WM_STATE_MAXIMIZED_VERT ||
            prop2 == ewmh_._NET_WM_STATE_MAXIMIZED_HORZ ||
            prop2 == ewmh_._NET_WM_STATE_MAXIMIZED_VERT;
        if (is_max_prop) {
            auto& cw = clients_[ev->window];
            const bool want_max = (action == 1) || (action == 2 && !cw.maximized);
            if (want_max != cw.maximized) {
                toggle_maximize(ev->window);
            }
        }

        if (prop1 == ewmh_._NET_WM_STATE_HIDDEN || prop2 == ewmh_._NET_WM_STATE_HIDDEN) {
            auto& cw = clients_[ev->window];
            const bool want_min = (action == 1) || (action == 2 && !cw.minimized);
            if (want_min) minimize_window(ev->window);
            else restore_window(ev->window);
        }
    } else if (ev->type == ewmh_._NET_WM_MOVERESIZE && clients_.contains(ev->window)) {
        const int16_t root_x = static_cast<int16_t>(ev->data.data32[0]);
        const int16_t root_y = static_cast<int16_t>(ev->data.data32[1]);
        const uint32_t direction = ev->data.data32[2];
        if (direction == 11) {
            // _NET_WM_MOVERESIZE_CANCEL
            hide_snap_preview();
            if (drag_mode_ != DragMode::None) {
                xcb_ungrab_pointer(conn_, XCB_CURRENT_TIME);
                drag_mode_ = DragMode::None;
                drag_window_ = XCB_WINDOW_NONE;
            }
        } else if (direction == 8 || direction == 10) {
            // _NET_WM_MOVERESIZE_MOVE / _NET_WM_MOVERESIZE_MOVE_KEYBOARD
            begin_window_move(ev->window, root_x, root_y, XCB_CURRENT_TIME);
        } else if (direction <= 7) {
            begin_window_resize(ev->window, root_x, root_y, static_cast<int>(direction),
                                XCB_CURRENT_TIME);
        }
    }
}

void WindowManager::on_configure_notify(const xcb_configure_notify_event_t* ev) {
    if (ev->window == screen_->root && ev->width > 0 && ev->height > 0 &&
        (ev->width != screen_width_ || ev->height != screen_height_)) {
        screen_width_ = ev->width;
        screen_height_ = ev->height;
        monitors_ = query_monitors(conn_, screen_);
        apply_wallpaper();
        recompute_workarea();
    }
}

void WindowManager::on_randr_screen_change() {
    auto info_cookie = xcb_randr_get_screen_info(conn_, screen_->root);
    UniqueMalloc<xcb_randr_get_screen_info_reply_t> info(
        xcb_randr_get_screen_info_reply(conn_, info_cookie, nullptr));
    if (info) {
        xcb_randr_screen_size_t* sizes = xcb_randr_get_screen_info_sizes(info.get());
        const int num_sizes = xcb_randr_get_screen_info_sizes_length(info.get());
        if (info->sizeID < num_sizes) {
            if (sizes[info->sizeID].width != screen_width_ ||
                sizes[info->sizeID].height != screen_height_) {
                screen_width_ = sizes[info->sizeID].width;
                screen_height_ = sizes[info->sizeID].height;
                monitors_ = query_monitors(conn_, screen_);
                apply_wallpaper();
                recompute_workarea();
            }
        }
    }
}

void WindowManager::toggle_maximize(xcb_window_t win) {
    auto it = clients_.find(win);
    if (it == clients_.end()) return;
    set_maximized(win, !it->second.maximized);
}

void WindowManager::set_maximized(xcb_window_t win, bool enable) {
    auto it = clients_.find(win);
    if (it == clients_.end()) return;
    ClientWindow& cw = it->second;
    if (cw.fullscreen) {
        set_fullscreen(win, false);
    }
    if (cw.maximized == enable) return;

    if (enable) {
        const Rect mon_wa = workarea_for_rect(cw.rect);
        cw.saved_rect = cw.rect;
        cw.maximized = true;
        cw.rect = Rect{
            mon_wa.x,
            mon_wa.y,
            std::max<uint16_t>(kMinClientWidth, mon_wa.width),
            std::max<uint16_t>(kMinClientHeight, mon_wa.height)
        };
        configure_client_rect(win, cw.rect, 0);
    } else {
        cw.maximized = false;
        cw.rect = cw.saved_rect;
        clamp_rect_to_workarea(cw.rect);
        configure_client_rect(win, cw.rect, theme::kBorderWidth);
    }
    update_wm_state_property(win, cw);
    raise_docks();
}

void WindowManager::begin_window_move(xcb_window_t win, int16_t root_x, int16_t root_y,
                                      xcb_timestamp_t time) {
    auto it = clients_.find(win);
    if (it == clients_.end() || it->second.fullscreen) return;
    ClientWindow& cw = it->second;

    focus_window(win);
    const uint32_t stack_above = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn_, cw.frame, XCB_CONFIG_WINDOW_STACK_MODE, &stack_above);
    raise_docks();

    if (cw.maximized) {
        cw.maximized = false;
        Rect restored = cw.saved_rect;
        if (restored.width < kMinClientWidth || restored.height < kMinClientHeight) {
            restored.width = static_cast<uint16_t>(std::max<int>(kMinClientWidth, workarea_.width * 2 / 3));
            restored.height = static_cast<uint16_t>(std::max<int>(kMinClientHeight, workarea_.height * 2 / 3));
        }
        restored.x = static_cast<int16_t>(root_x - static_cast<int>(restored.width) / 2);
        restored.y = static_cast<int16_t>(std::max<int>(workarea_.y, root_y - 14));
        cw.rect = restored;
        configure_client_rect(win, cw.rect, theme::kBorderWidth);
        update_wm_state_property(win, cw);
    }

    drag_window_ = win;
    drag_start_root_x_ = root_x;
    drag_start_root_y_ = root_y;
    drag_start_rect_ = cw.rect;
    drag_mode_ = DragMode::Move;
    snap_zone_ = SnapZone::None;

    xcb_grab_pointer(conn_, 0, screen_->root,
                     XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_RELEASE,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                     XCB_WINDOW_NONE, XCB_CURSOR_NONE, time);
}

void WindowManager::begin_window_resize(xcb_window_t win, int16_t root_x, int16_t root_y,
                                        int dir, xcb_timestamp_t time) {
    auto it = clients_.find(win);
    if (it == clients_.end() || it->second.fullscreen) return;
    ClientWindow& cw = it->second;

    focus_window(win);
    const uint32_t stack_above = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn_, cw.frame, XCB_CONFIG_WINDOW_STACK_MODE, &stack_above);
    raise_docks();

    if (cw.maximized) {
        cw.maximized = false;
        update_wm_state_property(win, cw);
    }

    drag_window_ = win;
    drag_start_root_x_ = root_x;
    drag_start_root_y_ = root_y;
    drag_start_rect_ = cw.rect;
    resize_dir_ = dir;
    drag_mode_ = DragMode::Resize;
    snap_zone_ = SnapZone::None;

    xcb_grab_pointer(conn_, 0, screen_->root,
                     XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_BUTTON_RELEASE,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                     XCB_WINDOW_NONE, XCB_CURSOR_NONE, time);
}

Rect WindowManager::snap_rect_for(SnapZone zone, const Rect& mon_wa) const noexcept {
    constexpr int kPad = 8;
    if (zone == SnapZone::TopMaximize) {
        return Rect{
            static_cast<int16_t>(mon_wa.x + kPad),
            static_cast<int16_t>(mon_wa.y + kPad),
            static_cast<uint16_t>(std::max<int>(100, static_cast<int>(mon_wa.width) - 2 * kPad)),
            static_cast<uint16_t>(std::max<int>(100, static_cast<int>(mon_wa.height) - 2 * kPad))
        };
    }
    const uint16_t half_w = static_cast<uint16_t>(
        std::max<int>(100, (static_cast<int>(mon_wa.width) - 3 * kPad) / 2));
    const uint16_t full_h = static_cast<uint16_t>(
        std::max<int>(100, static_cast<int>(mon_wa.height) - 2 * kPad));
    if (zone == SnapZone::LeftHalf) {
        return Rect{static_cast<int16_t>(mon_wa.x + kPad),
                    static_cast<int16_t>(mon_wa.y + kPad), half_w, full_h};
    }
    if (zone == SnapZone::RightHalf) {
        return Rect{static_cast<int16_t>(mon_wa.x + 2 * kPad + half_w),
                    static_cast<int16_t>(mon_wa.y + kPad), half_w, full_h};
    }
    return Rect{};
}

void WindowManager::update_snap_preview(int16_t root_x, int16_t root_y) {
    const Rect mon_wa = workarea_for_monitor(monitor_for_point(monitors_, root_x, root_y));
    SnapZone next_zone = SnapZone::None;
    constexpr int kEdge = 12;
    if (root_y <= mon_wa.y + kEdge) {
        next_zone = SnapZone::TopMaximize;
    } else if (root_x <= mon_wa.x + kEdge) {
        next_zone = SnapZone::LeftHalf;
    } else if (root_x >= mon_wa.x + static_cast<int>(mon_wa.width) - kEdge) {
        next_zone = SnapZone::RightHalf;
    }

    if (next_zone == snap_zone_ &&
        mon_wa.x == snap_mon_wa_.x && mon_wa.y == snap_mon_wa_.y &&
        mon_wa.width == snap_mon_wa_.width && mon_wa.height == snap_mon_wa_.height) {
        return;
    }
    snap_zone_ = next_zone;
    snap_mon_wa_ = mon_wa;

    if (snap_zone_ == SnapZone::None) {
        hide_snap_preview();
        return;
    }

    if (snap_preview_win_ == XCB_WINDOW_NONE) {
        snap_preview_win_ = xcb_generate_id(conn_);
        const uint32_t vals[] = {0x1e3a5f, 0x38bdf8, 1u};
        xcb_create_window(conn_, XCB_COPY_FROM_PARENT, snap_preview_win_, screen_->root,
                          0, 0, 100, 100, 2, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          screen_->root_visual,
                          XCB_CW_BACK_PIXEL | XCB_CW_BORDER_PIXEL | XCB_CW_OVERRIDE_REDIRECT, vals);
    }

    const Rect r = snap_rect_for(snap_zone_, mon_wa);
    const uint32_t cfg[] = {
        static_cast<uint32_t>(static_cast<int32_t>(r.x)),
        static_cast<uint32_t>(static_cast<int32_t>(r.y)),
        static_cast<uint32_t>(std::max<int>(1, static_cast<int>(r.width) - 4)),
        static_cast<uint32_t>(std::max<int>(1, static_cast<int>(r.height) - 4))
    };
    xcb_configure_window(conn_, snap_preview_win_,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         cfg);
    apply_rounded_shape(conn_, snap_preview_win_, r.width, r.height, 12, -2);
    xcb_map_window(conn_, snap_preview_win_);

    auto it = clients_.find(drag_window_);
    if (it != clients_.end() && it->second.frame != XCB_WINDOW_NONE) {
        const uint32_t stack_vals[] = {it->second.frame, XCB_STACK_MODE_BELOW};
        xcb_configure_window(conn_, snap_preview_win_,
                             XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, stack_vals);
    }
    raise_docks();
}

void WindowManager::hide_snap_preview() {
    snap_zone_ = SnapZone::None;
    if (snap_preview_win_ != XCB_WINDOW_NONE) {
        xcb_unmap_window(conn_, snap_preview_win_);
    }
}

void WindowManager::snap_window(xcb_window_t win, bool left_half) {
    auto it = clients_.find(win);
    if (it == clients_.end()) return;
    ClientWindow& cw = it->second;
    if (cw.fullscreen) {
        set_fullscreen(win, false);
    }
    cw.maximized = false;

    const Rect mon_wa = workarea_for_rect(cw.rect);
    constexpr int kGap = 8;
    const int avail_w = static_cast<int>(mon_wa.width) - 3 * kGap;
    const int half_w = std::max<int>(kMinClientWidth, avail_w / 2 - 2 * theme::kBorderWidth);
    const int snap_h = std::max<int>(kMinClientHeight,
        static_cast<int>(mon_wa.height) - 2 * kGap - 2 * theme::kBorderWidth);

    cw.rect = Rect{
        static_cast<int16_t>(left_half ? (mon_wa.x + kGap)
                                       : (mon_wa.x + kGap * 2 + half_w + 2 * theme::kBorderWidth)),
        static_cast<int16_t>(mon_wa.y + kGap),
        static_cast<uint16_t>(half_w),
        static_cast<uint16_t>(snap_h)
    };
    configure_client_rect(win, cw.rect);
    update_wm_state_property(win, cw);
    raise_docks();
}

void WindowManager::toggle_fullscreen(xcb_window_t win) {
    auto it = clients_.find(win);
    if (it == clients_.end()) return;
    set_fullscreen(win, !it->second.fullscreen);
}

void WindowManager::set_fullscreen(xcb_window_t win, bool enable) {
    auto it = clients_.find(win);
    if (it == clients_.end()) return;
    ClientWindow& cw = it->second;
    if (cw.fullscreen == enable) return;

    if (enable) {
        const Rect mon = monitors_.empty() ? Rect{0, 0, screen_width_, screen_height_}
                                           : monitor_for_rect(monitors_, cw.rect);
        cw.pre_fullscreen_rect = cw.rect;
        cw.fullscreen = true;
        cw.rect = mon;
        configure_client_rect(win, cw.rect, 0);
        const uint32_t stack_above = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(conn_, cw.frame, XCB_CONFIG_WINDOW_STACK_MODE, &stack_above);
    } else {
        cw.fullscreen = false;
        if (cw.maximized) {
            const Rect mon_wa = workarea_for_rect(cw.rect);
            cw.rect = Rect{
                mon_wa.x,
                mon_wa.y,
                std::max<uint16_t>(kMinClientWidth, mon_wa.width),
                std::max<uint16_t>(kMinClientHeight, mon_wa.height)
            };
        } else {
            cw.rect = cw.pre_fullscreen_rect;
            clamp_rect_to_workarea(cw.rect);
        }
        configure_client_rect(win, cw.rect, theme::kBorderWidth);
    }

    update_wm_state_property(win, cw);
    raise_docks();
}

void WindowManager::minimize_window(xcb_window_t win) {
    auto it = clients_.find(win);
    if (it == clients_.end() || it->second.minimized) return;
    ClientWindow& cw = it->second;
    cw.minimized = true;
    cw.mapped = false;
    xcb_unmap_window(conn_, cw.frame);
    set_icccm_wm_state(win, 3 /*IconicState*/);
    update_wm_state_property(win, cw);

    if (focused_window_ == win) {
        focused_window_ = XCB_WINDOW_NONE;
        xcb_window_t next_focus = XCB_WINDOW_NONE;
        for (auto rit = client_order_.rbegin(); rit != client_order_.rend(); ++rit) {
            auto cit = clients_.find(*rit);
            if (cit != clients_.end() && cit->second.desktop == current_desktop_ &&
                !cit->second.minimized) {
                next_focus = *rit;
                break;
            }
        }
        if (next_focus != XCB_WINDOW_NONE) {
            focus_window(next_focus);
        } else {
            xcb_set_input_focus(conn_, XCB_INPUT_FOCUS_POINTER_ROOT,
                                screen_->root, XCB_CURRENT_TIME);
            if (ewmh_initialized_) {
                xcb_ewmh_set_active_window(&ewmh_, screen_idx_, XCB_WINDOW_NONE);
            }
        }
    }
}

void WindowManager::restore_window(xcb_window_t win) {
    auto it = clients_.find(win);
    if (it == clients_.end()) return;
    ClientWindow& cw = it->second;

    if (cw.desktop != current_desktop_) {
        switch_desktop(cw.desktop);
    }
    if (cw.minimized || !cw.mapped) {
        cw.minimized = false;
        cw.mapped = true;
        xcb_map_window(conn_, win);
        xcb_map_window(conn_, cw.frame);
        set_icccm_wm_state(win, 1 /*NormalState*/);
        update_wm_state_property(win, cw);
    }
    focus_window(win);
    const uint32_t stack_above = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn_, cw.frame, XCB_CONFIG_WINDOW_STACK_MODE, &stack_above);
    draw_titlebar(cw);
    raise_docks();
}

void WindowManager::move_client_to_desktop(xcb_window_t win, uint32_t target) {
    if (target >= 2) return;
    auto it = clients_.find(win);
    if (it == clients_.end()) return;
    it->second.desktop = target;
    if (ewmh_initialized_) {
        xcb_ewmh_set_wm_desktop(&ewmh_, win, target);
    }
    switch_desktop(target);
    focus_window(win);
    raise_docks();
}

void WindowManager::update_wm_state_property(xcb_window_t win, const ClientWindow& cw) {
    if (!ewmh_initialized_) return;
    std::vector<xcb_atom_t> atoms;
    if (cw.fullscreen) atoms.push_back(ewmh_._NET_WM_STATE_FULLSCREEN);
    if (cw.maximized) {
        atoms.push_back(ewmh_._NET_WM_STATE_MAXIMIZED_VERT);
        atoms.push_back(ewmh_._NET_WM_STATE_MAXIMIZED_HORZ);
    }
    if (cw.minimized) atoms.push_back(ewmh_._NET_WM_STATE_HIDDEN);
    xcb_ewmh_set_wm_state(&ewmh_, win, static_cast<uint32_t>(atoms.size()),
                          atoms.empty() ? nullptr : atoms.data());
}

void WindowManager::focus_window(xcb_window_t win) {
    auto it = clients_.find(win);
    if (it == clients_.end() || it->second.minimized) return;

    const xcb_window_t prev = focused_window_;
    if (prev != XCB_WINDOW_NONE && prev != win) {
        auto pit = clients_.find(prev);
        if (pit != clients_.end()) {
            const uint32_t unfocused_px = theme::kBorderUnfocused;
            xcb_change_window_attributes(conn_, pit->second.frame, XCB_CW_BORDER_PIXEL, &unfocused_px);
            focused_window_ = win;
            draw_titlebar(pit->second);
        }
    }

    focused_window_ = win;
    const uint32_t focused_px = theme::kBorderFocused;
    xcb_change_window_attributes(conn_, it->second.frame, XCB_CW_BORDER_PIXEL, &focused_px);
    draw_titlebar(it->second);
    xcb_set_input_focus(conn_, XCB_INPUT_FOCUS_POINTER_ROOT, win, XCB_CURRENT_TIME);

    if (client_supports_protocol(conn_, win, wm_protocols_atom_, wm_take_focus_atom_)) {
        xcb_client_message_event_t msg{};
        msg.response_type = XCB_CLIENT_MESSAGE;
        msg.format = 32;
        msg.window = win;
        msg.type = wm_protocols_atom_;
        msg.data.data32[0] = wm_take_focus_atom_;
        msg.data.data32[1] = XCB_CURRENT_TIME;
        xcb_send_event(conn_, 0, win, XCB_EVENT_MASK_NO_EVENT,
                       reinterpret_cast<const char*>(&msg));
    }

    if (ewmh_initialized_) {
        xcb_ewmh_set_active_window(&ewmh_, screen_idx_, win);
    }
}

void WindowManager::switch_desktop(uint32_t target) {
    if (target >= 2 || target == current_desktop_) return;
    current_desktop_ = target;

    xcb_window_t new_focus = XCB_WINDOW_NONE;
    for (xcb_window_t win : client_order_) {
        auto it = clients_.find(win);
        if (it == clients_.end()) continue;
        ClientWindow& cw = it->second;
        if (cw.desktop == current_desktop_) {
            if (!cw.minimized && !cw.mapped) {
                xcb_map_window(conn_, cw.frame);
                cw.mapped = true;
                draw_titlebar(cw);
            }
            if (!cw.minimized) {
                new_focus = win;
            }
        } else {
            if (cw.mapped) {
                xcb_unmap_window(conn_, cw.frame);
                cw.mapped = false;
            }
        }
    }

    if (ewmh_initialized_) {
        xcb_ewmh_set_current_desktop(&ewmh_, screen_idx_, current_desktop_);
    }

    if (new_focus != XCB_WINDOW_NONE) {
        focus_window(new_focus);
    } else {
        focused_window_ = XCB_WINDOW_NONE;
        xcb_set_input_focus(conn_, XCB_INPUT_FOCUS_POINTER_ROOT,
                            screen_->root, XCB_CURRENT_TIME);
        if (ewmh_initialized_) {
            xcb_ewmh_set_active_window(&ewmh_, screen_idx_, XCB_WINDOW_NONE);
        }
    }
    raise_docks();
}

void WindowManager::cycle_focus() {
    std::vector<xcb_window_t> visible;
    for (xcb_window_t w : client_order_) {
        auto it = clients_.find(w);
        if (it != clients_.end() && it->second.desktop == current_desktop_ &&
            !it->second.minimized) {
            visible.push_back(w);
        }
    }
    if (visible.size() < 2) return;
    auto it = std::find(visible.begin(), visible.end(), focused_window_);
    size_t next_idx = 0;
    if (it != visible.end()) {
        next_idx = (static_cast<size_t>(std::distance(visible.begin(), it)) + 1) %
                   visible.size();
    }
    xcb_window_t target = visible[next_idx];
    focus_window(target);
    const uint32_t stack_above = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn_, clients_[target].frame, XCB_CONFIG_WINDOW_STACK_MODE, &stack_above);
    raise_docks();
}

void WindowManager::close_window(xcb_window_t win) {
    if (client_supports_protocol(conn_, win, wm_protocols_atom_, wm_delete_window_atom_)) {
        xcb_client_message_event_t msg{};
        msg.response_type = XCB_CLIENT_MESSAGE;
        msg.format = 32;
        msg.window = win;
        msg.type = wm_protocols_atom_;
        msg.data.data32[0] = wm_delete_window_atom_;
        msg.data.data32[1] = XCB_CURRENT_TIME;
        xcb_send_event(conn_, 0, win, XCB_EVENT_MASK_NO_EVENT,
                       reinterpret_cast<const char*>(&msg));
        xcb_flush(conn_);
        return;
    }

    xcb_kill_client(conn_, win);
    xcb_flush(conn_);
}

void WindowManager::update_client_list() {
    if (!ewmh_initialized_) return;
    const uint32_t count = static_cast<uint32_t>(client_order_.size());
    xcb_ewmh_set_client_list(&ewmh_, screen_idx_, count,
                             count > 0 ? client_order_.data() : nullptr);
    xcb_ewmh_set_client_list_stacking(&ewmh_, screen_idx_, count,
                                      count > 0 ? client_order_.data() : nullptr);
}

namespace {

xcb_window_t query_live_panel_window(xcb_connection_t* conn, xcb_window_t root, xcb_atom_t panel_atom) {
    if (!conn || root == XCB_WINDOW_NONE || panel_atom == XCB_ATOM_NONE) {
        return XCB_WINDOW_NONE;
    }
    auto cookie = xcb_get_property(conn, 0, root, panel_atom, XCB_ATOM_WINDOW, 0, 1);
    UniqueMalloc<xcb_get_property_reply_t> reply(
        xcb_get_property_reply(conn, cookie, nullptr));
    if (!reply || xcb_get_property_value_length(reply.get()) < 4) {
        return XCB_WINDOW_NONE;
    }
    const xcb_window_t panel_win = *static_cast<const xcb_window_t*>(
        xcb_get_property_value(reply.get()));
    if (panel_win == XCB_WINDOW_NONE) {
        return XCB_WINDOW_NONE;
    }
    auto attr_cookie = xcb_get_window_attributes(conn, panel_win);
    UniqueMalloc<xcb_get_window_attributes_reply_t> attr(
        xcb_get_window_attributes_reply(conn, attr_cookie, nullptr));
    return attr ? panel_win : static_cast<xcb_window_t>(XCB_WINDOW_NONE);
}

} // namespace

bool WindowManager::is_panel_alive() {
    return query_live_panel_window(conn_,
                                   screen_ ? screen_->root : static_cast<xcb_window_t>(XCB_WINDOW_NONE),
                                   lunar_panel_win_atom_) != XCB_WINDOW_NONE;
}

void WindowManager::ensure_panel_alive() {
    if (!running_ || restart_requested_) return;

    std::error_code ec;
    const std::string lock_path = "/tmp/lunar-restarting-" + display_tag_ + ".lock";
    if (std::filesystem::exists(lock_path, ec)) return;

    const std::time_t now = std::time(nullptr);
    if (now - last_panel_spawn_time_ < 3) return;
    last_panel_spawn_time_ = now;

    const std::filesystem::path self_exe(resolve_self_exe_path("/usr/bin/lunar-wm"));
    const std::filesystem::path sibling = self_exe.parent_path() / "lunar-panel";
    std::string panel_bin = "lunar-panel";
    if (std::filesystem::exists(sibling, ec)) {
        panel_bin = sibling.string();
    } else if (std::filesystem::exists("/usr/bin/lunar-panel", ec)) {
        panel_bin = "/usr/bin/lunar-panel";
    }

    const char* xephyr = std::getenv("LUNAR_XEPHYR");
    const bool is_xephyr = (xephyr && *xephyr == '1');
    const char* log_path = is_xephyr ? "/tmp/lunar-x2-panel.log" : "/tmp/lunar-panel.log";

    std::cout << "lunar-wm: panel missing, respawning " << panel_bin << '\n';
    const pid_t pid = fork();
    if (pid == 0) {
        setsid();
        const int devnull_fd = open("/dev/null", O_RDONLY);
        if (devnull_fd >= 0) {
            dup2(devnull_fd, STDIN_FILENO);
            if (devnull_fd > STDERR_FILENO) close(devnull_fd);
        }
        const int log_fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log_fd >= 0) {
            dup2(log_fd, STDOUT_FILENO);
            dup2(log_fd, STDERR_FILENO);
            if (log_fd > STDERR_FILENO) close(log_fd);
        }
        if (const char* home = std::getenv("HOME"); home && *home) {
            if (chdir(home) != 0) {
                // ignore if directory missing
            }
        }
        const char* argv[] = {panel_bin.c_str(), nullptr};
        execvp(argv[0], const_cast<char* const*>(argv));
        _exit(127);
    }
    if (pid > 0) {
        const char* pid_path = is_xephyr ? "/tmp/lunar-de-x2.pids" : "/tmp/lunar-de-pids.txt";
        std::ofstream pid_out(pid_path, std::ios::app);
        if (pid_out) {
            pid_out << pid << '\n';
        }
    }
}

void WindowManager::send_panel_action(uint32_t action, uint32_t arg1, uint32_t arg2) {
    if (lunar_panel_win_atom_ == XCB_ATOM_NONE || lunar_panel_action_atom_ == XCB_ATOM_NONE) {
        return;
    }
    const xcb_window_t panel_win = query_live_panel_window(
        conn_, screen_ ? screen_->root : static_cast<xcb_window_t>(XCB_WINDOW_NONE), lunar_panel_win_atom_);
    if (panel_win == XCB_WINDOW_NONE) {
        ensure_panel_alive();
        return;
    }

    xcb_client_message_event_t msg{};
    msg.response_type = XCB_CLIENT_MESSAGE;
    msg.format = 32;
    msg.window = panel_win;
    msg.type = lunar_panel_action_atom_;
    msg.data.data32[0] = action;
    msg.data.data32[1] = arg1;
    msg.data.data32[2] = arg2;
    xcb_send_event(conn_, 0, panel_win, XCB_EVENT_MASK_NO_EVENT,
                   reinterpret_cast<const char*>(&msg));
    xcb_flush(conn_);
}

void WindowManager::spawn_terminal() {
    const pid_t pid = fork();
    if (pid == 0) {
        setsid();
        prepare_x11_app_env();
        const char* const argv_primary[] = {"x-terminal-emulator", nullptr};
        execvp(argv_primary[0], const_cast<char* const*>(argv_primary));
        const char* const argv_fallback[] = {"gnome-terminal", nullptr};
        execvp(argv_fallback[0], const_cast<char* const*>(argv_fallback));
        _exit(127);
    }
}

} // namespace lunar
