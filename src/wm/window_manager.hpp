#pragma once

#include <csignal>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_keysyms.h>

#include "common/desktop_apps.hpp"
#include "common/image_utils.hpp"
#include "common/xcb_utils.hpp"

namespace lunar {

struct Strut {
    uint32_t left = 0;
    uint32_t right = 0;
    uint32_t top = 0;
    uint32_t bottom = 0;
    uint32_t left_start_y = 0;
    uint32_t left_end_y = 0;
    uint32_t right_start_y = 0;
    uint32_t right_end_y = 0;
    uint32_t top_start_x = 0;
    uint32_t top_end_x = 0;
    uint32_t bottom_start_x = 0;
    uint32_t bottom_end_x = 0;
};

struct ClientWindow {
    xcb_window_t window = XCB_WINDOW_NONE;
    xcb_window_t frame = XCB_WINDOW_NONE;
    xcb_gcontext_t title_gc = XCB_NONE;
    Rect rect{};
    Rect saved_rect{};
    Rect pre_fullscreen_rect{};
    std::string title;
    uint32_t desktop = 0;
    int hover_btn = 0; // 0=none, 1=minimize, 2=maximize, 3=close
    bool decorated = true;
    bool maximized = false;
    bool fullscreen = false;
    bool minimized = false;
    bool mapped = true;
};

class WindowManager {
public:
    WindowManager() = default;
    ~WindowManager();

    WindowManager(const WindowManager&) = delete;
    WindowManager& operator=(const WindowManager&) = delete;

    bool init(const char* display_name = nullptr);
    int run();
    void request_quit() noexcept { running_ = 0; }
    void request_restart() noexcept { restart_requested_ = 1; running_ = 0; }
    bool restart_requested() const noexcept { return restart_requested_ != 0; }

private:
    enum class DragMode { None, Move, Resize };
    enum class SnapZone { None, TopMaximize, LeftHalf, RightHalf };

    void setup_root_visuals();
    void apply_wallpaper(bool animate = false);
    void reload_desktop_icons();
    void draw_desktop_icons(ImageBuffer& img);
    int hit_test_desktop_icon(int16_t x, int16_t y) const;
    void terminate_session();
    void setup_ewmh();
    void setup_keys();
    void setup_randr();
    void scan_existing_windows();

    void grab_keys();
    void grab_client_buttons(xcb_window_t win);

    void handle_event(xcb_generic_event_t* event);
    void on_map_request(const xcb_map_request_event_t* ev);
    void on_configure_request(const xcb_configure_request_event_t* ev);
    void on_destroy_notify(const xcb_destroy_notify_event_t* ev);
    void on_unmap_notify(const xcb_unmap_notify_event_t* ev);
    void on_enter_notify(const xcb_enter_notify_event_t* ev);
    void on_leave_notify(const xcb_leave_notify_event_t* ev);
    void on_expose(const xcb_expose_event_t* ev);
    void on_button_press(const xcb_button_press_event_t* ev);
    void on_motion_notify(const xcb_motion_notify_event_t* ev);
    void on_button_release(const xcb_button_release_event_t* ev);
    void on_key_press(const xcb_key_press_event_t* ev);
    void on_key_release(const xcb_key_release_event_t* ev);
    void on_property_notify(const xcb_property_notify_event_t* ev);
    void on_client_message(const xcb_client_message_event_t* ev);
    void on_configure_notify(const xcb_configure_notify_event_t* ev);
    void on_randr_screen_change();

    bool is_dock_window(xcb_window_t win) const;
    bool should_decorate(xcb_window_t win) const;
    bool read_window_strut(xcb_window_t win, Strut& out) const;
    std::string read_window_title(xcb_window_t win) const;
    uint16_t titlebar_height_for(const ClientWindow& cw) const noexcept;
    int hit_test_titlebar_button(const ClientWindow& cw, int16_t x, int16_t y) const;
    void draw_titlebar(ClientWindow& cw);
    void redraw_all_titlebars();

    void manage_dock(xcb_window_t win);
    void manage_client(xcb_window_t win);
    void unmanage_window(xcb_window_t win);

