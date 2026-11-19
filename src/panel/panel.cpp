#include "panel/panel.hpp"

#include <X11/keysym.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xcb/xcb_icccm.h>

namespace lunar {

namespace {

// App Menu layout constants
constexpr uint16_t kAppMenuW = 716;
constexpr uint16_t kAppMenuH = 516;
constexpr int kAppCols = 4;
constexpr int kAppVisibleRows = 4;
constexpr int kAppPadX = 20;
constexpr int kSearchBoxY = 16;
constexpr int kSearchBoxH = 36;
constexpr int kCatRowY = 64;
constexpr int kCatRowH = 26;
constexpr int kGridTopY = 104;
constexpr int kCellW = 166;
constexpr int kCellH = 92;
constexpr int kIconSize = 40;

// Quick Settings layout constants
constexpr uint16_t kQuickW = 328;
constexpr uint16_t kQuickH = 290;
constexpr int kQsPad = 16;
constexpr int kVolRowY = 56;
constexpr int kVolMuteX = 16;
constexpr int kVolMuteW = 28;
constexpr int kSliderX = 52;
constexpr int kSliderW = 216;
constexpr int kSliderY = 60;
constexpr int kSliderH = 22;
constexpr int kTileY1 = 102;
constexpr int kTileY2 = 162;
constexpr int kTileY3 = 222;
constexpr int kTileW = 142;
constexpr int kTileH = 50;
constexpr int kTileX1 = 16;
constexpr int kTileX2 = 170;

// Wallpaper Picker layout constants
constexpr int kWpTabY = 54;
constexpr int kWpTabH = 26;
constexpr int kWpGridY = 94;
constexpr int kWpCols = 4;
constexpr int kWpVisibleRows = 3;
constexpr int kWpCellW = 160;
constexpr int kWpCellH = 122;
constexpr int kWpThumbW = 148;
constexpr int kWpThumbH = 82;
constexpr std::array<const char*, 4> kWpGroups = {
    "32 Free", "Zorin OS", "Custom", "All"
};

// Context Menu layout constants
constexpr uint16_t kCtxW = 228;
constexpr int kCtxRowH = 34;
constexpr std::array<const char*, 8> kCtxLabels = {
    "All Applications",
    "Change Wallpaper (32+)",
    "Appearance (Bar Style)",
    "Open Terminal",
    "Files (Nautilus)",
    "System Settings",
    "Switch Workspace",
    "Exit Lunar DE",
};
constexpr uint16_t kCtxH = static_cast<uint16_t>(kCtxLabels.size() * kCtxRowH + 12);

constexpr std::array<const char*, 8> kCategories = {
    "All", "Internet", "Office", "Graphics", "Dev", "Media", "System", "Utilities"
};

std::string to_lower_copy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string run_capture_first_line(const char* cmd) {
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return {};
    char buf[256]{};
    std::string out;
    if (std::fgets(buf, sizeof(buf), pipe)) {
        out = buf;
    }
    pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

std::string ellipsize(const FontRenderer& font, const std::string& text, int px_size, int max_px) {
    if (font.measure_text(text, px_size) <= max_px) return text;
    std::string out = text;
    while (out.size() > 3 && font.measure_text(out + "...", px_size) > max_px) {
        out.pop_back();
    }
    return out + "...";
}

} // namespace

Panel::~Panel() {
    if (conn_) {
        close_popup();
        if (lunar_panel_win_atom_ != XCB_ATOM_NONE && screen_) {
            xcb_delete_property(conn_, screen_->root, lunar_panel_win_atom_);
        }
        if (popup_backbuf_ != XCB_PIXMAP_NONE) xcb_free_pixmap(conn_, popup_backbuf_);
        if (popup_gc_ != XCB_NONE) xcb_free_gc(conn_, popup_gc_);
        if (popup_win_ != XCB_WINDOW_NONE) xcb_destroy_window(conn_, popup_win_);
        if (backbuf_ != XCB_PIXMAP_NONE) xcb_free_pixmap(conn_, backbuf_);
        if (gc_ != XCB_NONE) xcb_free_gc(conn_, gc_);
        if (win_ != XCB_WINDOW_NONE) xcb_destroy_window(conn_, win_);
        if (keysyms_) xcb_key_symbols_free(keysyms_);
        if (ewmh_initialized_) xcb_ewmh_connection_wipe(&ewmh_);
        xcb_flush(conn_);
        xcb_disconnect(conn_);
        conn_ = nullptr;
    }
}

bool Panel::init(const char* display_name) {
    conn_ = xcb_connect(display_name, &screen_idx_);
    if (!conn_ || xcb_connection_has_error(conn_)) {
        std::cerr << "lunar-panel: failed to connect to X display\n";
        if (conn_) {
            xcb_disconnect(conn_);
            conn_ = nullptr;
        }
        return false;
    }

    screen_ = get_screen(conn_, screen_idx_);
    if (!screen_) {
        std::cerr << "lunar-panel: failed to locate X screen " << screen_idx_ << '\n';
        return false;
    }

    const MonitorInfo pmon = primary_monitor(query_monitors(conn_, screen_));
    primary_mon_rect_ = pmon.rect;
    panel_x_ = pmon.rect.x;
    width_ = pmon.rect.width;
    screen_height_ = pmon.rect.height;
    load_bar_layout();

    xcb_intern_atom_cookie_t* cookies = xcb_ewmh_init_atoms(conn_, &ewmh_);
    if (!xcb_ewmh_init_atoms_replies(&ewmh_, cookies, nullptr)) {
        std::cerr << "lunar-panel: warning: partial failure initializing EWMH atoms\n";
    }
    ewmh_initialized_ = true;
    keysyms_ = xcb_key_symbols_alloc(conn_);

    lunar_panel_win_atom_ = intern_atom(conn_, ipc::kPanelWindowAtom);
    lunar_panel_action_atom_ = intern_atom(conn_, ipc::kPanelActionAtom);
    lunar_wm_action_atom_ = intern_atom(conn_, ipc::kWmActionAtom);

    has_bold_font_ = font_bold_.init(true);
    has_regular_font_ = font_regular_.init(false);

    app_registry_.load();
    taskbar_pins_ = load_taskbar_pins(app_registry_);
    update_filtered_apps();
    wallpapers_ = collect_wallpapers();
    wallpaper_cycle_ = is_wallpaper_cycle_enabled();
    if (wallpaper_cycle_) {
        last_cycle_time_ = std::time(nullptr);
        for (size_t i = 0; i < wallpapers_.size(); ++i) {
            if (wallpapers_[i].path != kWallpaperCycleToken) {
                cycle_index_ = i;
                write_cycle_active_wallpaper(wallpapers_[i].path);
                break;
            }
        }
        current_wp_path_ = kWallpaperCycleToken;
    } else {
        current_wp_path_ = resolve_wallpaper_path();
    }
    update_filtered_wallpapers();

    rebuild_background_strip();
    create_window_and_gc();
    create_popup_window();
    publish_dock_properties();

    if (lunar_panel_win_atom_ != XCB_ATOM_NONE) {
        xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, screen_->root,
                            lunar_panel_win_atom_, XCB_ATOM_WINDOW, 32, 1, &win_);
    }

    const uint32_t root_mask =
        XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(conn_, screen_->root, XCB_CW_EVENT_MASK, &root_mask);

    xcb_map_window(conn_, win_);
    refresh_current_desktop();
    refresh_active_window();
    refresh_client_list();
    last_clock_ = format_clock();
    redraw();
    xcb_flush(conn_);

