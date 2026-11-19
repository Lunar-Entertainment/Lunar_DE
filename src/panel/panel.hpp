#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_keysyms.h>

#include "common/desktop_apps.hpp"
#include "common/image_utils.hpp"
#include "common/xcb_utils.hpp"

namespace lunar {

class Panel {
public:
    Panel() = default;
    ~Panel();

    Panel(const Panel&) = delete;
    Panel& operator=(const Panel&) = delete;

    bool init(const char* display_name = nullptr);
    int run();
    void request_quit() noexcept { running_ = false; }

private:
    enum class PopupKind { None, AppMenu, QuickSettings, ContextMenu, WallpaperPicker, GeminiLive, Appearance, RunDialog };
    enum class BarLayout { TopBar, Taskbar };

    struct LiveStatus {
        std::string status = "listening"; // needs_key, listening, connecting, live, speaking, paused, error
        bool wake_enabled = true;
        std::string user_text;
        std::string model_text;
        std::string error;
    };

    struct QuickStatus {
        int volume_pct = 70;
        bool muted = false;
        bool wifi_enabled = true;
        bool wifi_connected = false;
        std::string wifi_name = "Wi-Fi";
        std::string power_label = "AC Power";
    };

    struct TaskItem {
        xcb_window_t window = XCB_WINDOW_NONE;
        std::string title;
        bool minimized = false;
        int16_t x1 = 0;
        int16_t x2 = 0;
    };

    struct PinnedItem {
        DesktopApp app;
        int16_t x1 = 0;
        int16_t x2 = 0;
    };

    void rebuild_background_strip();
    void create_window_and_gc();
    void create_popup_window();
    void recreate_backbuffer();
    void publish_dock_properties();

    void handle_event(xcb_generic_event_t* event);
    void on_property_notify(const xcb_property_notify_event_t* ev);
    void on_configure_notify(const xcb_configure_notify_event_t* ev);
    void on_button_press(const xcb_button_press_event_t* ev);
    void on_button_release(const xcb_button_release_event_t* ev);
    void on_motion_notify(const xcb_motion_notify_event_t* ev);
    void on_key_press(const xcb_key_press_event_t* ev);
    void on_key_release(const xcb_key_release_event_t* ev);
    void on_client_message(const xcb_client_message_event_t* ev);

    void open_popup(PopupKind kind, int16_t anchor_x = 0, int16_t anchor_y = 0);
    void close_popup();
    bool acquire_popup_keyboard_grab();
    void redraw_popup();
    void draw_app_menu();
    void draw_quick_settings();
    void draw_context_menu();
    void draw_wallpaper_picker();
    void draw_gemini_live();
    void draw_appearance();
    void draw_run_dialog();

    void handle_popup_button_press(const xcb_button_press_event_t* ev);
    void handle_app_menu_click(int16_t x, int16_t y, uint8_t button);
    void handle_quick_settings_click(int16_t x, int16_t y, uint8_t button);
    void handle_context_menu_click(int16_t x, int16_t y);
    void handle_wallpaper_picker_click(int16_t x, int16_t y, uint8_t button);
    void handle_gemini_live_click(int16_t x, int16_t y, uint8_t button);
    void handle_appearance_click(int16_t x, int16_t y, uint8_t button);
    void handle_run_dialog_click(int16_t x, int16_t y, uint8_t button);
    void execute_run_command(const std::string& raw);
    void power_off_system();
    void restart_and_update_lunar();
    void load_bar_layout();
    void set_bar_layout(BarLayout layout);
    bool refresh_live_status();
    void send_live_command(const char* cmd);
    void save_gemini_api_key(const std::string& key);
    void update_filtered_apps();
    void update_filtered_wallpapers();
    void apply_selected_wallpaper(const std::string& path);
    void advance_cycle_wallpaper();
    const ImageBuffer& get_wallpaper_thumb(const std::string& path);

    void refresh_quick_status();
    void set_system_volume(int pct);
    void toggle_system_mute();
    void toggle_wifi();