    void recompute_workarea();
    Rect workarea_for_monitor(const Rect& mon) const noexcept;
    Rect workarea_for_rect(const Rect& r) const noexcept;
    void clamp_rect_to_workarea(Rect& rect) const;
    void configure_client_rect(xcb_window_t win, const Rect& rect, uint16_t border_w = theme::kBorderWidth);
    void begin_window_move(xcb_window_t win, int16_t root_x, int16_t root_y, xcb_timestamp_t time);
    void begin_window_resize(xcb_window_t win, int16_t root_x, int16_t root_y, int dir, xcb_timestamp_t time);
    Rect snap_rect_for(SnapZone zone, const Rect& mon_wa) const noexcept;
    void update_snap_preview(int16_t root_x, int16_t root_y);
    void hide_snap_preview();
    void toggle_maximize(xcb_window_t win);
    void set_maximized(xcb_window_t win, bool enable);
    void snap_window(xcb_window_t win, bool left_half);
    void toggle_fullscreen(xcb_window_t win);
    void set_fullscreen(xcb_window_t win, bool enable);
    void minimize_window(xcb_window_t win);
    void restore_window(xcb_window_t win);
    void move_client_to_desktop(xcb_window_t win, uint32_t target);
    void update_wm_state_property(xcb_window_t win, const ClientWindow& cw);
    void raise_docks();

    void focus_window(xcb_window_t win);
    void switch_desktop(uint32_t target);
    void cycle_focus();
    void close_window(xcb_window_t win);
    void update_client_list();
    void send_panel_action(uint32_t action, uint32_t arg1 = 0, uint32_t arg2 = 0);
    void spawn_terminal();

    xcb_connection_t* conn_ = nullptr;
    int screen_idx_ = 0;
    xcb_screen_t* screen_ = nullptr;
    mutable xcb_ewmh_connection_t ewmh_{};
    bool ewmh_initialized_ = false;
    xcb_key_symbols_t* keysyms_ = nullptr;
    xcb_window_t wm_check_win_ = XCB_WINDOW_NONE;
    xcb_pixmap_t root_pixmap_ = XCB_PIXMAP_NONE;
    std::vector<uint32_t> prev_wallpaper_pixels_;
    ImageBuffer cached_wp_base_;
    std::string cached_wp_key_;

    FontRenderer title_font_;
    bool has_title_font_ = false;
    AppRegistry app_registry_;
    std::vector<DesktopApp> desktop_icons_;
    int selected_desktop_icon_ = -1;
    int last_desktop_click_idx_ = -1;
    xcb_timestamp_t last_desktop_click_time_ = 0;

    uint16_t screen_width_ = 0;
    uint16_t screen_height_ = 0;
    Rect workarea_{};
    std::vector<MonitorInfo> monitors_;

    uint16_t numlock_mask_ = XCB_MOD_MASK_2;
    uint8_t randr_first_event_ = 0;
    bool randr_available_ = false;

    xcb_atom_t wm_protocols_atom_ = XCB_ATOM_NONE;
    xcb_atom_t wm_delete_window_atom_ = XCB_ATOM_NONE;
    xcb_atom_t motif_wm_hints_atom_ = XCB_ATOM_NONE;
    xcb_atom_t gtk_frame_extents_atom_ = XCB_ATOM_NONE;
    xcb_atom_t lunar_panel_win_atom_ = XCB_ATOM_NONE;
    xcb_atom_t lunar_panel_action_atom_ = XCB_ATOM_NONE;
    xcb_atom_t lunar_wm_action_atom_ = XCB_ATOM_NONE;

    std::vector<xcb_window_t> client_order_;
    std::unordered_map<xcb_window_t, ClientWindow> clients_;
    std::unordered_map<xcb_window_t, xcb_window_t> frame_to_client_;
    std::unordered_map<xcb_window_t, int> ignore_unmap_;
    std::unordered_map<xcb_window_t, Strut> docks_;
    uint32_t current_desktop_ = 0;
    xcb_window_t focused_window_ = XCB_WINDOW_NONE;
    uint32_t cascade_offset_ = 0;

    DragMode drag_mode_ = DragMode::None;
    SnapZone snap_zone_ = SnapZone::None;
    Rect snap_mon_wa_{};
    int resize_dir_ = 4; // 0..7 EWMH resize direction (4 = bottom-right)
    xcb_window_t drag_window_ = XCB_WINDOW_NONE;
    xcb_window_t snap_preview_win_ = XCB_WINDOW_NONE;
    int16_t drag_start_root_x_ = 0;
    int16_t drag_start_root_y_ = 0;
    Rect drag_start_rect_{};

    xcb_window_t last_title_click_win_ = XCB_WINDOW_NONE;
    xcb_timestamp_t last_title_click_time_ = 0;

    bool super_tap_pending_ = false;
    xcb_keycode_t super_keycode_ = 0;

    volatile sig_atomic_t running_ = 0;
    volatile sig_atomic_t restart_requested_ = 0;
};

} // namespace lunar