    std::cout << "lunar-panel: initialized (" << width_ << "x" << height_
              << ", loaded " << app_registry_.apps().size() << " apps)\n";
    return true;
}

void Panel::rebuild_background_strip() {
    bg_strip_.width = width_;
    bg_strip_.height = height_;
    bg_strip_.pixels.assign(static_cast<size_t>(width_) * height_, theme::kPanelBg);

    const std::string wp_path = resolve_wallpaper_path();
    current_wp_path_ = wp_path;
    if (!wp_path.empty()) {
        ImageBuffer full_wp = load_image_cover_scaled(wp_path, width_, screen_height_);
        if (full_wp.pixels.size() >= static_cast<size_t>(width_) * screen_height_) {
            const int src_y = (bar_layout_ == BarLayout::Taskbar)
                                  ? std::max(0, static_cast<int>(screen_height_) - height_)
                                  : 0;
            std::copy_n(full_wp.pixels.begin() + static_cast<size_t>(src_y) * width_,
                        static_cast<size_t>(width_) * height_,
                        bg_strip_.pixels.begin());
            fill_rect_alpha(bg_strip_, 0, 0, width_, height_, 0x0e1117, 0.72f);
        }
    }
    fill_rect_alpha(bg_strip_, 0, height_ - 1, width_, 1, 0x000000, 0.25f);
}

void Panel::create_window_and_gc() {
    win_ = xcb_generate_id(conn_);
    const uint32_t win_mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK;
    const uint32_t win_vals[] = {
        theme::kPanelBg,
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
            XCB_EVENT_MASK_BUTTON_PRESS
    };

    xcb_create_window(conn_, XCB_COPY_FROM_PARENT, win_, screen_->root,
                      panel_x_, panel_y_, width_, height_, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen_->root_visual, win_mask, win_vals);

    gc_ = xcb_generate_id(conn_);
    const uint32_t gc_mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    const uint32_t gc_vals[] = {theme::kTextPrimary, theme::kPanelBg, 0};
    xcb_create_gc(conn_, gc_, win_, gc_mask, gc_vals);

    recreate_backbuffer();
}

void Panel::create_popup_window() {
    popup_win_ = xcb_generate_id(conn_);
    const uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
    const uint32_t vals[] = {
        0x161a22,
        1, // override_redirect so WM doesn't tile or border popups
        XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_BUTTON_PRESS |
            XCB_EVENT_MASK_BUTTON_RELEASE | XCB_EVENT_MASK_POINTER_MOTION |
            XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE
    };
    xcb_create_window(conn_, XCB_COPY_FROM_PARENT, popup_win_, screen_->root,
                      0, height_ + 4, 200, 200, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT,
                      screen_->root_visual, mask, vals);
    if (ewmh_initialized_) {
        xcb_ewmh_set_wm_name(&ewmh_, popup_win_, 11, "lunar-popup");
    }

    popup_gc_ = xcb_generate_id(conn_);
    const uint32_t gc_mask = XCB_GC_FOREGROUND | XCB_GC_BACKGROUND | XCB_GC_GRAPHICS_EXPOSURES;
    const uint32_t gc_vals[] = {theme::kTextPrimary, 0x161a22, 0};
    xcb_create_gc(conn_, popup_gc_, popup_win_, gc_mask, gc_vals);
}

void Panel::recreate_backbuffer() {
    if (backbuf_ != XCB_PIXMAP_NONE) xcb_free_pixmap(conn_, backbuf_);
    backbuf_ = xcb_generate_id(conn_);
    xcb_create_pixmap(conn_, screen_->root_depth, backbuf_, win_, width_, height_);
    canvas_.width = width_;
    canvas_.height = height_;
    canvas_.pixels.resize(static_cast<size_t>(width_) * height_);
}

void Panel::publish_dock_properties() {
    if (!ewmh_initialized_) return;
    xcb_ewmh_set_wm_name(&ewmh_, win_, 11, "lunar-panel");

    xcb_atom_t dock_type = ewmh_._NET_WM_WINDOW_TYPE_DOCK;
    xcb_ewmh_set_wm_window_type(&ewmh_, win_, 1, &dock_type);

    xcb_ewmh_wm_strut_partial_t strut{};
    if (bar_layout_ == BarLayout::Taskbar) {
        const int mon_bottom = primary_mon_rect_.y + primary_mon_rect_.height;
        const int extra_bottom = std::max(0, static_cast<int>(screen_->height_in_pixels) - mon_bottom);
        strut.bottom = static_cast<uint32_t>(extra_bottom + height_);
        strut.bottom_start_x = static_cast<uint32_t>(std::max<int>(0, panel_x_));
        strut.bottom_end_x = static_cast<uint32_t>(std::max<int>(0, panel_x_ + static_cast<int>(width_) - 1));
        xcb_ewmh_set_wm_strut_partial(&ewmh_, win_, strut);
        xcb_ewmh_set_wm_strut(&ewmh_, win_, 0, 0, 0, strut.bottom);
    } else {
        strut.top = static_cast<uint32_t>(std::max<int>(0, panel_y_) + height_);
        strut.top_start_x = static_cast<uint32_t>(std::max<int>(0, panel_x_));
        strut.top_end_x = static_cast<uint32_t>(std::max<int>(0, panel_x_ + static_cast<int>(width_) - 1));
        xcb_ewmh_set_wm_strut_partial(&ewmh_, win_, strut);
        xcb_ewmh_set_wm_strut(&ewmh_, win_, 0, 0, strut.top, 0);
    }
}

int Panel::run() {
    running_ = true;
    const int xfd = xcb_get_file_descriptor(conn_);

    while (running_) {
        bool need_panel_redraw = false;
        bool need_popup_redraw = false;

        UniqueMalloc<xcb_generic_event_t> pending_motion;
        while (xcb_generic_event_t* raw_ev = xcb_poll_for_event(conn_)) {
            const uint8_t etype = raw_ev->response_type & ~0x80;
            if (etype == XCB_MOTION_NOTIFY) {
                pending_motion.reset(raw_ev);
                continue;
            }
            if (pending_motion) {
                handle_event(pending_motion.get());
                pending_motion.reset();
                need_popup_redraw = true;
            }
            UniqueMalloc<xcb_generic_event_t> ev(raw_ev);
            handle_event(ev.get());
            if (etype == XCB_KEY_PRESS || etype == XCB_KEY_RELEASE) {
                need_popup_redraw = true;
            } else {
                need_panel_redraw = true;
                need_popup_redraw = true;
            }
        }
        if (pending_motion) {
            handle_event(pending_motion.get());
            need_popup_redraw = true;
        }

        const std::string now_clock = format_clock();
        if (now_clock != last_clock_) {
            last_clock_ = now_clock;
            need_panel_redraw = true;
        }
        if (refresh_live_status()) {
            need_panel_redraw = true;
            if (popup_kind_ == PopupKind::GeminiLive) need_popup_redraw = true;
        }
        if (wallpaper_cycle_) {
            const std::time_t now_t = std::time(nullptr);
            if (now_t - last_cycle_time_ >= 15) {
                last_cycle_time_ = now_t;
                advance_cycle_wallpaper();
                need_panel_redraw = true;
            }
        }

        if (need_panel_redraw || need_popup_redraw) {
            if (need_panel_redraw) redraw();
            if (need_popup_redraw && popup_kind_ != PopupKind::None) {
                redraw_popup();
            }
            xcb_flush(conn_);
        }

        if (xcb_connection_has_error(conn_)) {
            std::cerr << "lunar-panel: X connection lost\n";
            return 1;
        }
        if (!running_) break;

        struct pollfd pfd{};
        pfd.fd = xfd;
        pfd.events = POLLIN;
        int ret = poll(&pfd, 1, 250);
        if (ret < 0 && errno != EINTR) {
            break;
        }
    }
    return 0;
}

void Panel::handle_event(xcb_generic_event_t* event) {
    const uint8_t type = event->response_type & ~0x80;
    switch (type) {
    case 0: {
        const auto* err = reinterpret_cast<const xcb_generic_error_t*>(event);
        if (err->error_code != XCB_WINDOW && err->error_code != XCB_DRAWABLE) {
            std::cerr << "lunar-panel: X11 error code=" << static_cast<int>(err->error_code) << '\n';
        }
        break;
    }
    case XCB_EXPOSE: {
        const auto* ex = reinterpret_cast<const xcb_expose_event_t*>(event);
        if (ex->window == popup_win_ && popup_kind_ != PopupKind::None) {
            redraw_popup();
        } else {
            redraw();
        }
        break;
    }
    case XCB_PROPERTY_NOTIFY:
        on_property_notify(reinterpret_cast<const xcb_property_notify_event_t*>(event));
        break;
    case XCB_CONFIGURE_NOTIFY:
        on_configure_notify(reinterpret_cast<const xcb_configure_notify_event_t*>(event));
        break;
    case XCB_BUTTON_PRESS:
        on_button_press(reinterpret_cast<const xcb_button_press_event_t*>(event));
        break;
    case XCB_BUTTON_RELEASE:
        on_button_release(reinterpret_cast<const xcb_button_release_event_t*>(event));
        break;
    case XCB_MOTION_NOTIFY:
        on_motion_notify(reinterpret_cast<const xcb_motion_notify_event_t*>(event));
        break;
    case XCB_KEY_PRESS:
        on_key_press(reinterpret_cast<const xcb_key_press_event_t*>(event));
        break;
    case XCB_KEY_RELEASE:
        on_key_release(reinterpret_cast<const xcb_key_release_event_t*>(event));
        break;
    case XCB_CLIENT_MESSAGE:
        on_client_message(reinterpret_cast<const xcb_client_message_event_t*>(event));
        break;
    default:
        break;
    }
}

void Panel::on_property_notify(const xcb_property_notify_event_t* ev) {
    if (!ewmh_initialized_) return;
    if (ev->window == screen_->root) {
        if (ev->atom == ewmh_._NET_ACTIVE_WINDOW) {
            refresh_active_window();
            refresh_client_list();
        } else if (ev->atom == ewmh_._NET_CLIENT_LIST) {
            refresh_client_list();
        } else if (ev->atom == ewmh_._NET_CURRENT_DESKTOP) {
            refresh_current_desktop();
        }
    } else if (ev->window == active_window_ &&
               (ev->atom == ewmh_._NET_WM_NAME || ev->atom == XCB_ATOM_WM_NAME)) {
        active_title_ = read_window_title(active_window_);
        refresh_client_list();
    }
}

void Panel::on_configure_notify(const xcb_configure_notify_event_t* ev) {
    if (ev->window == screen_->root && ev->width > 0 && ev->height > 0) {
        const MonitorInfo pmon = primary_monitor(query_monitors(conn_, screen_));
        const int16_t next_y = (bar_layout_ == BarLayout::Taskbar)
                                   ? static_cast<int16_t>(pmon.rect.y + pmon.rect.height - height_)
                                   : pmon.rect.y;
        if (pmon.rect.x != panel_x_ || next_y != panel_y_ ||
            pmon.rect.width != width_ || pmon.rect.height != screen_height_) {
            primary_mon_rect_ = pmon.rect;
            panel_x_ = pmon.rect.x;
            panel_y_ = next_y;
            width_ = pmon.rect.width;
            screen_height_ = pmon.rect.height;
            const uint32_t vals[] = {
                static_cast<uint32_t>(static_cast<int32_t>(panel_x_)),
                static_cast<uint32_t>(static_cast<int32_t>(panel_y_)),
                width_,
                height_
            };
            xcb_configure_window(conn_, win_,
                                 XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                                     XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                                 vals);
            recreate_backbuffer();
            rebuild_background_strip();
            publish_dock_properties();
            close_popup();
            redraw();
        }
    }
}

void Panel::on_button_press(const xcb_button_press_event_t* ev) {
    if (ev->event == popup_win_) {
        handle_popup_button_press(ev);
        return;
    }
    if (ev->event != win_) return;

    // Check pinned taskbar icons first (LMB launches, MMB/RMB unpins)
    for (const auto& pin : pinned_hit_items_) {
        if (ev->event_x >= pin.x1 && ev->event_x <= pin.x2) {
            if (ev->detail == XCB_BUTTON_INDEX_2 || ev->detail == XCB_BUTTON_INDEX_3) {
                toggle_taskbar_pin(app_registry_, pin.app);
                taskbar_pins_ = load_taskbar_pins(app_registry_);
                redraw();
                xcb_flush(conn_);
            } else if (ev->detail == XCB_BUTTON_INDEX_1) {
                if (pin.app.id == "zorin-appearance.desktop") {
                    open_popup(PopupKind::Appearance);
                } else {
                    close_popup();
                    AppRegistry::launch_app(pin.app);
                }
            }
            return;
        }
    }

    // Check taskbar window pills next (LMB activates/minimizes, MMB/RMB closes)
    for (const auto& item : task_items_) {
        if (ev->event_x >= item.x1 && ev->event_x <= item.x2) {
            close_popup();
            if (ev->detail == XCB_BUTTON_INDEX_2 || ev->detail == XCB_BUTTON_INDEX_3) {
                send_close_window(item.window);
            } else if (ev->detail == XCB_BUTTON_INDEX_1) {
                if (item.window == active_window_ && !item.minimized) {
                    send_toggle_minimize(item.window);
                } else {
                    send_activate_window(item.window);
                }
            }
            return;
        }
    }

    // RMB anywhere else on panel opens context menu unless clicking the top-right cluster
    if (ev->detail == XCB_BUTTON_INDEX_3) {
        if (ev->event_x >= tray_hit_min_x_) {
            open_popup(popup_kind_ == PopupKind::QuickSettings
                           ? PopupKind::None
                           : PopupKind::QuickSettings);
        } else {
            open_popup(PopupKind::ContextMenu, ev->root_x, static_cast<int16_t>(height_ + 4));
        }
        return;
    }

    if (ev->detail != XCB_BUTTON_INDEX_1) return;

    if (ev->event_x >= apps_hit_min_x_ && ev->event_x <= apps_hit_max_x_) {
        open_popup(popup_kind_ == PopupKind::AppMenu ? PopupKind::None : PopupKind::AppMenu);
    } else if (ev->event_x >= ws_hit_min_x_ && ev->event_x <= ws_hit_max_x_) {
        close_popup();
        send_switch_desktop((current_desktop_ + 1) % num_desktops_);
    } else if (ev->event_x >= live_hit_min_x_ && ev->event_x <= live_hit_max_x_) {
        open_popup(popup_kind_ == PopupKind::GeminiLive
                       ? PopupKind::None
                       : PopupKind::GeminiLive);
    } else if (ev->event_x >= term_hit_min_x_ && ev->event_x <= term_hit_max_x_) {
        close_popup();
        spawn_terminal();
    } else if (ev->event_x >= tray_hit_min_x_) {
        open_popup(popup_kind_ == PopupKind::QuickSettings
                       ? PopupKind::None
                       : PopupKind::QuickSettings);
    } else {
        close_popup();
    }
}

void Panel::on_button_release(const xcb_button_release_event_t* ev) {
    (void)ev;
    dragging_volume_ = false;
}

void Panel::on_motion_notify(const xcb_motion_notify_event_t* ev) {
    if (popup_kind_ == PopupKind::None || ev->event != popup_win_) return;
    popup_hover_x_ = ev->event_x;
    popup_hover_y_ = ev->event_y;

    if (popup_kind_ == PopupKind::QuickSettings && dragging_volume_) {
        const int pct = std::clamp(
            ((ev->event_x - kSliderX) * 100) / std::max(1, kSliderW), 0, 100);
        if (pct != quick_status_.volume_pct) {
            set_system_volume(pct);
        }
    }
}

void Panel::on_key_press(const xcb_key_press_event_t* ev) {
    if (popup_kind_ == PopupKind::None || !keysyms_) return;

    const int col = (ev->state & XCB_MOD_MASK_SHIFT) ? 1 : 0;
    const xcb_keysym_t ks = xcb_key_symbols_get_keysym(keysyms_, ev->detail, col);

    if (ks == XK_Super_L || ks == XK_Super_R) {
        super_tap_pending_ = true;
        super_keycode_ = ev->detail;
        return;
    }
    super_tap_pending_ = false;

    if (ks == XK_Escape) {
        close_popup();
        return;
    }

    if (popup_kind_ == PopupKind::WallpaperPicker) {
        const int total_rows = static_cast<int>(
            (filtered_wp_indices_.size() + kWpCols - 1) / kWpCols);
        const int max_scroll = std::max(0, total_rows - kWpVisibleRows);
        if (ks == XK_Down || ks == XK_Page_Down) {
            wp_scroll_row_ = std::min(max_scroll, wp_scroll_row_ + 1);
        } else if (ks == XK_Up || ks == XK_Page_Up) {
            wp_scroll_row_ = std::max(0, wp_scroll_row_ - 1);
        }
        return;
    }

    if (popup_kind_ == PopupKind::GeminiLive && editing_api_key_) {
        if (ks == XK_BackSpace) {
            if (!api_key_input_.empty()) {
                api_key_input_.pop_back();
                redraw_popup();
                xcb_flush(conn_);
            }
        } else if (ks == XK_Return || ks == XK_KP_Enter) {
            save_gemini_api_key(api_key_input_);
            editing_api_key_ = false;
            redraw_popup();
            xcb_flush(conn_);
        } else if (ks >= 0x20 && ks <= 0x7E) {
            if (api_key_input_.size() < 128) {
                api_key_input_.push_back(static_cast<char>(ks));
                redraw_popup();
                xcb_flush(conn_);
            }
        }
        return;
    }

    if (popup_kind_ != PopupKind::AppMenu) return;

    if (ks == XK_BackSpace) {
        if (!search_query_.empty()) {
            search_query_.pop_back();
            update_filtered_apps();
        }
    } else if (ks == XK_Return || ks == XK_KP_Enter) {
        if (selected_app_pos_ >= 0 &&
            static_cast<size_t>(selected_app_pos_) < filtered_app_indices_.size()) {
            const DesktopApp app = app_registry_.apps()[filtered_app_indices_[selected_app_pos_]];
            if (app.id == "zorin-appearance.desktop") {
                open_popup(PopupKind::Appearance);
                return;
            }
            close_popup();
            AppRegistry::launch_app(app);
        }
    } else if (ks == XK_Right) {
        if (selected_app_pos_ + 1 < static_cast<int>(filtered_app_indices_.size())) {
            ++selected_app_pos_;
        }
    } else if (ks == XK_Left) {
        if (selected_app_pos_ > 0) --selected_app_pos_;
    } else if (ks == XK_Down) {
        if (selected_app_pos_ + kAppCols < static_cast<int>(filtered_app_indices_.size())) {
            selected_app_pos_ += kAppCols;
        }
    } else if (ks == XK_Up) {
        if (selected_app_pos_ - kAppCols >= 0) {
            selected_app_pos_ -= kAppCols;
        }
    } else if (ks >= 0x20 && ks <= 0x7E) {
        if (search_query_.size() < 40) {
            search_query_.push_back(static_cast<char>(ks));
            update_filtered_apps();
        }
    }

    const int sel_row = selected_app_pos_ / kAppCols;
    if (sel_row < app_scroll_row_) app_scroll_row_ = sel_row;
    if (sel_row >= app_scroll_row_ + kAppVisibleRows) {
        app_scroll_row_ = sel_row - kAppVisibleRows + 1;
    }
}

void Panel::on_key_release(const xcb_key_release_event_t* ev) {
    if (popup_kind_ == PopupKind::None || !keysyms_) return;
    const xcb_keysym_t ks = xcb_key_symbols_get_keysym(keysyms_, ev->detail, 0);
    if (ks == XK_Super_L || ks == XK_Super_R || ev->detail == super_keycode_) {
        const bool was_tap = super_tap_pending_;
        super_tap_pending_ = false;
        super_keycode_ = 0;
        if (was_tap) {
            close_popup();
        }
    }
}

void Panel::on_client_message(const xcb_client_message_event_t* ev) {
    if (ev->type != lunar_panel_action_atom_) return;
    const uint32_t action = ev->data.data32[0];
    switch (action) {
    case ipc::kPanelToggleAppMenu:
        open_popup(popup_kind_ == PopupKind::AppMenu ? PopupKind::None : PopupKind::AppMenu);
        break;
    case ipc::kPanelToggleQuickSettings:
        open_popup(popup_kind_ == PopupKind::QuickSettings
                       ? PopupKind::None
                       : PopupKind::QuickSettings);
        break;
    case ipc::kPanelOpenContextMenu:
        open_popup(PopupKind::ContextMenu,
                   static_cast<int16_t>(ev->data.data32[1]),
                   static_cast<int16_t>(ev->data.data32[2]));
        break;
    case ipc::kPanelDismissPopups:
        close_popup();
        break;
    case ipc::kPanelOpenWallpaperPicker:
        open_popup(PopupKind::WallpaperPicker);
        break;
    case ipc::kPanelReloadWallpaper:
        rebuild_background_strip();
        redraw();
        break;
    case ipc::kPanelToggleGeminiLive:
        open_popup(popup_kind_ == PopupKind::GeminiLive
                       ? PopupKind::None
                       : PopupKind::GeminiLive);
        break;
    case ipc::kPanelOpenAppearance:
        open_popup(popup_kind_ == PopupKind::Appearance
                       ? PopupKind::None
                       : PopupKind::Appearance);
        break;
    default:
        break;
    }
}

void Panel::open_popup(PopupKind kind, int16_t anchor_x, int16_t anchor_y) {
    if (kind == PopupKind::None) {
        close_popup();
        return;
    }

    popup_kind_ = kind;
    popup_hover_x_ = -1;
    popup_hover_y_ = -1;
    dragging_volume_ = false;

    const bool is_bottom = (bar_layout_ == BarLayout::Taskbar);
    int16_t px = static_cast<int16_t>(panel_x_ + 10);
    int16_t py = static_cast<int16_t>(panel_y_ + height_ + 6);

    if (kind == PopupKind::AppMenu) {
        popup_w_ = std::min<uint16_t>(kAppMenuW, static_cast<uint16_t>(std::max<int>(320, width_ - 20)));
        popup_h_ = std::min<uint16_t>(kAppMenuH, static_cast<uint16_t>(std::max<int>(240, screen_height_ - height_ - 20)));
        px = is_bottom
                 ? static_cast<int16_t>(panel_x_ + std::max<int>(10, (static_cast<int>(width_) - popup_w_) / 2))
                 : static_cast<int16_t>(panel_x_ + 10);
        py = is_bottom
                 ? static_cast<int16_t>(panel_y_ - popup_h_ - 8)
                 : static_cast<int16_t>(panel_y_ + height_ + 6);
        search_query_.clear();
        active_category_ = "All";
        desktop_icons_ = load_desktop_icons(app_registry_);
        taskbar_pins_ = load_taskbar_pins(app_registry_);
        update_filtered_apps();
    } else if (kind == PopupKind::QuickSettings) {
        popup_w_ = kQuickW;
        popup_h_ = kQuickH;
        px = static_cast<int16_t>(panel_x_ + std::max<int>(8, static_cast<int>(width_) - popup_w_ - 10));
        py = is_bottom
                 ? static_cast<int16_t>(panel_y_ - popup_h_ - 8)
                 : static_cast<int16_t>(panel_y_ + height_ + 6);
        refresh_quick_status();
    } else if (kind == PopupKind::ContextMenu) {
        popup_w_ = kCtxW;
        popup_h_ = kCtxH;
        const Rect mon = monitor_for_point(query_monitors(conn_, screen_), anchor_x, anchor_y);
        const int min_x = mon.x + 4;
        const int max_x = std::max<int>(min_x, mon.x + static_cast<int>(mon.width) - popup_w_ - 8);
        const int min_y = is_bottom ? (mon.y + 4) : (mon.y + height_ + 4);
        const int max_y = std::max<int>(min_y, mon.y + static_cast<int>(mon.height) - (is_bottom ? height_ + 4 : 8) - popup_h_);
        px = static_cast<int16_t>(std::clamp<int>(anchor_x, min_x, max_x));
        py = static_cast<int16_t>(std::clamp<int>(anchor_y, min_y, max_y));
    } else if (kind == PopupKind::WallpaperPicker) {
        popup_w_ = std::min<uint16_t>(kAppMenuW, static_cast<uint16_t>(std::max<int>(360, width_ - 20)));
        popup_h_ = std::min<uint16_t>(kAppMenuH, static_cast<uint16_t>(std::max<int>(260, screen_height_ - height_ - 20)));
        px = static_cast<int16_t>(panel_x_ + std::max<int>(10, (static_cast<int>(width_) - popup_w_) / 2));
        py = is_bottom
                 ? static_cast<int16_t>(panel_y_ - popup_h_ - 8)
                 : static_cast<int16_t>(panel_y_ + height_ + 10);
        wallpapers_ = collect_wallpapers();
        update_filtered_wallpapers();
    } else if (kind == PopupKind::GeminiLive) {
        refresh_live_status();
        popup_w_ = 360;
        popup_h_ = 296;
        px = static_cast<int16_t>(panel_x_ + std::max<int>(10, static_cast<int>(width_) - popup_w_ - 12));
        py = is_bottom
                 ? static_cast<int16_t>(panel_y_ - popup_h_ - 8)
                 : static_cast<int16_t>(panel_y_ + height_ + 8);
        editing_api_key_ = false;
    } else if (kind == PopupKind::Appearance) {
        popup_w_ = 464;
        popup_h_ = 264;
        px = static_cast<int16_t>(panel_x_ + std::max<int>(10, (static_cast<int>(width_) - popup_w_) / 2));
        py = is_bottom
                 ? static_cast<int16_t>(panel_y_ - popup_h_ - 8)
                 : static_cast<int16_t>(panel_y_ + height_ + 10);
    }

    if (popup_backbuf_ != XCB_PIXMAP_NONE) {
        xcb_free_pixmap(conn_, popup_backbuf_);
    }
    popup_backbuf_ = xcb_generate_id(conn_);
    xcb_create_pixmap(conn_, screen_->root_depth, popup_backbuf_, popup_win_, popup_w_, popup_h_);
    popup_canvas_.width = popup_w_;
    popup_canvas_.height = popup_h_;
    popup_canvas_.pixels.assign(static_cast<size_t>(popup_w_) * popup_h_, 0x161a22);

    const uint32_t vals[] = {
        static_cast<uint32_t>(static_cast<int32_t>(px)),
        static_cast<uint32_t>(static_cast<int32_t>(py)),
        popup_w_,
        popup_h_,
        XCB_STACK_MODE_ABOVE
    };
    xcb_configure_window(conn_, popup_win_,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT |
                             XCB_CONFIG_WINDOW_STACK_MODE,
                         vals);
    apply_rounded_shape(conn_, popup_win_, popup_w_, popup_h_, 12, 0);

    xcb_map_window(conn_, popup_win_);
    redraw_popup();

    xcb_grab_pointer(conn_, 1, popup_win_,
                     XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
                         XCB_EVENT_MASK_POINTER_MOTION,
                     XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                     XCB_WINDOW_NONE, XCB_CURSOR_NONE, XCB_CURRENT_TIME);
    xcb_grab_keyboard(conn_, 0, popup_win_, XCB_CURRENT_TIME,
                      XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    xcb_flush(conn_);
}

void Panel::close_popup() {
    if (popup_kind_ == PopupKind::None) return;
    popup_kind_ = PopupKind::None;
    dragging_volume_ = false;
    if (conn_ && popup_win_ != XCB_WINDOW_NONE) {
        xcb_ungrab_pointer(conn_, XCB_CURRENT_TIME);
        xcb_ungrab_keyboard(conn_, XCB_CURRENT_TIME);
        xcb_unmap_window(conn_, popup_win_);
        xcb_flush(conn_);
    }
}

void Panel::update_filtered_apps() {
    filtered_app_indices_.clear();
    const std::string q = to_lower_copy(search_query_);
    const auto& all = app_registry_.apps();
    for (size_t i = 0; i < all.size(); ++i) {
        if (active_category_ != "All" && all[i].category != active_category_) continue;
        if (!q.empty()) {
            const std::string n = to_lower_copy(all[i].name);
            const std::string c = to_lower_copy(all[i].comment);
            if (n.find(q) == std::string::npos && c.find(q) == std::string::npos) {
                continue;
            }
        }
        filtered_app_indices_.push_back(i);
    }
    app_scroll_row_ = 0;
    selected_app_pos_ = filtered_app_indices_.empty() ? -1 : 0;
}

void Panel::handle_popup_button_press(const xcb_button_press_event_t* ev) {
    if (popup_kind_ == PopupKind::None) return;

    // Click outside popup closes it cleanly
    if (ev->event_x < 0 || ev->event_y < 0 ||
        ev->event_x >= popup_w_ || ev->event_y >= popup_h_) {
        close_popup();
        return;
    }

    if (popup_kind_ == PopupKind::AppMenu) {
        handle_app_menu_click(ev->event_x, ev->event_y, ev->detail);
    } else if (popup_kind_ == PopupKind::QuickSettings) {
        handle_quick_settings_click(ev->event_x, ev->event_y, ev->detail);
    } else if (popup_kind_ == PopupKind::ContextMenu) {
        if (ev->detail == XCB_BUTTON_INDEX_1 || ev->detail == XCB_BUTTON_INDEX_3) {
            handle_context_menu_click(ev->event_x, ev->event_y);
        }
    } else if (popup_kind_ == PopupKind::WallpaperPicker) {
        handle_wallpaper_picker_click(ev->event_x, ev->event_y, ev->detail);
    } else if (popup_kind_ == PopupKind::GeminiLive) {
        handle_gemini_live_click(ev->event_x, ev->event_y, ev->detail);
    } else if (popup_kind_ == PopupKind::Appearance) {
        handle_appearance_click(ev->event_x, ev->event_y, ev->detail);
    }
}

void Panel::handle_app_menu_click(int16_t x, int16_t y, uint8_t button) {
    const int total_rows = static_cast<int>(
        (filtered_app_indices_.size() + kAppCols - 1) / kAppCols);
    const int max_scroll = std::max(0, total_rows - kAppVisibleRows);

    if (button == XCB_BUTTON_INDEX_4) {
        app_scroll_row_ = std::max(0, app_scroll_row_ - 1);
        return;
    }
    if (button == XCB_BUTTON_INDEX_5) {
        app_scroll_row_ = std::min(max_scroll, app_scroll_row_ + 1);
        return;
    }
    if (button != XCB_BUTTON_INDEX_1 && button != XCB_BUTTON_INDEX_2 &&
        button != XCB_BUTTON_INDEX_3) {
        return;
    }

    // Top-right Close (X) button in App Menu header
    if (y >= kSearchBoxY && y <= kSearchBoxY + kSearchBoxH &&
        x >= popup_w_ - kAppPadX - 30 && x <= popup_w_ - kAppPadX) {
        close_popup();
        return;
    }

    // Category filter chip hit check
    if (y >= kCatRowY && y <= kCatRowY + kCatRowH) {
        const FontRenderer& font = has_bold_font_ ? font_bold_ : font_regular_;
        int cx = kAppPadX;
        for (const char* cat : kCategories) {
            const int cw = font.measure_text(cat, 11) + 18;
            if (x >= cx && x <= cx + cw) {
                active_category_ = cat;
                update_filtered_apps();
                return;
            }
            cx += cw + 6;
        }
        return;
    }

    // App grid tile hit check
    if (y >= kGridTopY && x >= kAppPadX && x < kAppPadX + kAppCols * kCellW) {
        const int col = (x - kAppPadX) / kCellW;
        const int vis_row = (y - kGridTopY) / kCellH;
        if (col >= 0 && col < kAppCols && vis_row >= 0 && vis_row < kAppVisibleRows) {
            const int idx = (app_scroll_row_ + vis_row) * kAppCols + col;
            if (idx >= 0 && static_cast<size_t>(idx) < filtered_app_indices_.size()) {
                const DesktopApp app = app_registry_.apps()[filtered_app_indices_[idx]];
                if (button == XCB_BUTTON_INDEX_2) {
                    toggle_taskbar_pin(app_registry_, app);
                    taskbar_pins_ = load_taskbar_pins(app_registry_);
                    redraw();
                    return;
                }
                if (button == XCB_BUTTON_INDEX_3) {
                    toggle_desktop_icon(app_registry_, app);
                    desktop_icons_ = load_desktop_icons(app_registry_);
                    send_wm_reload_desktop_icons();
                    return;
                }
                if (app.id == "zorin-appearance.desktop") {
                    open_popup(PopupKind::Appearance);
                    return;
                }
                close_popup();
                AppRegistry::launch_app(app);
            }
        }
    }
}

void Panel::handle_quick_settings_click(int16_t x, int16_t y, uint8_t button) {
    if (button == XCB_BUTTON_INDEX_4) {
        set_system_volume(std::min(100, quick_status_.volume_pct + 5));
        return;
    }
    if (button == XCB_BUTTON_INDEX_5) {
        set_system_volume(std::max(0, quick_status_.volume_pct - 5));
        return;
    }
    if (button != XCB_BUTTON_INDEX_1 && button != XCB_BUTTON_INDEX_2 &&
        button != XCB_BUTTON_INDEX_3) {
        return;
    }

    // Top-right power/exit circle button in QuickSettings header
    if (y >= 12 && y <= 44 && x >= popup_w_ - 46 && x <= popup_w_ - 14) {
        close_popup();
        send_wm_exit();
        return;
    }

    // Volume mute button
    if (y >= kVolRowY && y <= kVolRowY + 30 && x >= kVolMuteX && x <= kVolMuteX + kVolMuteW) {
        toggle_system_mute();
        return;
    }

    // Volume slider
    if (y >= kSliderY - 4 && y <= kSliderY + kSliderH + 4 &&
        x >= kSliderX - 4 && x <= kSliderX + kSliderW + 4) {
        dragging_volume_ = true;
        const int pct = std::clamp(((x - kSliderX) * 100) / std::max(1, kSliderW), 0, 100);
        set_system_volume(pct);
        return;
    }

    // 3x2 Quick Toggle Tiles
    if (y >= kTileY1 && y <= kTileY1 + kTileH) {
        if (x >= kTileX1 && x <= kTileX1 + kTileW) {
            if (button == XCB_BUTTON_INDEX_3) {
                close_popup();
                spawn_command("gnome-control-center wifi");
            } else {
                toggle_wifi();
            }
            return;
        }
        if (x >= kTileX2 && x <= kTileX2 + kTileW) {
            open_popup(PopupKind::WallpaperPicker);
            return;
        }
    }

    if (y >= kTileY2 && y <= kTileY2 + kTileH) {
        if (x >= kTileX1 && x <= kTileX1 + kTileW) {
            open_popup(PopupKind::AppMenu);
            return;
        }
        if (x >= kTileX2 && x <= kTileX2 + kTileW) {
            close_popup();
            spawn_terminal();
            return;
        }
    }

    if (y >= kTileY3 && y <= kTileY3 + kTileH) {
        if (x >= kTileX1 && x <= kTileX1 + kTileW) {
            open_popup(PopupKind::Appearance);
        } else if (x >= kTileX2 && x <= kTileX2 + kTileW) {
            close_popup();
            send_wm_exit();
        }
    }
}

void Panel::handle_context_menu_click(int16_t x, int16_t y) {
    (void)x;
    const int idx = (y - 6) / kCtxRowH;
    if (idx < 0 || static_cast<size_t>(idx) >= kCtxLabels.size()) return;

    switch (idx) {
    case 0:
        open_popup(PopupKind::AppMenu);
        break;
    case 1:
        open_popup(PopupKind::WallpaperPicker);
        break;
    case 2:
        open_popup(PopupKind::Appearance);
        break;
    case 3:
        close_popup();
        spawn_terminal();
        break;
    case 4:
        close_popup();
        spawn_command("nautilus");
        break;
    case 5:
        close_popup();
        spawn_command("gnome-control-center");
        break;
    case 6:
        close_popup();
        send_switch_desktop((current_desktop_ + 1) % num_desktops_);
        break;
    case 7:
        close_popup();
        send_wm_exit();
        break;
    default:
        break;
    }
}

void Panel::refresh_quick_status() {
    // Audio volume & mute via pactl
    const std::string vol_str = run_capture_first_line(
        "pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null | grep -oE '[0-9]+%' | head -n1 | tr -d '%'");
    if (!vol_str.empty()) {
        quick_status_.volume_pct = std::clamp(std::atoi(vol_str.c_str()), 0, 100);
    }
    const std::string mute_str = run_capture_first_line(
        "pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null");
    quick_status_.muted = (mute_str.find("yes") != std::string::npos);

    // Wi-Fi status via nmcli
    const std::string radio = run_capture_first_line("nmcli radio wifi 2>/dev/null");
    quick_status_.wifi_enabled = (radio != "disabled");
    const std::string ssid = run_capture_first_line(
        "nmcli -t -f TYPE,STATE,CONNECTION dev 2>/dev/null | awk -F: '$1==\"wifi\" && $2==\"connected\" {print $3; exit}'");
    if (!ssid.empty()) {
        quick_status_.wifi_connected = true;
        quick_status_.wifi_name = ssid;
    } else {
        quick_status_.wifi_connected = false;
        quick_status_.wifi_name = quick_status_.wifi_enabled ? "Disconnected" : "Wi-Fi Off";
    }

    // Battery or AC Power
    quick_status_.power_label = "AC Power";
    std::error_code ec;
    if (std::filesystem::exists("/sys/class/power_supply", ec)) {
        for (const auto& entry : std::filesystem::directory_iterator("/sys/class/power_supply", ec)) {
            const std::string fname = entry.path().filename().string();
            if (fname.rfind("BAT", 0) == 0) {
                std::ifstream cap(entry.path() / "capacity");
                int pct = 0;
                if (cap >> pct) {
                    quick_status_.power_label = "Battery " + std::to_string(pct) + "%";
                }
                break;
            }
        }
    }
}

void Panel::set_system_volume(int pct) {
    quick_status_.volume_pct = std::clamp(pct, 0, 100);
    const std::string cmd =
        "pactl set-sink-volume @DEFAULT_SINK@ " + std::to_string(quick_status_.volume_pct) +
        "% >/dev/null 2>&1 &";
    std::system(cmd.c_str());
}

void Panel::toggle_system_mute() {
    quick_status_.muted = !quick_status_.muted;
    std::system("pactl set-sink-mute @DEFAULT_SINK@ toggle >/dev/null 2>&1 &");
}

void Panel::toggle_wifi() {
    const bool next_on = !quick_status_.wifi_enabled;
    quick_status_.wifi_enabled = next_on;
    if (!next_on) {
        quick_status_.wifi_connected = false;
        quick_status_.wifi_name = "Wi-Fi Off";
        std::system("nmcli radio wifi off >/dev/null 2>&1 &");
    } else {
        quick_status_.wifi_name = "Connecting...";
        std::system("nmcli radio wifi on >/dev/null 2>&1 &");
    }
}

void Panel::redraw_popup() {
    if (popup_kind_ == PopupKind::None || popup_backbuf_ == XCB_PIXMAP_NONE) return;

    std::fill(popup_canvas_.pixels.begin(), popup_canvas_.pixels.end(), 0x161a22);

    if (popup_kind_ == PopupKind::AppMenu) {
        draw_app_menu();
    } else if (popup_kind_ == PopupKind::QuickSettings) {
        draw_quick_settings();
    } else if (popup_kind_ == PopupKind::ContextMenu) {
        draw_context_menu();
    } else if (popup_kind_ == PopupKind::WallpaperPicker) {
        draw_wallpaper_picker();
    } else if (popup_kind_ == PopupKind::GeminiLive) {
        draw_gemini_live();
    } else if (popup_kind_ == PopupKind::Appearance) {
        draw_appearance();
    }

    stroke_rounded_rect_aa(popup_canvas_, 0.5f, 0.5f,
                           static_cast<float>(popup_w_ - 1),
                           static_cast<float>(popup_h_ - 1),
                           11.5f, 1.0f, 0x334155, 0.85f);

    upload_pixels_to_pixmap(conn_, screen_, popup_backbuf_, popup_w_, popup_h_, popup_canvas_.pixels);
    xcb_copy_area(conn_, popup_backbuf_, popup_win_, popup_gc_, 0, 0, 0, 0, popup_w_, popup_h_);
}

void Panel::draw_app_menu() {
    const FontRenderer& f_bold = has_bold_font_ ? font_bold_ : font_regular_;
    const FontRenderer& f_reg = has_regular_font_ ? font_regular_ : font_bold_;

    // Search input bar (leave room on right for Close 'X' button)
    const int search_w = popup_w_ - 2 * kAppPadX - 36;
    fill_rounded_rect_aa(popup_canvas_, static_cast<float>(kAppPadX), static_cast<float>(kSearchBoxY),
                         static_cast<float>(search_w), static_cast<float>(kSearchBoxH),
                         8.0f, 0x222834, 1.0f);
    // Magnifier icon inside search box
    stroke_circle_aa(popup_canvas_, kAppPadX + 16.0f, kSearchBoxY + 15.0f, 5.0f, 1.6f, 0x9ca3af, 0.9f);
    draw_line_aa(popup_canvas_, kAppPadX + 19.5f, kSearchBoxY + 18.5f,
                 kAppPadX + 23.5f, kSearchBoxY + 22.5f, 1.8f, 0x9ca3af, 0.9f);

    if (search_query_.empty()) {
        f_reg.draw_text(popup_canvas_, kAppPadX + 34, kSearchBoxY + 22,
                        "Type to search Zorin OS applications...", 13, 0x9ca3af);
    } else {
        f_bold.draw_text(popup_canvas_, kAppPadX + 34, kSearchBoxY + 22,
                         search_query_ + "_", 13, 0xf3f4f6);
    }

    const std::string count_str = std::to_string(filtered_app_indices_.size()) + " apps";
    const int cw_count = f_reg.measure_text(count_str, 11);
    f_reg.draw_text(popup_canvas_, kAppPadX + search_w - cw_count - 12,
                    kSearchBoxY + 21, count_str, 11, 0x9ca3af);

    // Top-right Close (X) button
    {
        const float cx = static_cast<float>(popup_w_ - kAppPadX - 13);
        const float cy = static_cast<float>(kSearchBoxY + kSearchBoxH / 2);
        const bool hov = (popup_hover_x_ >= popup_w_ - kAppPadX - 30 &&
                          popup_hover_y_ >= kSearchBoxY &&
                          popup_hover_y_ <= kSearchBoxY + kSearchBoxH);
        fill_circle_aa(popup_canvas_, cx, cy, 13.0f, hov ? 0xef4444 : 0x262d3b, 1.0f);
        const uint32_t xcol = hov ? 0xffffff : 0xcbd5e1;
        draw_line_aa(popup_canvas_, cx - 4.0f, cy - 4.0f, cx + 4.0f, cy + 4.0f, 1.6f, xcol, 0.95f);
        draw_line_aa(popup_canvas_, cx + 4.0f, cy - 4.0f, cx - 4.0f, cy + 4.0f, 1.6f, xcol, 0.95f);
    }

    // Category filter chips
    int cx = kAppPadX;
    for (const char* cat : kCategories) {
        const bool is_active = (active_category_ == cat);
        const int chip_w = f_bold.measure_text(cat, 11) + 18;
        if (cx + chip_w > popup_w_ - kAppPadX) break;
        fill_rounded_rect_aa(popup_canvas_, static_cast<float>(cx), static_cast<float>(kCatRowY),
                             static_cast<float>(chip_w), static_cast<float>(kCatRowH),
                             13.0f, is_active ? 0x2563eb : 0x232936, 1.0f);
        f_bold.draw_text(popup_canvas_, cx + 9, kCatRowY + 17, cat, 11,
                         is_active ? 0xffffff : 0xcbd5e1);
        cx += chip_w + 6;
    }

    // 4x4 App Grid
    int hovered_app_idx = -1;
    const int start_idx = app_scroll_row_ * kAppCols;
    for (int r = 0; r < kAppVisibleRows; ++r) {
        for (int c = 0; c < kAppCols; ++c) {
            const int idx = start_idx + r * kAppCols + c;
            if (idx < 0 || static_cast<size_t>(idx) >= filtered_app_indices_.size()) break;

            const int tx = kAppPadX + c * kCellW;
            const int ty = kGridTopY + r * kCellH;
            const bool hovered = (popup_hover_x_ >= tx && popup_hover_x_ < tx + kCellW &&
                                  popup_hover_y_ >= ty && popup_hover_y_ < ty + kCellH);
            const bool selected = (idx == selected_app_pos_);
            if (hovered) hovered_app_idx = idx;

            const DesktopApp& app = app_registry_.apps()[filtered_app_indices_[idx]];
            const bool pinned_desktop = is_on_desktop(desktop_icons_, app.id);
            const bool pinned_bar = is_on_desktop(taskbar_pins_, app.id);

            if (hovered || selected) {
                fill_rounded_rect_aa(popup_canvas_, static_cast<float>(tx + 4),
                                     static_cast<float>(ty + 4),
                                     static_cast<float>(kCellW - 8),
                                     static_cast<float>(kCellH - 8),
                                     10.0f, hovered ? 0x2a3548 : 0x222a38, 1.0f);
                stroke_rounded_rect_aa(popup_canvas_, static_cast<float>(tx + 4.5f),
                                       static_cast<float>(ty + 4.5f),
                                       static_cast<float>(kCellW - 9),
                                       static_cast<float>(kCellH - 9),
                                       10.0f, 1.0f, hovered ? 0x38bdf8 : 0x475569, 0.85f);
            }

            if (pinned_bar) {
                fill_circle_aa(popup_canvas_, static_cast<float>(tx + 14),
                               static_cast<float>(ty + 14), 4.0f, 0x34d399, 0.95f);
            }
            if (pinned_desktop) {
                fill_circle_aa(popup_canvas_, static_cast<float>(tx + kCellW - 14),
                               static_cast<float>(ty + 14), 4.0f, 0x38bdf8, 0.95f);
            }

            const int icon_x = tx + (kCellW - kIconSize) / 2;
            const int icon_y = ty + 12;
            app_registry_.draw_app_icon(popup_canvas_, app, icon_x, icon_y, kIconSize, f_bold);

            const std::string label = ellipsize(f_reg, app.name, 12, kCellW - 18);
            const int lw = f_reg.measure_text(label, 12);
            f_reg.draw_text(popup_canvas_, tx + (kCellW - lw) / 2, ty + 74, label, 12, 0xf1f5f9);
        }
    }

    // Scroll indicator on right edge
    const int total_rows = static_cast<int>(
        (filtered_app_indices_.size() + kAppCols - 1) / kAppCols);
    if (total_rows > kAppVisibleRows) {
        const float track_h = static_cast<float>(kAppVisibleRows * kCellH - 12);
        const float thumb_h = std::max(28.0f, track_h * (static_cast<float>(kAppVisibleRows) / total_rows));
        const float max_scroll = static_cast<float>(total_rows - kAppVisibleRows);
        const float thumb_y = static_cast<float>(kGridTopY + 6) +
                              (track_h - thumb_h) * (static_cast<float>(app_scroll_row_) / max_scroll);
        fill_rounded_rect_aa(popup_canvas_, static_cast<float>(popup_w_ - 10),
                             thumb_y, 4.0f, thumb_h, 2.0f, 0x64748b, 0.85f);
    }

    // Subtle footer hint or hovered app description
    std::string footer = "Left: Launch  |  Right: Desktop Icon  |  Middle: Pin to Taskbar";
    const int inspect_idx = (hovered_app_idx >= 0) ? hovered_app_idx : selected_app_pos_;
    if (inspect_idx >= 0 && static_cast<size_t>(inspect_idx) < filtered_app_indices_.size()) {
        const DesktopApp& happ = app_registry_.apps()[filtered_app_indices_[inspect_idx]];
        if (!happ.comment.empty()) {
            footer = ellipsize(f_reg, happ.name + " - " + happ.comment + "  (Middle-click: Pin to Taskbar)",
                               11, popup_w_ - 2 * kAppPadX);
        }
    }
    f_reg.draw_text(popup_canvas_, kAppPadX, popup_h_ - 12, footer, 11, 0x94a3b8);
}

void Panel::draw_quick_settings() {
    const FontRenderer& f_bold = has_bold_font_ ? font_bold_ : font_regular_;
    const FontRenderer& f_reg = has_regular_font_ ? font_regular_ : font_bold_;

    // Header: user@host + power label + exit button
    const char* user = std::getenv("USER");
    const std::string header = std::string(user ? user : "lunar") + "  |  " + quick_status_.power_label;
    f_bold.draw_text(popup_canvas_, kQsPad, 33, header, 13, 0xf1f5f9);

    // Top-right Power / Exit circle button
    const float pwr_cx = static_cast<float>(popup_w_ - 30);
    const float pwr_cy = 28.0f;
    const bool pwr_hover = (popup_hover_x_ >= popup_w_ - 46 && popup_hover_y_ <= 44);
    fill_circle_aa(popup_canvas_, pwr_cx, pwr_cy, 15.0f, pwr_hover ? 0xdc2626 : 0x262d3b, 1.0f);
    draw_arc_aa(popup_canvas_, pwr_cx, pwr_cy + 0.5f, 5.2f, -55.0f, 180.0f, 1.6f, 0xf8fafc, 0.95f);
    draw_arc_aa(popup_canvas_, pwr_cx, pwr_cy + 0.5f, 5.2f, -180.0f, -125.0f, 1.6f, 0xf8fafc, 0.95f);
    draw_line_aa(popup_canvas_, pwr_cx, pwr_cy - 6.0f, pwr_cx, pwr_cy + 0.3f, 1.6f, 0xf8fafc, 0.95f);

    // Volume slider card
    fill_rounded_rect_aa(popup_canvas_, static_cast<float>(kQsPad), static_cast<float>(kVolRowY - 4),
                         static_cast<float>(popup_w_ - 2 * kQsPad), 38.0f, 10.0f, 0x212734, 1.0f);
    // Speaker icon on left of slider
    const float scy = static_cast<float>(kSliderY + kSliderH / 2);
    const uint32_t spk_col = quick_status_.muted ? 0xef4444 : 0xf1f5f9;
    fill_rounded_rect_aa(popup_canvas_, kVolMuteX + 4.0f, scy - 3.0f, 4.0f, 6.0f, 1.0f, spk_col, 0.95f);
    draw_line_aa(popup_canvas_, kVolMuteX + 7.0f, scy - 2.5f, kVolMuteX + 12.0f, scy - 6.0f, 1.6f, spk_col, 0.95f);
    draw_line_aa(popup_canvas_, kVolMuteX + 12.0f, scy - 6.0f, kVolMuteX + 12.0f, scy + 6.0f, 1.6f, spk_col, 0.95f);
    draw_line_aa(popup_canvas_, kVolMuteX + 7.0f, scy + 2.5f, kVolMuteX + 12.0f, scy + 6.0f, 1.6f, spk_col, 0.95f);
    if (!quick_status_.muted) {
        draw_arc_aa(popup_canvas_, kVolMuteX + 13.0f, scy, 4.2f, -45.0f, 45.0f, 1.4f, spk_col, 0.9f);
    }

    // Slider track + fill + knob
    const float track_y = scy - 2.5f;
    fill_rounded_rect_aa(popup_canvas_, static_cast<float>(kSliderX), track_y,
                         static_cast<float>(kSliderW), 5.0f, 2.5f, 0x374151, 1.0f);
    const float fill_w = (static_cast<float>(quick_status_.volume_pct) / 100.0f) * kSliderW;
    fill_rounded_rect_aa(popup_canvas_, static_cast<float>(kSliderX), track_y,
                         std::max(5.0f, fill_w), 5.0f, 2.5f,
                         quick_status_.muted ? 0x64748b : 0x38bdf8, 1.0f);
    fill_circle_aa(popup_canvas_, static_cast<float>(kSliderX) + fill_w, scy, 7.0f, 0xf8fafc, 1.0f);

    const std::string pct_str = quick_status_.muted ? "Mute" : (std::to_string(quick_status_.volume_pct) + "%");
    f_reg.draw_text(popup_canvas_, kSliderX + kSliderW + 10, static_cast<int>(scy) + 4, pct_str, 11, 0xcbd5e1);

    // Helper lambda for Quick Settings tiles
    auto draw_tile = [&](int tx, int ty, int tw, int th, bool active,
                         const std::string& title, const std::string& sub) {
        const bool hov = (popup_hover_x_ >= tx && popup_hover_x_ <= tx + tw &&
                          popup_hover_y_ >= ty && popup_hover_y_ <= ty + th);
        uint32_t bg = active ? (hov ? 0x3b82f6 : 0x2563eb) : (hov ? 0x2b3344 : 0x212734);
        fill_rounded_rect_aa(popup_canvas_, static_cast<float>(tx), static_cast<float>(ty),
                             static_cast<float>(tw), static_cast<float>(th), 12.0f, bg, 1.0f);
        f_bold.draw_text(popup_canvas_, tx + 14, ty + 23, title, 12, 0xf8fafc);
        const std::string sub_el = ellipsize(f_reg, sub, 11, tw - 24);
        f_reg.draw_text(popup_canvas_, tx + 14, ty + 40, sub_el, 11,
                        active ? 0xdbeafe : 0x94a3b8);
    };

    draw_tile(kTileX1, kTileY1, kTileW, kTileH, quick_status_.wifi_enabled,
              "Wi-Fi", quick_status_.wifi_name);
    draw_tile(kTileX2, kTileY1, kTileW, kTileH, false,
              "Wallpaper", "32 free & cycle");
    draw_tile(kTileX1, kTileY2, kTileW, kTileH, false,
              "Applications", "All Zorin OS apps");
    draw_tile(kTileX2, kTileY2, kTileW, kTileH, false,
              "Terminal", "Open shell");
    draw_tile(kTileX1, kTileY3, kTileW, kTileH, false,
              "Appearance", bar_layout_ == BarLayout::Taskbar ? "Taskbar (Win 11)" : "Top Bar");
    draw_tile(kTileX2, kTileY3, kTileW, kTileH, false,
              "Exit Lunar DE", "Quit session");
}

void Panel::draw_context_menu() {
    const FontRenderer& f_reg = has_regular_font_ ? font_regular_ : font_bold_;
    for (size_t i = 0; i < kCtxLabels.size(); ++i) {
        const int ry = 6 + static_cast<int>(i) * kCtxRowH;
        const bool hov = (popup_hover_x_ >= 0 && popup_hover_x_ < popup_w_ &&
                          popup_hover_y_ >= ry && popup_hover_y_ < ry + kCtxRowH);
        if (hov) {
            fill_rounded_rect_aa(popup_canvas_, 6.0f, static_cast<float>(ry + 2),
                                 static_cast<float>(popup_w_ - 12),
                                 static_cast<float>(kCtxRowH - 4),
                                 6.0f, i + 1 == kCtxLabels.size() ? 0x991b1b : 0x2563eb, 0.95f);
        }
        if (i + 1 == kCtxLabels.size()) {
            fill_rect_alpha(popup_canvas_, 10, ry, popup_w_ - 20, 1, 0xffffff, 0.08f);
        }
        f_reg.draw_text(popup_canvas_, 16, ry + 22, kCtxLabels[i], 13, 0xf1f5f9);
    }
}

void Panel::update_filtered_wallpapers() {
    filtered_wp_indices_.clear();
    for (size_t i = 0; i < wallpapers_.size(); ++i) {
        if (wp_group_ != "All" && wallpapers_[i].group != wp_group_) continue;
        filtered_wp_indices_.push_back(i);
    }
    wp_scroll_row_ = 0;
}

const ImageBuffer& Panel::get_wallpaper_thumb(const std::string& path) {
    auto it = wp_thumbs_.find(path);
    if (it != wp_thumbs_.end()) return it->second;

    ImageBuffer thumb = load_image_cover_scaled(path, kWpThumbW, kWpThumbH);
    auto [ins_it, ok] = wp_thumbs_.emplace(path, std::move(thumb));
    (void)ok;
    return ins_it->second;
}

void Panel::advance_cycle_wallpaper() {
    if (wallpapers_.empty()) return;
    for (size_t step = 0; step < wallpapers_.size(); ++step) {
        cycle_index_ = (cycle_index_ + 1) % wallpapers_.size();
        const std::string& cand = wallpapers_[cycle_index_].path;
        if (cand != kWallpaperCycleToken) {
            write_cycle_active_wallpaper(cand);
            rebuild_background_strip();
            current_wp_path_ = kWallpaperCycleToken;
            redraw();

            if (lunar_wm_action_atom_ != XCB_ATOM_NONE) {
                xcb_client_message_event_t msg{};
                msg.response_type = XCB_CLIENT_MESSAGE;
                msg.format = 32;
                msg.window = screen_->root;
                msg.type = lunar_wm_action_atom_;
                msg.data.data32[0] = ipc::kWmReloadWallpaper;
                xcb_send_event(conn_, 0, screen_->root,
                               XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                               reinterpret_cast<const char*>(&msg));
            }
            xcb_flush(conn_);
            return;
        }
    }
}

void Panel::apply_selected_wallpaper(const std::string& path) {
    if (path == kWallpaperCycleToken) {
        save_wallpaper_path(kWallpaperCycleToken);
        wallpaper_cycle_ = true;
        last_cycle_time_ = std::time(nullptr);
        advance_cycle_wallpaper();
        return;
    }

    wallpaper_cycle_ = false;
    save_wallpaper_path(path);
    current_wp_path_ = path;
    rebuild_background_strip();
    redraw();

    if (lunar_wm_action_atom_ != XCB_ATOM_NONE) {
        xcb_client_message_event_t msg{};
        msg.response_type = XCB_CLIENT_MESSAGE;
        msg.format = 32;
        msg.window = screen_->root;
        msg.type = lunar_wm_action_atom_;
        msg.data.data32[0] = ipc::kWmReloadWallpaper;
        xcb_send_event(conn_, 0, screen_->root,
                       XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char*>(&msg));
    }
    xcb_flush(conn_);
}

void Panel::handle_wallpaper_picker_click(int16_t x, int16_t y, uint8_t button) {
    const int total_rows = static_cast<int>(
        (filtered_wp_indices_.size() + kWpCols - 1) / kWpCols);
    const int max_scroll = std::max(0, total_rows - kWpVisibleRows);

    if (button == XCB_BUTTON_INDEX_4) {
        wp_scroll_row_ = std::max(0, wp_scroll_row_ - 1);
        return;
    }
    if (button == XCB_BUTTON_INDEX_5) {
        wp_scroll_row_ = std::min(max_scroll, wp_scroll_row_ + 1);
        return;
    }
    if (button != XCB_BUTTON_INDEX_1 && button != XCB_BUTTON_INDEX_2 &&
        button != XCB_BUTTON_INDEX_3) {
        return;
    }

    // Top-right Close (X) button in Wallpaper Picker header
    if (y >= 14 && y <= 44 && x >= popup_w_ - kAppPadX - 28 && x <= popup_w_ - kAppPadX + 4) {
        close_popup();
        return;
    }

    // Group tabs hit check
    if (y >= kWpTabY && y <= kWpTabY + kWpTabH) {
        const FontRenderer& f_bold = has_bold_font_ ? font_bold_ : font_regular_;
        int cx = kAppPadX;
        for (const char* grp : kWpGroups) {
            const int cw = f_bold.measure_text(grp, 11) + 20;
            if (x >= cx && x <= cx + cw) {
                wp_group_ = grp;
                update_filtered_wallpapers();
                return;
            }
            cx += cw + 8;
        }
        return;
    }

    // Thumbnail grid hit check
    if (y >= kWpGridY && x >= kAppPadX && x < kAppPadX + kWpCols * kWpCellW) {
        const int col = (x - kAppPadX) / kWpCellW;
        const int vis_row = (y - kWpGridY) / kWpCellH;
        if (col >= 0 && col < kWpCols && vis_row >= 0 && vis_row < kWpVisibleRows) {
            const int idx = (wp_scroll_row_ + vis_row) * kWpCols + col;
            if (idx >= 0 && static_cast<size_t>(idx) < filtered_wp_indices_.size()) {
                const std::string chosen = wallpapers_[filtered_wp_indices_[idx]].path;
                apply_selected_wallpaper(chosen);
            }
        }
    }
}

void Panel::draw_wallpaper_picker() {
    const FontRenderer& f_bold = has_bold_font_ ? font_bold_ : font_regular_;
    const FontRenderer& f_reg = has_regular_font_ ? font_regular_ : font_bold_;

    f_bold.draw_text(popup_canvas_, kAppPadX, 34, "Wallpapers & Custom Backgrounds", 15, 0xf8fafc);
    const std::string count_str = std::to_string(filtered_wp_indices_.size()) + " wallpapers";
    const int cw_count = f_reg.measure_text(count_str, 11);
    f_reg.draw_text(popup_canvas_, popup_w_ - kAppPadX - cw_count - 36, 33, count_str, 11, 0x94a3b8);

    // Top-right Close (X) button
    {
        const float cx = static_cast<float>(popup_w_ - kAppPadX - 13);
        const float cy = 28.0f;
        const bool hov = (popup_hover_x_ >= popup_w_ - kAppPadX - 28 &&
                          popup_hover_y_ >= 14 && popup_hover_y_ <= 44);
        fill_circle_aa(popup_canvas_, cx, cy, 13.0f, hov ? 0xef4444 : 0x262d3b, 1.0f);
        const uint32_t xcol = hov ? 0xffffff : 0xcbd5e1;
        draw_line_aa(popup_canvas_, cx - 4.0f, cy - 4.0f, cx + 4.0f, cy + 4.0f, 1.6f, xcol, 0.95f);
        draw_line_aa(popup_canvas_, cx + 4.0f, cy - 4.0f, cx - 4.0f, cy + 4.0f, 1.6f, xcol, 0.95f);
    }

    // Group tabs ("16 Free", "Zorin OS", "Custom", "All")
    int cx = kAppPadX;
    for (const char* grp : kWpGroups) {
        const bool active = (wp_group_ == grp);
        const int cw = f_bold.measure_text(grp, 11) + 20;
        fill_rounded_rect_aa(popup_canvas_, static_cast<float>(cx), static_cast<float>(kWpTabY),
                             static_cast<float>(cw), static_cast<float>(kWpTabH),
                             13.0f, active ? 0x2563eb : 0x232936, 1.0f);
        f_bold.draw_text(popup_canvas_, cx + 10, kWpTabY + 17, grp, 11,
                         active ? 0xffffff : 0xcbd5e1);
        cx += cw + 8;
    }

    if (filtered_wp_indices_.empty()) {
        f_reg.draw_text(popup_canvas_, kAppPadX, kWpGridY + 48,
                        "No custom wallpapers found yet. Add .jpg or .png images to ~/Pictures",
                        13, 0x94a3b8);
        return;
    }

    const int start_idx = wp_scroll_row_ * kWpCols;
    for (int r = 0; r < kWpVisibleRows; ++r) {
        for (int c = 0; c < kWpCols; ++c) {
            const int idx = start_idx + r * kWpCols + c;
            if (idx < 0 || static_cast<size_t>(idx) >= filtered_wp_indices_.size()) break;

            const WallpaperEntry& wp = wallpapers_[filtered_wp_indices_[idx]];
            const int cell_x = kAppPadX + c * kWpCellW;
            const int cell_y = kWpGridY + r * kWpCellH;
            const int tx = cell_x + (kWpCellW - kWpThumbW) / 2;
            const int ty = cell_y + 4;
            const bool is_current = (wp.path == current_wp_path_) ||
                                    (wallpaper_cycle_ && wp.path == kWallpaperCycleToken);
            const bool hov = (popup_hover_x_ >= cell_x && popup_hover_x_ < cell_x + kWpCellW &&
                              popup_hover_y_ >= cell_y && popup_hover_y_ < cell_y + kWpCellH);

            if (hov) {
                fill_rounded_rect_aa(popup_canvas_, static_cast<float>(cell_x + 2),
                                     static_cast<float>(cell_y),
                                     static_cast<float>(kWpCellW - 4),
                                     static_cast<float>(kWpCellH - 4),
                                     10.0f, 0x232a38, 1.0f);
            }

            if (wp.path == kWallpaperCycleToken) {
                fill_rounded_rect_aa(popup_canvas_, static_cast<float>(tx), static_cast<float>(ty),
                                     static_cast<float>(kWpThumbW), static_cast<float>(kWpThumbH),
                                     7.0f, 0x16243b, 1.0f);
                const float rcx = tx + kWpThumbW * 0.5f;
                const float rcy = ty + kWpThumbH * 0.5f - 4.0f;
                draw_arc_aa(popup_canvas_, rcx, rcy, 15.0f, -45.0f, 225.0f, 2.4f, 0x38bdf8, 0.95f);
                draw_line_aa(popup_canvas_, rcx + 8.0f, rcy - 13.0f, rcx + 12.0f, rcy - 6.0f, 2.2f, 0x38bdf8, 0.95f);
                draw_line_aa(popup_canvas_, rcx + 15.0f, rcy - 11.0f, rcx + 12.0f, rcy - 6.0f, 2.2f, 0x38bdf8, 0.95f);
                f_reg.draw_text(popup_canvas_, tx + 24, ty + kWpThumbH - 10, "Auto-Rotate All", 11, 0x93c5fd);
            } else {
                const ImageBuffer& thumb = get_wallpaper_thumb(wp.path);
                if (thumb.width == kWpThumbW && thumb.height == kWpThumbH &&
                    thumb.pixels.size() == static_cast<size_t>(kWpThumbW) * kWpThumbH) {
                    for (int py = 0; py < kWpThumbH; ++py) {
                        for (int px = 0; px < kWpThumbW; ++px) {
                            // Clip rounded corners on thumbnail (r=7)
                            const float dx = std::max(0.0f, std::fabs((px + 0.5f) - kWpThumbW * 0.5f) - (kWpThumbW * 0.5f - 7.0f));
                            const float dy = std::max(0.0f, std::fabs((py + 0.5f) - kWpThumbH * 0.5f) - (kWpThumbH * 0.5f - 7.0f));
                            const float dist = std::hypot(dx, dy) - 7.0f;
                            if (dist > 0.5f) continue;
                            const float cov = std::clamp(0.5f - dist, 0.0f, 1.0f);
                            const uint32_t col = thumb.pixels[static_cast<size_t>(py) * kWpThumbW + px];
                            blend_pixel(popup_canvas_, tx + px, ty + py, col, cov);
                        }
                    }
                }
            }

            if (is_current) {
                stroke_rounded_rect_aa(popup_canvas_, static_cast<float>(tx - 1),
                                       static_cast<float>(ty - 1),
                                       static_cast<float>(kWpThumbW + 2),
                                       static_cast<float>(kWpThumbH + 2),
                                       8.0f, 2.2f, 0x38bdf8, 1.0f);
            } else {
                stroke_rounded_rect_aa(popup_canvas_, static_cast<float>(tx),
                                       static_cast<float>(ty),
                                       static_cast<float>(kWpThumbW),
                                       static_cast<float>(kWpThumbH),
                                       7.0f, 1.0f, 0xffffff, 0.12f);
            }

            const std::string label = ellipsize(f_reg, wp.name, 11, kWpThumbW);
            const int lw = f_reg.measure_text(label, 11);
            f_reg.draw_text(popup_canvas_, cell_x + (kWpCellW - lw) / 2,
                            ty + kWpThumbH + 17, label, 11,
                            is_current ? 0x38bdf8 : 0xe2e8f0);
        }
    }

    const int total_rows = static_cast<int>(
        (filtered_wp_indices_.size() + kWpCols - 1) / kWpCols);
    if (total_rows > kWpVisibleRows) {
        const float track_h = static_cast<float>(kWpVisibleRows * kWpCellH - 12);
        const float thumb_h = std::max(28.0f, track_h * (static_cast<float>(kWpVisibleRows) / total_rows));
        const float max_scroll = static_cast<float>(total_rows - kWpVisibleRows);
        const float thumb_y = static_cast<float>(kWpGridY + 6) +
                              (track_h - thumb_h) * (static_cast<float>(wp_scroll_row_) / max_scroll);
        fill_rounded_rect_aa(popup_canvas_, static_cast<float>(popup_w_ - 10),
                             thumb_y, 4.0f, thumb_h, 2.0f, 0x64748b, 0.85f);
    }

    f_reg.draw_text(popup_canvas_, kAppPadX, popup_h_ - 12,
                    "Click any wallpaper to apply  |  Put custom .jpg/.png in ~/Pictures  |  Esc to close",
                    11, 0x64748b);
}

void Panel::refresh_current_desktop() {
    if (!ewmh_initialized_) return;
    uint32_t num = 2;
    if (xcb_ewmh_get_number_of_desktops_reply(
            &ewmh_, xcb_ewmh_get_number_of_desktops(&ewmh_, screen_idx_), &num, nullptr)) {
        num_desktops_ = std::max<uint32_t>(1, std::min<uint32_t>(4, num));
    }
    uint32_t cur = 0;
    if (xcb_ewmh_get_current_desktop_reply(
            &ewmh_, xcb_ewmh_get_current_desktop(&ewmh_, screen_idx_), &cur, nullptr)) {
        current_desktop_ = cur % num_desktops_;
    }
}

void Panel::send_switch_desktop(uint32_t target) {
    if (!ewmh_initialized_) return;
    xcb_ewmh_request_change_current_desktop(&ewmh_, screen_idx_, target, XCB_CURRENT_TIME);
    xcb_flush(conn_);
}

void Panel::send_wm_exit() {
    if (lunar_wm_action_atom_ != XCB_ATOM_NONE) {
        xcb_client_message_event_t msg{};
        msg.response_type = XCB_CLIENT_MESSAGE;
        msg.format = 32;
        msg.window = screen_->root;
        msg.type = lunar_wm_action_atom_;
        msg.data.data32[0] = ipc::kWmExit;
        xcb_send_event(conn_, 0, screen_->root,
                       XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char*>(&msg));
        xcb_flush(conn_);
    }
    running_ = false;
}

void Panel::send_wm_reload_desktop_icons() {
    if (lunar_wm_action_atom_ == XCB_ATOM_NONE) return;
    xcb_client_message_event_t msg{};
    msg.response_type = XCB_CLIENT_MESSAGE;
    msg.format = 32;
    msg.window = screen_->root;
    msg.type = lunar_wm_action_atom_;
    msg.data.data32[0] = ipc::kWmReloadDesktopIcons;
    xcb_send_event(conn_, 0, screen_->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char*>(&msg));
    xcb_flush(conn_);
}

void Panel::send_toggle_minimize(xcb_window_t win) {
    if (lunar_wm_action_atom_ == XCB_ATOM_NONE) return;
    xcb_client_message_event_t msg{};
    msg.response_type = XCB_CLIENT_MESSAGE;
    msg.format = 32;
    msg.window = screen_->root;
    msg.type = lunar_wm_action_atom_;
    msg.data.data32[0] = ipc::kWmToggleMinimize;
    msg.data.data32[1] = win;
    xcb_send_event(conn_, 0, screen_->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char*>(&msg));
    xcb_flush(conn_);
}

void Panel::send_activate_window(xcb_window_t win) {
    if (!ewmh_initialized_) return;
    xcb_client_message_event_t msg{};
    msg.response_type = XCB_CLIENT_MESSAGE;
    msg.format = 32;
    msg.window = win;
    msg.type = ewmh_._NET_ACTIVE_WINDOW;
    msg.data.data32[0] = 2; // pager / panel source
    msg.data.data32[1] = XCB_CURRENT_TIME;
    xcb_send_event(conn_, 0, screen_->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char*>(&msg));
    xcb_flush(conn_);
}

void Panel::send_close_window(xcb_window_t win) {
    if (!ewmh_initialized_) return;
    xcb_client_message_event_t msg{};
    msg.response_type = XCB_CLIENT_MESSAGE;
    msg.format = 32;
    msg.window = win;
    msg.type = ewmh_._NET_CLOSE_WINDOW;
    msg.data.data32[0] = XCB_CURRENT_TIME;
    msg.data.data32[1] = 2;
    xcb_send_event(conn_, 0, screen_->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char*>(&msg));
    xcb_flush(conn_);
}

void Panel::refresh_client_list() {
    task_items_.clear();
    if (!ewmh_initialized_) return;

    xcb_ewmh_get_windows_reply_t list{};
    auto cookie = xcb_ewmh_get_client_list(&ewmh_, screen_idx_);
    if (!xcb_ewmh_get_client_list_reply(&ewmh_, cookie, &list, nullptr)) {
        return;
    }

    for (uint32_t i = 0; i < list.windows_len; ++i) {
        const xcb_window_t w = list.windows[i];
        if (w == win_ || w == popup_win_) continue;

        TaskItem item;
        item.window = w;
        item.title = read_window_title(w);

        xcb_ewmh_get_atoms_reply_t states{};
        auto st_cookie = xcb_ewmh_get_wm_state(&ewmh_, w);
        if (xcb_ewmh_get_wm_state_reply(&ewmh_, st_cookie, &states, nullptr)) {
            for (uint32_t j = 0; j < states.atoms_len; ++j) {
                if (states.atoms[j] == ewmh_._NET_WM_STATE_HIDDEN) {
                    item.minimized = true;
                    break;
                }
            }
            xcb_ewmh_get_atoms_reply_wipe(&states);
        }
        task_items_.push_back(std::move(item));
    }
    xcb_ewmh_get_windows_reply_wipe(&list);
}

void Panel::refresh_active_window() {
    xcb_window_t next_active = XCB_WINDOW_NONE;
    if (ewmh_initialized_) {
        auto cookie = xcb_ewmh_get_active_window(&ewmh_, screen_idx_);
        if (!xcb_ewmh_get_active_window_reply(&ewmh_, cookie, &next_active, nullptr)) {
            next_active = XCB_WINDOW_NONE;
        }
    }

    if (next_active == win_ || next_active == popup_win_) {
        next_active = XCB_WINDOW_NONE;
    }

    if (active_window_ != XCB_WINDOW_NONE && active_window_ != next_active) {
        const uint32_t no_mask = XCB_EVENT_MASK_NO_EVENT;
        xcb_change_window_attributes(conn_, active_window_, XCB_CW_EVENT_MASK, &no_mask);
    }

    active_window_ = next_active;
    if (active_window_ != XCB_WINDOW_NONE) {
        const uint32_t prop_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
        xcb_change_window_attributes(conn_, active_window_, XCB_CW_EVENT_MASK, &prop_mask);
        active_title_ = read_window_title(active_window_);
    } else {
        active_title_.clear();
    }
}

std::string Panel::read_window_title(xcb_window_t win) const {
    if (win == XCB_WINDOW_NONE) return {};

    if (ewmh_initialized_) {
        xcb_ewmh_get_utf8_strings_reply_t name_reply{};
        auto cookie = xcb_ewmh_get_wm_name(&ewmh_, win);
        if (xcb_ewmh_get_wm_name_reply(&ewmh_, cookie, &name_reply, nullptr)) {
            std::string title(name_reply.strings, name_reply.strings_len);
            xcb_ewmh_get_utf8_strings_reply_wipe(&name_reply);
            if (!title.empty()) return title;
        }
    }

    xcb_icccm_get_text_property_reply_t icccm_name{};
    auto icccm_cookie = xcb_icccm_get_wm_name(conn_, win);
    if (xcb_icccm_get_wm_name_reply(conn_, icccm_cookie, &icccm_name, nullptr)) {
        std::string title(icccm_name.name, icccm_name.name_len);
        xcb_icccm_get_text_property_reply_wipe(&icccm_name);
        if (!title.empty()) return title;
    }

    const xcb_atom_t prop =
        ewmh_initialized_ ? ewmh_._NET_WM_NAME : static_cast<xcb_atom_t>(XCB_ATOM_WM_NAME);
    auto raw_cookie = xcb_get_property(conn_, 0, win, prop, XCB_GET_PROPERTY_TYPE_ANY, 0, 256);
    UniqueMalloc<xcb_get_property_reply_t> raw_reply(
        xcb_get_property_reply(conn_, raw_cookie, nullptr));
    if (raw_reply && xcb_get_property_value_length(raw_reply.get()) > 0) {
        return std::string(static_cast<const char*>(xcb_get_property_value(raw_reply.get())),
                           static_cast<size_t>(xcb_get_property_value_length(raw_reply.get())));
    }

    return "Window";
}

std::string Panel::format_clock() const {
    const std::time_t now = std::time(nullptr);
    std::tm tm_now{};
    localtime_r(&now, &tm_now);
    char buf[64]{};
    std::strftime(buf, sizeof(buf), "%b %e  %H:%M", &tm_now);
    std::string out(buf);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

void Panel::draw_status_icons(int right_edge_x) {
    const float cy = static_cast<float>(height_) * 0.5f;
    const uint32_t fg = 0xebedf0;

    // Subtle highlight pill behind top-right Quick Settings cluster when open
    const float cluster_w = 82.0f;
    const float cluster_x = static_cast<float>(right_edge_x) - cluster_w;
    tray_hit_min_x_ = static_cast<int16_t>(cluster_x);
    if (popup_kind_ == PopupKind::QuickSettings) {
        fill_rounded_rect_aa(canvas_, cluster_x, 3.0f, cluster_w,
                             static_cast<float>(height_ - 6), 6.0f, 0xffffff, 0.16f);
    }

    // 1. Power icon (rightmost)
    const float pwr_cx = static_cast<float>(right_edge_x - 16);
    draw_arc_aa(canvas_, pwr_cx, cy + 0.5f, 5.0f, -55.0f, 180.0f, 1.5f, fg, 0.92f);
    draw_arc_aa(canvas_, pwr_cx, cy + 0.5f, 5.0f, -180.0f, -125.0f, 1.5f, fg, 0.92f);
    draw_line_aa(canvas_, pwr_cx, cy - 6.0f, pwr_cx, cy + 0.2f, 1.6f, fg, 0.95f);

    // 2. Speaker / volume icon
    const float spk_x = pwr_cx - 26.0f;
    fill_rounded_rect_aa(canvas_, spk_x - 5.0f, cy - 2.5f, 4.0f, 5.0f, 1.0f, fg, 0.92f);
    draw_line_aa(canvas_, spk_x - 2.0f, cy - 2.0f, spk_x + 2.0f, cy - 5.0f, 1.6f, fg, 0.92f);
    draw_line_aa(canvas_, spk_x + 2.0f, cy - 5.0f, spk_x + 2.0f, cy + 5.0f, 1.6f, fg, 0.92f);
    draw_line_aa(canvas_, spk_x - 2.0f, cy + 2.0f, spk_x + 2.0f, cy + 5.0f, 1.6f, fg, 0.92f);
    draw_arc_aa(canvas_, spk_x + 2.5f, cy, 4.0f, -45.0f, 45.0f, 1.3f, fg, 0.85f);
    draw_arc_aa(canvas_, spk_x + 2.5f, cy, 6.8f, -42.0f, 42.0f, 1.3f, fg, 0.70f);

    // 3. Wi-Fi signal icon
    const float wifi_cx = spk_x - 24.0f;
    const float wifi_cy = cy + 4.0f;
    fill_circle_aa(canvas_, wifi_cx, wifi_cy, 1.3f, fg, 0.95f);
    draw_arc_aa(canvas_, wifi_cx, wifi_cy, 4.2f, -135.0f, -45.0f, 1.4f, fg, 0.90f);
    draw_arc_aa(canvas_, wifi_cx, wifi_cy, 7.6f, -135.0f, -45.0f, 1.4f, fg, 0.85f);

    // 4. Terminal launcher icon (">_") just left of the Quick Settings cluster
    const float term_cx = cluster_x - 16.0f;
    term_hit_min_x_ = static_cast<int16_t>(term_cx - 12);
    term_hit_max_x_ = static_cast<int16_t>(term_cx + 11);
    fill_rounded_rect_aa(canvas_, term_cx - 8.5f, cy - 6.0f, 17.0f, 12.0f, 2.5f, 0xffffff, 0.14f);
    draw_line_aa(canvas_, term_cx - 5.0f, cy - 2.5f, term_cx - 2.0f, cy, 1.4f, fg, 0.92f);
    draw_line_aa(canvas_, term_cx - 5.0f, cy + 2.5f, term_cx - 2.0f, cy, 1.4f, fg, 0.92f);
    draw_line_aa(canvas_, term_cx + 0.5f, cy + 2.5f, term_cx + 4.5f, cy + 2.5f, 1.4f, fg, 0.92f);

    // 5. Gemini Live orb icon just left of the Terminal icon
    const float live_cx = term_cx - 26.0f;
    live_hit_min_x_ = static_cast<int16_t>(live_cx - 12);
    live_hit_max_x_ = static_cast<int16_t>(live_cx + 11);
    uint32_t orb_col = 0x64748b;
    float orb_alpha = 0.72f;
    if (live_status_.status == "live" || live_status_.status == "connecting" ||
        live_status_.status == "speaking") {
        orb_col = 0x34d399;
        orb_alpha = 0.96f;
    } else if (live_status_.status == "error" || live_status_.status == "no_key" || live_status_.status == "needs_key") {
        orb_col = 0xf59e0b;
        orb_alpha = 0.92f;
    }
    fill_circle_aa(canvas_, live_cx, cy, 7.5f, orb_col, 0.22f);
    stroke_circle_aa(canvas_, live_cx, cy, 6.2f, 1.4f, orb_col, orb_alpha);
    fill_circle_aa(canvas_, live_cx, cy, 2.6f, orb_col, orb_alpha);
}

void Panel::redraw() {
    if (backbuf_ == XCB_PIXMAP_NONE || gc_ == XCB_NONE) return;

    std::copy(bg_strip_.pixels.begin(), bg_strip_.pixels.end(), canvas_.pixels.begin());

    const float cy = static_cast<float>(height_) * 0.5f;
    const int baseline_y = static_cast<int>(height_ / 2 + 4);
    const FontRenderer& clock_font = has_bold_font_ ? font_bold_ : font_regular_;
    const FontRenderer& title_font = has_regular_font_ ? font_regular_ : font_bold_;
    const FontRenderer& icon_font = has_bold_font_ ? font_bold_ : font_regular_;
    const std::string clock_str = last_clock_.empty() ? format_clock() : last_clock_;
    pinned_hit_items_.clear();

    if (bar_layout_ == BarLayout::Taskbar) {
        // Windows 11 style bottom taskbar:
        // Left: Workspace pill, Center: Start + Pinned Apps + Running Apps, Right: Tray + Clock
        fill_rect_alpha(canvas_, 0, 0, width_, 1, 0xffffff, 0.14f);

        // 1. Left Workspace indicator pill
        float ws_x = 14.0f;
        ws_hit_min_x_ = 8;
        for (uint32_t i = 0; i < num_desktops_; ++i) {
            if (i == current_desktop_) {
                fill_rounded_rect_aa(canvas_, ws_x, cy - 3.5f, 22.0f, 7.0f, 3.5f, 0xf8fafc, 0.95f);
                ws_x += 28.0f;
            } else {
                fill_circle_aa(canvas_, ws_x + 3.5f, cy, 3.2f, 0xd1d5db, 0.55f);
                ws_x += 13.0f;
            }
        }
        ws_hit_max_x_ = static_cast<int16_t>(ws_x + 6.0f);

        // 2. Right Clock + System Tray
        const int clock_w = clock_font.measure_text(clock_str, 12);
        const int clock_x = static_cast<int>(width_) - clock_w - 14;
        clock_font.draw_text(canvas_, clock_x, baseline_y, clock_str, 12, 0xf3f4f6);
        draw_status_icons(clock_x - 10);

        // 3. Centered Windows 11 cluster: Start Grid + Pinned Apps + Running window pills
        const int start_w = 36;
        const int pin_btn_w = 34;
        const int pin_gap = 6;
        const int n_pins = static_cast<int>(taskbar_pins_.size());
        const int pins_w = n_pins > 0 ? (n_pins * pin_btn_w + (n_pins - 1) * pin_gap + 10) : 0;
        const int n_items = static_cast<int>(task_items_.size());
        const int avail_for_tasks = std::max(0, live_hit_min_x_ - ws_hit_max_x_ - start_w - pins_w - 60);
        const int pill_w = n_items > 0 ? std::clamp(136, 64, std::max(64, avail_for_tasks / std::max(1, n_items))) : 0;
        const int gap = 6;
        const int tasks_w = n_items > 0 ? (n_items * pill_w + (n_items - 1) * gap + 12) : 0;
        const int total_center_w = start_w + pins_w + tasks_w;
        int cx = std::max(ws_hit_max_x_ + 16, (static_cast<int>(width_) - total_center_w) / 2);
        if (cx + total_center_w > live_hit_min_x_ - 12) {
            cx = std::max(ws_hit_max_x_ + 12, live_hit_min_x_ - 12 - total_center_w);
        }

        // Start / App Menu button
        apps_hit_min_x_ = static_cast<int16_t>(cx);
        apps_hit_max_x_ = static_cast<int16_t>(cx + start_w);
        fill_rounded_rect_aa(canvas_, static_cast<float>(cx), 6.0f,
                             static_cast<float>(start_w), static_cast<float>(height_ - 12),
                             8.0f, popup_kind_ == PopupKind::AppMenu ? 0x2563eb : 0xffffff,
                             popup_kind_ == PopupKind::AppMenu ? 0.90f : 0.10f);
        const float gx = static_cast<float>(cx) + 11.0f;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                fill_circle_aa(canvas_, gx + c * 6.0f, cy - 6.0f + r * 6.0f,
                               1.6f, 0x38bdf8, 0.96f);
            }
        }

        // Pinned Apps
        int tx = cx + start_w + 10;
        for (const auto& pin_app : taskbar_pins_) {
            if (tx + pin_btn_w > live_hit_min_x_ - 10) break;
            fill_rounded_rect_aa(canvas_, static_cast<float>(tx), 5.0f,
                                 static_cast<float>(pin_btn_w), static_cast<float>(height_ - 10),
                                 8.0f, 0xffffff, 0.08f);
            app_registry_.draw_app_icon(canvas_, pin_app, tx + 5, (height_ - 24) / 2, 24, icon_font);
            pinned_hit_items_.push_back(PinnedItem{pin_app, static_cast<int16_t>(tx),
                                                   static_cast<int16_t>(tx + pin_btn_w)});
            tx += pin_btn_w + pin_gap;
        }

        if (n_pins > 0 && !task_items_.empty() && tx + 10 < live_hit_min_x_) {
            fill_rect_alpha(canvas_, tx + 1, 10, 1, height_ - 20, 0xffffff, 0.16f);
            tx += 8;
        }

        for (auto& item : task_items_) {
            if (tx + pill_w > live_hit_min_x_ - 10) {
                item.x1 = 0;
                item.x2 = -1;
                continue;
            }
            item.x1 = static_cast<int16_t>(tx);
            item.x2 = static_cast<int16_t>(tx + pill_w);

            const bool is_active = (item.window == active_window_ && !item.minimized);
            const float bg_alpha = is_active ? 0.20f : (item.minimized ? 0.05f : 0.11f);
            fill_rounded_rect_aa(canvas_, static_cast<float>(tx), 6.0f,
                                 static_cast<float>(pill_w), static_cast<float>(height_ - 12),
                                 8.0f, 0xffffff, bg_alpha);
            if (is_active) {
                fill_rounded_rect_aa(canvas_, static_cast<float>(tx + pill_w / 2 - 12),
                                     static_cast<float>(height_ - 4),
                                     24.0f, 2.5f, 1.2f, 0x38bdf8, 0.98f);
            } else {
                fill_circle_aa(canvas_, static_cast<float>(tx + pill_w / 2),
                               static_cast<float>(height_ - 3), 1.8f, 0x94a3b8, 0.85f);
            }
            const std::string label = ellipsize(title_font, item.title, 12, pill_w - 16);
            title_font.draw_text(canvas_, tx + 8, baseline_y, label, 12,
                                 item.minimized ? 0x94a3b8 : 0xf1f5f9);
            tx += pill_w + gap;
        }
    } else {
        // 1. Left App Menu 3x3 grid icon
        const float grid_x = 12.0f;
        apps_hit_min_x_ = 0;
        if (popup_kind_ == PopupKind::AppMenu) {
            fill_rounded_rect_aa(canvas_, grid_x - 4.0f, 3.0f, 22.0f,
                                 static_cast<float>(height_ - 6), 5.0f, 0xffffff, 0.18f);
        }
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                fill_circle_aa(canvas_, grid_x + 2.0f + c * 5.0f, cy - 5.0f + r * 5.0f,
                               1.3f, 0xf1f5f9, 0.92f);
            }
        }
        apps_hit_max_x_ = 36;