    void refresh_active_window();
    void refresh_client_list();
    void refresh_current_desktop();
    void send_switch_desktop(uint32_t target);
    void send_toggle_minimize(xcb_window_t win);
    void send_activate_window(xcb_window_t win);
    void send_close_window(xcb_window_t win);
    void send_wm_exit();
    void send_wm_reload_desktop_icons();
    std::string read_window_title(xcb_window_t win) const;
    std::string format_clock() const;

    void draw_status_icons(int right_edge_x);
    void redraw();
    void spawn_command(const char* cmd);
    void spawn_terminal();

    xcb_connection_t* conn_ = nullptr;
    int screen_idx_ = 0;
    xcb_screen_t* screen_ = nullptr;
    mutable xcb_ewmh_connection_t ewmh_{};
    bool ewmh_initialized_ = false;
    xcb_key_symbols_t* keysyms_ = nullptr;

    xcb_window_t win_ = XCB_WINDOW_NONE;
    xcb_pixmap_t backbuf_ = XCB_PIXMAP_NONE;
    xcb_gcontext_t gc_ = XCB_NONE;

    xcb_window_t popup_win_ = XCB_WINDOW_NONE;
    xcb_pixmap_t popup_backbuf_ = XCB_PIXMAP_NONE;
    xcb_gcontext_t popup_gc_ = XCB_NONE;
    ImageBuffer popup_canvas_;
    PopupKind popup_kind_ = PopupKind::None;
    uint16_t popup_w_ = 0;
    uint16_t popup_h_ = 0;
    int16_t popup_hover_x_ = -1;
    int16_t popup_hover_y_ = -1;
    bool dragging_volume_ = false;
    bool popup_kb_grabbed_ = false;
    bool super_tap_pending_ = false;
    xcb_keycode_t super_keycode_ = 0;

    xcb_atom_t lunar_panel_win_atom_ = XCB_ATOM_NONE;
    xcb_atom_t lunar_panel_action_atom_ = XCB_ATOM_NONE;
    xcb_atom_t lunar_wm_action_atom_ = XCB_ATOM_NONE;

    FontRenderer font_bold_;
    FontRenderer font_regular_;
    bool has_bold_font_ = false;
    bool has_regular_font_ = false;
    std::vector<DesktopApp> desktop_icons_;
    std::vector<DesktopApp> taskbar_pins_;
    std::vector<PinnedItem> pinned_hit_items_;

    AppRegistry app_registry_;
    std::vector<size_t> filtered_app_indices_;
    std::string active_category_ = "All";
    std::string search_query_;
    int app_scroll_row_ = 0;
    int selected_app_pos_ = 0;

    std::vector<WallpaperEntry> wallpapers_;
    std::vector<size_t> filtered_wp_indices_;
    std::string wp_group_ = "32 Free";
    std::string current_wp_path_;
    bool wallpaper_cycle_ = false;
    size_t cycle_index_ = 0;
    std::time_t last_cycle_time_ = 0;
    int wp_scroll_row_ = 0;
    std::unordered_map<std::string, ImageBuffer> wp_thumbs_;

    QuickStatus quick_status_;
    LiveStatus live_status_;
    std::string api_key_input_;
    bool editing_api_key_ = false;
    std::string run_query_;

    ImageBuffer bg_strip_;
    ImageBuffer canvas_;

    int16_t panel_x_ = 0;
    int16_t panel_y_ = 0;
    Rect primary_mon_rect_{};
    BarLayout bar_layout_ = BarLayout::TopBar;
    uint16_t width_ = 1280;
    uint16_t height_ = theme::kPanelHeight;
    uint16_t screen_height_ = 720;

    int16_t apps_hit_min_x_ = 0;
    int16_t apps_hit_max_x_ = 38;
    int16_t ws_hit_min_x_ = 39;
    int16_t ws_hit_max_x_ = 96;
    int16_t live_hit_min_x_ = 1108;
    int16_t live_hit_max_x_ = 1136;
    int16_t term_hit_min_x_ = 1140;
    int16_t term_hit_max_x_ = 1170;
    int16_t tray_hit_min_x_ = 1172;

    uint32_t current_desktop_ = 0;
    uint32_t num_desktops_ = 2;
    xcb_window_t active_window_ = XCB_WINDOW_NONE;
    std::string active_title_;
    std::vector<TaskItem> task_items_;
    std::string last_clock_;

    bool running_ = false;
};

} // namespace lunar
