#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/image_utils.hpp"

namespace lunar {

struct DesktopApp {
    std::string id;
    std::string name;
    std::string comment;
    std::string exec_cmd;
    std::string working_dir;
    std::string icon_name;
    std::string category; // Internet, Office, Graphics, Dev, Media, System, Utilities
    bool terminal = false;
};

struct RgbaIcon {
    uint16_t width = 0;
    uint16_t height = 0;
    // Packed 0xAARRGGBB
    std::vector<uint32_t> pixels;
    bool attempted = false;
};

class AppRegistry {
public:
    void load();
    const std::vector<DesktopApp>& apps() const noexcept { return apps_; }

    void draw_app_icon(ImageBuffer& target, const DesktopApp& app, int x, int y, int size,
                       const FontRenderer& font_bold);
    static void launch_app(const DesktopApp& app);

private:
    void index_icon_directory(const std::string& dir);
    std::string find_png_icon(const std::string& icon_name) const;
    const RgbaIcon& get_or_load_icon(const std::string& icon_name, int target_size);

    std::vector<DesktopApp> apps_;
    std::unordered_map<std::string, std::string> icon_paths_;
    std::unordered_map<std::string, RgbaIcon> icon_cache_;
};

std::string desktop_icons_config_path();
std::vector<DesktopApp> load_desktop_icons(const AppRegistry& registry);
void save_desktop_icons(const std::vector<DesktopApp>& icons);
bool is_on_desktop(const std::vector<DesktopApp>& icons, const std::string& app_id);
bool toggle_desktop_icon(const AppRegistry& registry, const DesktopApp& app);

std::string taskbar_pins_config_path();
std::vector<DesktopApp> load_taskbar_pins(const AppRegistry& registry);
void save_taskbar_pins(const std::vector<DesktopApp>& pins);
bool toggle_taskbar_pin(const AppRegistry& registry, const DesktopApp& app);

} // namespace lunar