        // 2. Workspace indicator pill ([===  o])
        ws_hit_min_x_ = 37;
        float ws_x = 44.0f;
        for (uint32_t i = 0; i < num_desktops_; ++i) {
            if (i == current_desktop_) {
                fill_rounded_rect_aa(canvas_, ws_x, cy - 3.5f, 22.0f, 7.0f, 3.5f, 0xf8fafc, 0.95f);
                ws_x += 28.0f;
            } else {
                fill_circle_aa(canvas_, ws_x + 3.5f, cy, 3.2f, 0xd1d5db, 0.55f);
                ws_x += 13.0f;
            }
        }
        ws_hit_max_x_ = static_cast<int16_t>(ws_x + 6.0f);

        // 3. Center module: Clock ("nov 16  16:52")
        const int clock_w = clock_font.measure_text(clock_str, 13);
        const int clock_x = std::max(ws_hit_max_x_ + 12, (static_cast<int>(width_) - clock_w) / 2);
        clock_font.draw_text(canvas_, clock_x, baseline_y, clock_str, 13, 0xf3f4f6);

        // 4. Right module: Terminal + Quick Settings system tray
        draw_status_icons(static_cast<int>(width_) - 6);

        // 5. Pinned apps + Taskbar window pills between workspace pill and centered clock
        int tx = ws_hit_max_x_ + 6;
        if (!taskbar_pins_.empty() && tx + 28 < clock_x) {
            fill_rect_alpha(canvas_, ws_hit_max_x_, 7, 1, height_ - 14, 0xffffff, 0.16f);
            for (const auto& pin_app : taskbar_pins_) {
                if (tx + 24 > clock_x - 10) break;
                fill_rounded_rect_aa(canvas_, static_cast<float>(tx), 4.0f,
                                     22.0f, static_cast<float>(height_ - 8),
                                     5.0f, 0xffffff, 0.08f);
                app_registry_.draw_app_icon(canvas_, pin_app, tx + 2, (height_ - 18) / 2, 18, icon_font);
                pinned_hit_items_.push_back(PinnedItem{pin_app, static_cast<int16_t>(tx),
                                                       static_cast<int16_t>(tx + 22)});
                tx += 26;
            }
        }

        const int avail_task_w = clock_x - tx - 14;
        if (!task_items_.empty() && avail_task_w > 48) {
            fill_rect_alpha(canvas_, tx + 2, 7, 1, height_ - 14, 0xffffff, 0.16f);
            const int n_items = static_cast<int>(task_items_.size());
            const int gap = 6;
            const int pill_w = std::clamp((avail_task_w - gap * (n_items - 1)) / n_items, 48, 150);
            tx += 8;

            for (auto& item : task_items_) {
                if (tx + pill_w > clock_x - 10) {
                    item.x1 = 0;
                    item.x2 = -1;
                    continue;
                }
                item.x1 = static_cast<int16_t>(tx);
                item.x2 = static_cast<int16_t>(tx + pill_w);

                const bool is_active = (item.window == active_window_ && !item.minimized);
                const float bg_alpha = is_active ? 0.18f : (item.minimized ? 0.04f : 0.09f);
                fill_rounded_rect_aa(canvas_, static_cast<float>(tx), 4.0f,
                                     static_cast<float>(pill_w), static_cast<float>(height_ - 8),
                                     6.0f, 0xffffff, bg_alpha);
                if (is_active) {
                    fill_rounded_rect_aa(canvas_, static_cast<float>(tx + 8),
                                         static_cast<float>(height_ - 3),
                                         static_cast<float>(pill_w - 16), 2.0f,
                                         1.0f, 0x38bdf8, 0.95f);
                }

                const std::string label = ellipsize(title_font, item.title, 11, pill_w - 16);
                title_font.draw_text(canvas_, tx + 8, baseline_y, label, 11,
                                     item.minimized ? 0x94a3b8 : 0xf1f5f9);
                tx += pill_w + gap;
            }
        }
    }

    upload_pixels_to_pixmap(conn_, screen_, backbuf_, width_, height_, canvas_.pixels);
    xcb_copy_area(conn_, backbuf_, win_, gc_, 0, 0, 0, 0, width_, height_);
}

void Panel::spawn_command(const char* cmd) {
    const pid_t pid = fork();
    if (pid == 0) {
        setsid();
        prepare_x11_app_env();
        const char* argv[] = {"/bin/sh", "-c", cmd, nullptr};
        execvp(argv[0], const_cast<char* const*>(argv));
        _exit(127);
    }
}

void Panel::spawn_terminal() {
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

namespace {

std::string extract_json_string(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t kpos = json.find(needle);
    if (kpos == std::string::npos) return "";
    const size_t colon = json.find(':', kpos + needle.size());
    if (colon == std::string::npos) return "";
    const size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    std::string out;
    for (size_t i = q1 + 1; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            const char nxt = json[i + 1];
            if (nxt == 'n') out.push_back(' ');
            else out.push_back(nxt);
            ++i;
        } else if (json[i] == '"') {
            break;
        } else {
            out.push_back(json[i]);
        }
    }
    return out;
}

bool extract_json_bool(const std::string& json, const char* key, bool fallback) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t kpos = json.find(needle);
    if (kpos == std::string::npos) return fallback;
    const size_t colon = json.find(':', kpos + needle.size());
    if (colon == std::string::npos) return fallback;
    const size_t val = json.find_first_not_of(" \t\r\n", colon + 1);
    if (val == std::string::npos) return fallback;
    if (json.compare(val, 4, "true") == 0) return true;
    if (json.compare(val, 5, "false") == 0) return false;
    return fallback;
}

std::string clip_text(const std::string& s, size_t max_len) {
    if (s.size() <= max_len) return s;
    if (max_len <= 3) return s.substr(0, max_len);
    return s.substr(0, max_len - 3) + "...";
}

} // namespace

bool Panel::refresh_live_status() {
    std::ifstream in("/tmp/lunar-live-state.json");
    if (!in.is_open()) return false;
    std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (json.empty()) return false;

    LiveStatus next;
    next.status = extract_json_string(json, "status");
    if (next.status.empty()) next.status = "idle";
    next.wake_enabled = extract_json_bool(json, "wake_enabled", true);
    next.user_text = extract_json_string(json, "user_text");
    next.model_text = extract_json_string(json, "model_text");
    next.error = extract_json_string(json, "error");

    const bool changed = (next.status != live_status_.status ||
                          next.wake_enabled != live_status_.wake_enabled ||
                          next.user_text != live_status_.user_text ||
                          next.model_text != live_status_.model_text ||
                          next.error != live_status_.error);
    live_status_ = std::move(next);
    return changed;
}

void Panel::send_live_command(const char* cmd) {
    std::ofstream out("/tmp/lunar-live-cmd", std::ios::app);
    if (out.is_open()) {
        out << cmd << '\n';
        out.flush();
    }
}

void Panel::save_gemini_api_key(const std::string& key) {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::filesystem::path dir;
    if (xdg && *xdg) {
        dir = std::filesystem::path(xdg) / "lunar-de";
    } else if (home && *home) {
        dir = std::filesystem::path(home) / ".config" / "lunar-de";
    } else {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path file = dir / "gemini_api_key";
    std::ofstream out(file, std::ios::trunc);
    if (out.is_open()) {
        out << key << '\n';
        out.flush();
        out.close();
        chmod(file.c_str(), 0600);
        send_live_command("reload_key");
    }
}

void Panel::handle_gemini_live_click(int16_t x, int16_t y, uint8_t button) {
    if (button != XCB_BUTTON_INDEX_1) return;

    // Close button (top-right X)
    if (x >= popup_w_ - 34 && x <= popup_w_ - 10 && y >= 10 && y <= 34) {
        close_popup();
        return;
    }

    // Start/Stop Live button: x=[16, 344], y=[64, 98]
    if (x >= 16 && x <= 344 && y >= 64 && y <= 98) {
        send_live_command("toggle_live");
        return;
    }

    // API key input box: x=[16, 266], y=[244, 276]
    if (x >= 16 && x <= 266 && y >= 244 && y <= 276) {
        editing_api_key_ = true;
        return;
    }

    // Save button: x=[274, 344], y=[244, 276]
    if (x >= 274 && x <= 344 && y >= 244 && y <= 276) {
        if (!api_key_input_.empty()) {
            save_gemini_api_key(api_key_input_);
        } else {
            send_live_command("reload_key");
        }
        editing_api_key_ = false;
        return;
    }

    editing_api_key_ = false;
}

void Panel::draw_gemini_live() {
    const FontRenderer& f_bold = has_bold_font_ ? font_bold_ : font_regular_;
    const FontRenderer& f_reg = has_regular_font_ ? font_regular_ : font_bold_;

    // Header + status orb
    uint32_t status_col = 0x64748b;
    std::string status_label = "Idle";
    if (live_status_.status == "connecting") {
        status_col = 0x34d399;
        status_label = "Connecting to Gemini Live...";
    } else if (live_status_.status == "live") {
        status_col = 0x34d399;
        status_label = "Live - Speak now";
    } else if (live_status_.status == "speaking") {
        status_col = 0x34d399;
        status_label = "Live - Speaking...";
    } else if (live_status_.status == "no_key" || live_status_.status == "needs_key") {
        status_col = 0xf59e0b;
        status_label = "API Key Needed";
    } else if (live_status_.status == "error") {
        status_col = 0xef4444;
        status_label = "Error";
    }

    fill_circle_aa(popup_canvas_, 26.0f, 26.0f, 7.0f, status_col, 0.95f);
    f_bold.draw_text(popup_canvas_, 42, 30, "Lunar Gemini Live", 14, 0xf8fafc);
    f_reg.draw_text(popup_canvas_, 42, 48, status_label, 11, status_col);

    // Close X button
    fill_rounded_rect_aa(popup_canvas_, static_cast<float>(popup_w_ - 32), 12.0f, 20.0f, 20.0f,
                         5.0f, 0xffffff, 0.08f);
    draw_line_aa(popup_canvas_, static_cast<float>(popup_w_ - 26), 18.0f,
                 static_cast<float>(popup_w_ - 18), 26.0f, 1.5f, 0xcbd5e1, 0.9f);
    draw_line_aa(popup_canvas_, static_cast<float>(popup_w_ - 18), 18.0f,
                 static_cast<float>(popup_w_ - 26), 26.0f, 1.5f, 0xcbd5e1, 0.9f);

    // Action button (full-width Start / Stop Live)
    const bool is_live = (live_status_.status == "live" ||
                          live_status_.status == "connecting" ||
                          live_status_.status == "speaking");
    fill_rounded_rect_aa(popup_canvas_, 16.0f, 64.0f, 328.0f, 34.0f, 8.0f,
                         is_live ? 0xef4444 : 0x2563eb, 0.92f);
    f_bold.draw_text(popup_canvas_, is_live ? 148 : 146, 86,
                     is_live ? "Stop Live" : "Start Live", 12, 0xffffff);

    // Transcript box
    fill_rounded_rect_aa(popup_canvas_, 16.0f, 110.0f, 328.0f, 104.0f, 8.0f, 0x10141c, 1.0f);
    stroke_rounded_rect_aa(popup_canvas_, 16.5f, 110.5f, 327.0f, 103.0f, 8.0f, 1.0f, 0x283244, 0.9f);

    const std::string you_line = live_status_.user_text.empty()
        ? "Click Start Live to talk with Gemini."
        : "You: " + clip_text(live_status_.user_text, 44);
    const std::string lunar_line = live_status_.model_text.empty()
        ? "Shortcut: Super + G"
        : "Lunar: " + clip_text(live_status_.model_text, 42);

    f_reg.draw_text(popup_canvas_, 26, 134, you_line, 11, 0xcbd5e1);
    f_reg.draw_text(popup_canvas_, 26, 158, lunar_line, 11, 0x93c5fd);
    if (!live_status_.error.empty()) {
        f_reg.draw_text(popup_canvas_, 26, 198, clip_text(live_status_.error, 46), 10, 0xfbbf24);
    }

    // API Key entry row
    f_reg.draw_text(popup_canvas_, 16, 236, "Gemini API Key (saved locally):", 11, 0x94a3b8);
    fill_rounded_rect_aa(popup_canvas_, 16.0f, 244.0f, 250.0f, 32.0f, 7.0f, 0x222834, 1.0f);
    stroke_rounded_rect_aa(popup_canvas_, 16.5f, 244.5f, 249.0f, 31.0f, 7.0f, 1.0f,
                           editing_api_key_ ? 0x38bdf8 : 0x334155, 0.95f);

    std::string key_display;
    if (api_key_input_.empty()) {
        key_display = editing_api_key_ ? "Type API key & press Enter..." : "Click here to set API key";
    } else {
        const size_t n = api_key_input_.size();
        const std::string tail = n > 4 ? api_key_input_.substr(n - 4) : api_key_input_;
        key_display = std::string(std::min<size_t>(14, n > 4 ? n - 4 : 0), '*') + tail;
        if (editing_api_key_) key_display.push_back('|');
    }
    f_reg.draw_text(popup_canvas_, 26, 265, key_display, 11,
                    api_key_input_.empty() ? 0x64748b : 0xf1f5f9);

    fill_rounded_rect_aa(popup_canvas_, 274.0f, 244.0f, 70.0f, 32.0f, 7.0f, 0x334155, 1.0f);
    f_bold.draw_text(popup_canvas_, 294, 265, "Save", 11, 0xf8fafc);
}

void Panel::load_bar_layout() {
    bar_layout_ = BarLayout::TopBar;
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::filesystem::path file;
    if (xdg && *xdg) {
        file = std::filesystem::path(xdg) / "lunar-de" / "panel_layout";
    } else if (home && *home) {
        file = std::filesystem::path(home) / ".config" / "lunar-de" / "panel_layout";
    }
    if (!file.empty()) {
        std::ifstream in(file);
        std::string val;
        if (std::getline(in, val)) {
            while (!val.empty() && (val.back() == '\n' || val.back() == '\r' || val.back() == ' ')) {
                val.pop_back();
            }
            if (val == "taskbar") {
                bar_layout_ = BarLayout::Taskbar;
            }
        }
    }
    height_ = (bar_layout_ == BarLayout::Taskbar) ? 44 : theme::kPanelHeight;
    panel_y_ = (bar_layout_ == BarLayout::Taskbar)
                   ? static_cast<int16_t>(primary_mon_rect_.y + primary_mon_rect_.height - height_)
                   : primary_mon_rect_.y;
}

void Panel::set_bar_layout(BarLayout layout) {
    if (bar_layout_ == layout) return;
    bar_layout_ = layout;

    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::filesystem::path dir;
    if (xdg && *xdg) {
        dir = std::filesystem::path(xdg) / "lunar-de";
    } else if (home && *home) {
        dir = std::filesystem::path(home) / ".config" / "lunar-de";
    }
    if (!dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::ofstream out(dir / "panel_layout", std::ios::trunc);
        if (out.is_open()) {
            out << (layout == BarLayout::Taskbar ? "taskbar" : "top") << '\n';
        }
    }

    height_ = (bar_layout_ == BarLayout::Taskbar) ? 44 : theme::kPanelHeight;
    panel_y_ = (bar_layout_ == BarLayout::Taskbar)
                   ? static_cast<int16_t>(primary_mon_rect_.y + primary_mon_rect_.height - height_)
                   : primary_mon_rect_.y;

    const uint32_t cfg[] = {
        static_cast<uint32_t>(static_cast<int32_t>(panel_x_)),
        static_cast<uint32_t>(static_cast<int32_t>(panel_y_)),
        static_cast<uint32_t>(width_),
        static_cast<uint32_t>(height_)
    };
    xcb_configure_window(conn_, win_,
                         XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
                             XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT,
                         cfg);
    recreate_backbuffer();
    rebuild_background_strip();
    publish_dock_properties();
    redraw();
    close_popup();
    xcb_flush(conn_);
}

void Panel::handle_appearance_click(int16_t x, int16_t y, uint8_t button) {
    if (button != XCB_BUTTON_INDEX_1) return;

    // Close X
    if (x >= popup_w_ - 36 && x <= popup_w_ - 10 && y >= 10 && y <= 36) {
        close_popup();
        return;
    }

    // Left card: Top Bar (x=[18..224], y=[58..198])
    if (x >= 18 && x <= 224 && y >= 58 && y <= 198) {
        set_bar_layout(BarLayout::TopBar);
        return;
    }

    // Right card: Taskbar (x=[240..446], y=[58..198])
    if (x >= 240 && x <= 446 && y >= 58 && y <= 198) {
        set_bar_layout(BarLayout::Taskbar);
        return;
    }

    // Bottom buttons: Wallpapers [18..154], Swap Monitors [162..302], Settings [310..446]
    if (y >= 212 && y <= 246) {
        if (x >= 18 && x <= 154) {
            open_popup(PopupKind::WallpaperPicker);
        } else if (x >= 162 && x <= 302) {
            const char* xdg = std::getenv("XDG_CONFIG_HOME");
            const char* home = std::getenv("HOME");
            std::string cfg_dir = (xdg && *xdg)
                ? (std::string(xdg) + "/lunar-de")
                : ((home && *home) ? (std::string(home) + "/.config/lunar-de") : "/tmp");
            std::filesystem::create_directories(cfg_dir);
            const std::string script_path = cfg_dir + "/monitor_layout.sh";
            const std::string swap_cmd =
                "python3 -c \""
                "import subprocess, re, pathlib;"
                "out = subprocess.check_output(['xrandr', '--query'], text=True);"
                "mons = re.findall(r'^(\\S+)\\s+connected\\s+(primary\\s+)?(\\d+x\\d+)\\+(\\d+)\\+(\\d+)', out, re.M);"
                "prim = next((m for m in mons if m[1]), mons[0] if mons else None);"
                "secs = [m for m in mons if prim and m[0] != prim[0]];"
                "prim and secs and ("
                "sec := secs[0], "
                "cmd := (f'xrandr --output {sec[0]} --auto --pos 0x0 --output {prim[0]} --primary --auto --right-of {sec[0]}' "
                "if int(sec[3]) >= int(prim[3]) else "
                "f'xrandr --output {prim[0]} --primary --auto --pos 0x0 --output {sec[0]} --auto --right-of {prim[0]}'), "
                "subprocess.run(cmd, shell=True), "
                "pathlib.Path('" + script_path + "').write_text('#!/bin/sh\\n' + cmd + '\\n')"
                ")\" >/dev/null 2>&1";
            if (std::system(swap_cmd.c_str()) == 0) {
                close_popup();
            }
        } else if (x >= 310 && x <= 446) {
            close_popup();
            spawn_command("gnome-control-center");
        }
    }
}

void Panel::draw_appearance() {
    const FontRenderer& f_bold = has_bold_font_ ? font_bold_ : font_regular_;
    const FontRenderer& f_reg = has_regular_font_ ? font_regular_ : font_bold_;

    f_bold.draw_text(popup_canvas_, 18, 34, "Zorin Appearance - Bar Layout", 15, 0xf8fafc);

    // Close X
    {
        const float cx = static_cast<float>(popup_w_ - 24);
        const float cy = 26.0f;
        fill_circle_aa(popup_canvas_, cx, cy, 12.0f, 0x262d3b, 1.0f);
        draw_line_aa(popup_canvas_, cx - 4.0f, cy - 4.0f, cx + 4.0f, cy + 4.0f, 1.5f, 0xcbd5e1, 0.95f);
        draw_line_aa(popup_canvas_, cx + 4.0f, cy - 4.0f, cx - 4.0f, cy + 4.0f, 1.5f, 0xcbd5e1, 0.95f);
    }

    auto draw_layout_card = [&](int x, int y, int w, int h, bool active,
                                bool is_taskbar, const char* title, const char* desc) {
        fill_rounded_rect_aa(popup_canvas_, static_cast<float>(x), static_cast<float>(y),
                             static_cast<float>(w), static_cast<float>(h),
                             10.0f, active ? 0x1e3a5f : 0x1e2430, 1.0f);
        stroke_rounded_rect_aa(popup_canvas_, static_cast<float>(x + 0.5f), static_cast<float>(y + 0.5f),
                               static_cast<float>(w - 1), static_cast<float>(h - 1),
                               10.0f, active ? 2.0f : 1.0f, active ? 0x38bdf8 : 0x334155, 0.95f);

        // Mini desktop preview box
        const int px = x + 14;
        const int py = y + 14;
        const int pw = w - 28;
        const int ph = 78;
        fill_rounded_rect_aa(popup_canvas_, static_cast<float>(px), static_cast<float>(py),
                             static_cast<float>(pw), static_cast<float>(ph),
                             6.0f, 0x0f172a, 1.0f);
        if (is_taskbar) {
            fill_rounded_rect_aa(popup_canvas_, static_cast<float>(px), static_cast<float>(py + ph - 14),
                                 static_cast<float>(pw), 14.0f, 4.0f, 0x1e293b, 1.0f);
            fill_rounded_rect_aa(popup_canvas_, static_cast<float>(px + pw / 2 - 24),
                                 static_cast<float>(py + ph - 11), 48.0f, 8.0f, 3.0f, 0x38bdf8, 0.9f);
        } else {
            fill_rounded_rect_aa(popup_canvas_, static_cast<float>(px), static_cast<float>(py),
                                 static_cast<float>(pw), 11.0f, 4.0f, 0x1e293b, 1.0f);
            fill_rounded_rect_aa(popup_canvas_, static_cast<float>(px + 6), static_cast<float>(py + 3),
                                 14.0f, 5.0f, 2.0f, 0x38bdf8, 0.9f);
            fill_rounded_rect_aa(popup_canvas_, static_cast<float>(px + pw / 2 - 12), static_cast<float>(py + 3),
                                 24.0f, 5.0f, 2.0f, 0xe2e8f0, 0.8f);
        }

        f_bold.draw_text(popup_canvas_, x + 14, y + 112, title, 13, 0xf8fafc);
        f_reg.draw_text(popup_canvas_, x + 14, y + 130, desc, 11, active ? 0x93c5fd : 0x94a3b8);
    };

    draw_layout_card(18, 58, 206, 140, bar_layout_ == BarLayout::TopBar, false,
                     "Top Bar", "Classic Lunar 28px top bar");
    draw_layout_card(240, 58, 206, 140, bar_layout_ == BarLayout::Taskbar, true,
                     "Taskbar (Win 11)", "Centered 44px bottom bar");

    fill_rounded_rect_aa(popup_canvas_, 18.0f, 212.0f, 136.0f, 34.0f, 8.0f, 0x262d3b, 1.0f);
    f_bold.draw_text(popup_canvas_, 34, 234, "Wallpapers (32+)", 11, 0xf1f5f9);

    fill_rounded_rect_aa(popup_canvas_, 162.0f, 212.0f, 140.0f, 34.0f, 8.0f, 0x1e3a5f, 1.0f);
    f_bold.draw_text(popup_canvas_, 174, 234, "Swap Monitors (L/R)", 11, 0x93c5fd);

    fill_rounded_rect_aa(popup_canvas_, 310.0f, 212.0f, 136.0f, 34.0f, 8.0f, 0x262d3b, 1.0f);
    f_bold.draw_text(popup_canvas_, 332, 234, "System Settings", 11, 0xf1f5f9);
}

} // namespace lunar
