#include "common/desktop_apps.hpp"
#include "common/xcb_utils.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <png.h>
#include <unistd.h>
#include <unordered_set>

namespace fs = std::filesystem;

namespace lunar {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = s.size();
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string clean_exec_line(const std::string& raw) {
    // Strip Flatpak @@u ... @@ / @@ ... @@ forwarding markers and --file-forwarding
    std::string s = raw;
    for (const char* marker : {"--file-forwarding", "@@u", "@@"}) {
        size_t pos = 0;
        const size_t mlen = std::char_traits<char>::length(marker);
        while ((pos = s.find(marker, pos)) != std::string::npos) {
            s.erase(pos, mlen);
        }
    }

    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 1 < s.size()) {
            char next = s[i + 1];
            if (next == '%') {
                out.push_back('%');
                ++i;
                continue;
            }
            if (std::string("fFuUdDnNickvm").find(next) != std::string::npos) {
                ++i;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return trim(out);
}

std::string map_category(const std::string& raw_cats) {
    const std::string low = to_lower(raw_cats);
    if (low.find("network") != std::string::npos ||
        low.find("webbrowser") != std::string::npos ||
        low.find("email") != std::string::npos) {
        return "Internet";
    }
    if (low.find("development") != std::string::npos ||
        low.find("ide") != std::string::npos) {
        return "Dev";
    }
    if (low.find("graphics") != std::string::npos ||
        low.find("photography") != std::string::npos ||
        low.find("3dgraphics") != std::string::npos) {
        return "Graphics";
    }
    if (low.find("audiovideo") != std::string::npos ||
        low.find("audio") != std::string::npos ||
        low.find("video") != std::string::npos ||
        low.find("music") != std::string::npos) {
        return "Media";
    }
    if (low.find("office") != std::string::npos) {
        return "Office";
    }
    if (low.find("settings") != std::string::npos ||
        low.find("system") != std::string::npos ||
        low.find("packagemanager") != std::string::npos) {
        return "System";
    }
    return "Utilities";
}

bool parse_desktop_file(const fs::path& path, DesktopApp& out) {
    std::ifstream in(path);
    if (!in.is_open()) return false;

    bool in_main_group = false;
    bool no_display = false;
    bool hidden = false;
    std::string type;
    std::string raw_cats;

    out = DesktopApp{};
    out.id = path.filename().string();

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            if (line == "[Desktop Entry]") {
                in_main_group = true;
            } else if (in_main_group) {
                break; // Stop before [Desktop Action ...] sections
            }
            continue;
        }
        if (!in_main_group) continue;

        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));

        if (key == "Type") type = val;
        else if (key == "Name" && out.name.empty()) out.name = val;
        else if (key == "Comment" && out.comment.empty()) out.comment = val;
        else if (key == "Exec" && out.exec_cmd.empty()) out.exec_cmd = clean_exec_line(val);
        else if (key == "Path" && out.working_dir.empty()) out.working_dir = val;
        else if (key == "Icon" && out.icon_name.empty()) out.icon_name = val;
        else if (key == "Categories" && raw_cats.empty()) raw_cats = val;
        else if (key == "NoDisplay") no_display = (to_lower(val) == "true");
        else if (key == "Hidden") hidden = (to_lower(val) == "true");
        else if (key == "Terminal") out.terminal = (to_lower(val) == "true");
    }

    if (type != "Application" || no_display || hidden || out.name.empty() || out.exec_cmd.empty()) {
        return false;
    }

    out.category = map_category(raw_cats);
    return true;
}

RgbaIcon load_png_scaled(const std::string& path, int target_size) {
    RgbaIcon out;
    out.attempted = true;
    if (path.empty() || target_size <= 0) return out;

    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return out;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) {
        std::fclose(fp);
        return out;
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        std::fclose(fp);
        return out;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(fp);
        return out;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    const png_uint_32 src_w = png_get_image_width(png, info);
    const png_uint_32 src_h = png_get_image_height(png, info);
    const png_byte color_type = png_get_color_type(png, info);
    const png_byte bit_depth = png_get_bit_depth(png, info);

    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }

    png_read_update_info(png, info);
    if (src_w == 0 || src_h == 0 || src_w > 2048 || src_h > 2048) {
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(fp);
        return out;
    }

    std::vector<uint8_t> rgba(static_cast<size_t>(src_w) * src_h * 4);
    std::vector<png_bytep> row_ptrs(src_h);
    for (png_uint_32 y = 0; y < src_h; ++y) {
        row_ptrs[y] = rgba.data() + static_cast<size_t>(y) * src_w * 4;
    }
    png_read_image(png, row_ptrs.data());
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(fp);

    out.width = static_cast<uint16_t>(target_size);
    out.height = static_cast<uint16_t>(target_size);
    out.pixels.resize(static_cast<size_t>(target_size) * target_size);

    for (int y = 0; y < target_size; ++y) {
        const double sy = ((y + 0.5) * src_h) / target_size - 0.5;
        const uint32_t y0 = static_cast<uint32_t>(std::clamp(sy, 0.0, static_cast<double>(src_h - 1)));
        const uint32_t y1 = std::min(y0 + 1, src_h - 1);
        const double fy = std::max(0.0, sy - y0);

        for (int x = 0; x < target_size; ++x) {
            const double sx = ((x + 0.5) * src_w) / target_size - 0.5;
            const uint32_t x0 = static_cast<uint32_t>(std::clamp(sx, 0.0, static_cast<double>(src_w - 1)));
            const uint32_t x1 = std::min(x0 + 1, src_w - 1);
            const double fx = std::max(0.0, sx - x0);

            const size_t i00 = (static_cast<size_t>(y0) * src_w + x0) * 4;
            const size_t i10 = (static_cast<size_t>(y0) * src_w + x1) * 4;
            const size_t i01 = (static_cast<size_t>(y1) * src_w + x0) * 4;
            const size_t i11 = (static_cast<size_t>(y1) * src_w + x1) * 4;

            const double w00 = (1.0 - fx) * (1.0 - fy);
            const double w10 = fx * (1.0 - fy);
            const double w01 = (1.0 - fx) * fy;
            const double w11 = fx * fy;

            const uint32_t r = static_cast<uint32_t>(rgba[i00] * w00 + rgba[i10] * w10 + rgba[i01] * w01 + rgba[i11] * w11);
            const uint32_t g = static_cast<uint32_t>(rgba[i00 + 1] * w00 + rgba[i10 + 1] * w10 + rgba[i01 + 1] * w01 + rgba[i11 + 1] * w11);
            const uint32_t b = static_cast<uint32_t>(rgba[i00 + 2] * w00 + rgba[i10 + 2] * w10 + rgba[i01 + 2] * w01 + rgba[i11 + 2] * w11);
            const uint32_t a = static_cast<uint32_t>(rgba[i00 + 3] * w00 + rgba[i10 + 3] * w10 + rgba[i01 + 3] * w01 + rgba[i11 + 3] * w11);

            out.pixels[static_cast<size_t>(y) * target_size + x] =
                (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    return out;
}

} // namespace

void AppRegistry::load() {
    apps_.clear();
    icon_paths_.clear();
    icon_cache_.clear();

    const char* home = std::getenv("HOME");
    std::vector<std::string> app_dirs;
    if (home && *home) {
        app_dirs.push_back(std::string(home) + "/.local/share/applications");
        app_dirs.push_back(std::string(home) + "/.local/share/flatpak/exports/share/applications");
    }
    app_dirs.push_back("/var/lib/flatpak/exports/share/applications");
    app_dirs.push_back("/usr/share/applications");
    app_dirs.push_back("/var/lib/snapd/desktop/applications");

    std::unordered_set<std::string> seen_ids;
    std::unordered_set<std::string> seen_names;

    for (const auto& dir : app_dirs) {
        std::error_code ec;
        if (!fs::exists(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            if (entry.path().extension() != ".desktop") continue;
            const std::string id = entry.path().filename().string();
            if (seen_ids.contains(id)) continue;

            DesktopApp app;
            if (parse_desktop_file(entry.path(), app)) {
                const std::string name_key = to_lower(app.name);
                if (seen_names.contains(name_key)) continue;
                seen_ids.insert(id);
                seen_names.insert(name_key);
                apps_.push_back(std::move(app));
            }
        }
    }

    std::sort(apps_.begin(), apps_.end(), [](const DesktopApp& a, const DesktopApp& b) {
        return to_lower(a.name) < to_lower(b.name);
    });

    // Index PNG/SVG/XPM icon files in priority order (PNG preferred over SVG/XPM)
    if (home && *home) {
        index_icon_directory(std::string(home) + "/.local/share/icons");
        index_icon_directory(std::string(home) + "/.local/share/flatpak/exports/share/icons");
    }
    const std::array<const char*, 21> icon_dirs = {
        "/var/lib/flatpak/exports/share/icons/hicolor/128x128/apps",
        "/var/lib/flatpak/exports/share/icons/hicolor/64x64/apps",
        "/var/lib/flatpak/exports/share/icons/hicolor/48x48/apps",
        "/var/lib/flatpak/exports/share/icons/hicolor/scalable/apps",
        "/usr/share/icons/Zorin/128x128/apps",
        "/usr/share/icons/Zorin/64x64/apps",
        "/usr/share/icons/Zorin/48x48",
        "/usr/share/icons/Zorin/512x512/apps",
        "/usr/share/icons/Zorin/scalable",
        "/usr/share/icons/ZorinBlue-Light/48x48",
        "/usr/share/icons/ZorinBlue-Dark/48x48",
        "/usr/share/icons/hicolor/128x128/apps",
        "/usr/share/icons/hicolor/64x64/apps",
        "/usr/share/icons/hicolor/48x48/apps",
        "/usr/share/icons/hicolor/256x256/apps",
        "/usr/share/icons/hicolor/512x512/apps",
        "/usr/share/icons/hicolor/scalable/apps",
        "/usr/share/icons/Adwaita/48x48",
        "/usr/share/icons/Adwaita/scalable",
        "/usr/share/icons/gnome/48x48",
        "/usr/share/pixmaps",
    };
    for (const char* dir : icon_dirs) {
        index_icon_directory(dir);
    }

    // Batch-rasterize any uncached SVG/XPM icons in one fast pass
    const char* xdg_cache = std::getenv("XDG_CACHE_HOME");
    std::string cache_dir = (xdg_cache && *xdg_cache)
        ? (std::string(xdg_cache) + "/lunar-de/icons")
        : ((home && *home) ? (std::string(home) + "/.cache/lunar-de/icons") : "/tmp/lunar-de-icons");

    std::string batch;
    for (const auto& app : apps_) {
        if (app.icon_name.empty()) continue;
        std::string src;
        if (app.icon_name.front() == '/') {
            std::error_code ec;
            if (fs::exists(app.icon_name, ec)) {
                src = app.icon_name;
            } else {
                auto it = icon_paths_.find(fs::path(app.icon_name).stem().string());
                if (it != icon_paths_.end()) src = it->second;
            }
        } else {
            auto it = icon_paths_.find(app.icon_name);
            if (it != icon_paths_.end()) src = it->second;
        }
        if (src.empty()) continue;
        const std::string ext = to_lower(fs::path(src).extension().string());
        if (ext == ".svg" || ext == ".xpm") {
            const std::string stem = fs::path(src).stem().string();
            const std::string dst = cache_dir + "/" + stem + "_48.png";
            std::error_code ec;
            if (!fs::exists(dst, ec)) {
                batch += src + "\t" + dst + "\t48\n";
            }
        }
    }
    if (!batch.empty()) {
        std::error_code ec;
        fs::create_directories(cache_dir, ec);
        const char* helper = nullptr;
        if (fs::exists("/usr/local/share/lunar-de/scripts/render_icons.py", ec)) {
            helper = "python3 /usr/local/share/lunar-de/scripts/render_icons.py 2>/dev/null";
        } else if (fs::exists("/home/lunar/Lunar_DE/scripts/render_icons.py", ec)) {
            helper = "python3 /home/lunar/Lunar_DE/scripts/render_icons.py 2>/dev/null";
        }
        if (helper) {
            FILE* pipe = popen(helper, "w");
            if (pipe) {
                std::fwrite(batch.data(), 1, batch.size(), pipe);
                pclose(pipe);
            }
        }
    }
}

void AppRegistry::index_icon_directory(const std::string& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    for (const auto& entry : fs::recursive_directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        const std::string ext = to_lower(entry.path().extension().string());
        if (ext != ".png" && ext != ".svg" && ext != ".xpm") continue;
        const std::string stem = entry.path().stem().string();
        auto it = icon_paths_.find(stem);
        if (it == icon_paths_.end()) {
            icon_paths_[stem] = entry.path().string();
        } else if (ext == ".png" && !to_lower(it->second).ends_with(".png")) {
            // Upgrade SVG/XPM entry to native PNG when available
            it->second = entry.path().string();
        }
    }
}

std::string AppRegistry::find_png_icon(const std::string& icon_name) const {
    if (icon_name.empty()) return {};
    const char* xdg_cache = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    std::string cache_dir = (xdg_cache && *xdg_cache)
        ? (std::string(xdg_cache) + "/lunar-de/icons")
        : ((home && *home) ? (std::string(home) + "/.cache/lunar-de/icons") : "/tmp/lunar-de-icons");

    std::string resolved;
    if (icon_name.front() == '/') {
        std::error_code ec;
        if (fs::exists(icon_name, ec)) {
            resolved = icon_name;
        } else {
            auto it = icon_paths_.find(fs::path(icon_name).stem().string());
            if (it != icon_paths_.end()) resolved = it->second;
        }
    } else {
        auto it = icon_paths_.find(icon_name);
        if (it != icon_paths_.end()) resolved = it->second;
    }
    if (resolved.empty()) return {};

    const std::string ext = to_lower(fs::path(resolved).extension().string());
    if (ext == ".png") return resolved;

    const std::string cached = cache_dir + "/" + fs::path(resolved).stem().string() + "_48.png";
    std::error_code ec;
    if (fs::exists(cached, ec)) return cached;
    return {};
}

const RgbaIcon& AppRegistry::get_or_load_icon(const std::string& icon_name, int target_size) {
    auto it = icon_cache_.find(icon_name);
    if (it != icon_cache_.end()) return it->second;

    const std::string path = find_png_icon(icon_name);
    RgbaIcon loaded = load_png_scaled(path, target_size);
    auto [ins_it, ok] = icon_cache_.emplace(icon_name, std::move(loaded));
    (void)ok;
    return ins_it->second;
}

void AppRegistry::draw_app_icon(ImageBuffer& target, const DesktopApp& app, int x, int y, int size,
                                const FontRenderer& font_bold) {
    const RgbaIcon& icon = get_or_load_icon(app.icon_name, size);
    if (!icon.pixels.empty() && icon.width == size && icon.height == size) {
        for (int py = 0; py < size; ++py) {
            for (int px = 0; px < size; ++px) {
                const uint32_t argb = icon.pixels[static_cast<size_t>(py) * size + px];
                const uint32_t a = (argb >> 24) & 0xFF;
                if (a > 0) {
                    blend_pixel(target, x + px, y + py, argb & 0x00FFFFFF,
                                static_cast<float>(a) / 255.0f);
                }
            }
        }
        return;
    }

    // Fallback: sleek colored monogram badge for SVG-only or missing icons
    static constexpr std::array<uint32_t, 6> kPalette = {
        0x2563eb, 0x0891b2, 0x059669, 0x7c3aed, 0xd97706, 0xdb2777
    };
    const uint32_t badge_col = kPalette[std::hash<std::string>{}(app.name) % kPalette.size()];
    fill_rounded_rect_aa(target, static_cast<float>(x), static_cast<float>(y),
                         static_cast<float>(size), static_cast<float>(size),
                         7.0f, badge_col, 0.92f);

    std::string letter(1, static_cast<char>(std::toupper(static_cast<unsigned char>(app.name[0]))));
    const int lw = font_bold.measure_text(letter, 16);
    font_bold.draw_text(target, x + (size - lw) / 2, y + size / 2 + 6, letter, 16, 0xffffff);
}

void AppRegistry::launch_app(const DesktopApp& app) {
    std::string cmd = app.exec_cmd;
    const char* disp = std::getenv("DISPLAY");
    const std::string display_str = (disp && *disp) ? disp : ":0";

    // Ensure Flatpak apps inherit DISPLAY and X11 backend flags inside sandbox
    const size_t fp_pos = cmd.find("flatpak run");
    if (fp_pos != std::string::npos) {
        const size_t insert_at = fp_pos + 11;
        const std::string extra =
            " --env=DISPLAY=" + display_str +
            " --env=GDK_BACKEND=x11 --env=QT_QPA_PLATFORM=xcb"
            " --env=ELECTRON_OZONE_PLATFORM_HINT=x11 --env=GTK_USE_PORTAL=0";
        cmd.insert(insert_at, extra);
    }

    // Inside Xephyr, isolate Chromium-based browser single-instance socket so windows open in :2
    if (const char* xephyr_env = std::getenv("LUNAR_XEPHYR"); xephyr_env && *xephyr_env == '1') {
        const std::string low = to_lower(cmd);
        if (low.find("brave-browser") != std::string::npos ||
            low.find("google-chrome") != std::string::npos ||
            low.find("chromium") != std::string::npos) {
            cmd += " --user-data-dir=/tmp/lunar-xephyr-browser --no-first-run --new-window";
        }
    }

    const pid_t pid = fork();
    if (pid == 0) {
        setsid();
        prepare_x11_app_env();
        if (!app.working_dir.empty()) {
            if (chdir(app.working_dir.c_str()) != 0) {
                // ignore if directory missing
            }
        }
        if (app.terminal) {
            const char* argv[] = {"x-terminal-emulator", "-e", "sh", "-c", cmd.c_str(), nullptr};
            execvp(argv[0], const_cast<char* const*>(argv));
        } else {
            const char* argv[] = {"/bin/sh", "-c", cmd.c_str(), nullptr};
            execvp(argv[0], const_cast<char* const*>(argv));
        }
        _exit(127);
    }
}

std::string desktop_icons_config_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::string base;
    if (xdg && *xdg) {
        base = xdg;
    } else if (home && *home) {
        base = std::string(home) + "/.config";
    } else {
        return "/tmp/lunar-de-desktop-icons";
    }
    return base + "/lunar-de/desktop_icons";
}

std::vector<DesktopApp> load_desktop_icons(const AppRegistry& registry) {
    const std::string path = desktop_icons_config_path();
    std::vector<DesktopApp> result;

    if (!std::filesystem::exists(path)) {
        // Seed useful defaults on first run
        const char* default_queries[] = {"terminal", "files", "antigravity"};
        for (const char* q : default_queries) {
            for (const auto& app : registry.apps()) {
                if (to_lower(app.name).find(q) != std::string::npos) {
                    if (!is_on_desktop(result, app.id)) {
                        result.push_back(app);
                    }
                    break;
                }
            }
        }
        save_desktop_icons(result);
        return result;
    }

    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty()) continue;
        for (const auto& app : registry.apps()) {
            if (app.id == line || app.name == line) {
                if (!is_on_desktop(result, app.id)) {
                    result.push_back(app);
                }
                break;
            }
        }
    }
    return result;
}

void save_desktop_icons(const std::vector<DesktopApp>& icons) {
    const std::string path = desktop_icons_config_path();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return;
    for (const auto& app : icons) {
        out << (!app.id.empty() ? app.id : app.name) << '\n';
    }
}

bool is_on_desktop(const std::vector<DesktopApp>& icons, const std::string& app_id) {
    for (const auto& ic : icons) {
        if (ic.id == app_id) return true;
    }
    return false;
}

bool toggle_desktop_icon(const AppRegistry& registry, const DesktopApp& app) {
    std::vector<DesktopApp> icons = load_desktop_icons(registry);
    for (auto it = icons.begin(); it != icons.end(); ++it) {
        if (it->id == app.id) {
            icons.erase(it);
            save_desktop_icons(icons);
            return false;
        }
    }
    icons.push_back(app);
    save_desktop_icons(icons);
    return true;
}

std::string taskbar_pins_config_path() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    std::string base;
    if (xdg && *xdg) {
        base = xdg;
    } else if (home && *home) {
        base = std::string(home) + "/.config";
    } else {
        return "/tmp/lunar-de-taskbar-pins";
    }
    return base + "/lunar-de/taskbar_pins";
}

std::vector<DesktopApp> load_taskbar_pins(const AppRegistry& registry) {
    const std::string path = taskbar_pins_config_path();
    std::vector<DesktopApp> result;

    if (!std::filesystem::exists(path)) {
        const char* default_queries[] = {"files", "brave", "terminal"};
        for (const char* q : default_queries) {
            for (const auto& app : registry.apps()) {
                if (to_lower(app.name).find(q) != std::string::npos) {
                    if (!is_on_desktop(result, app.id)) {
                        result.push_back(app);
                    }
                    break;
                }
            }
        }
        save_taskbar_pins(result);
        return result;
    }

    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty()) continue;
        for (const auto& app : registry.apps()) {
            if (app.id == line || app.name == line) {
                if (!is_on_desktop(result, app.id)) {
                    result.push_back(app);
                }
                break;
            }
        }
    }
    return result;
}

void save_taskbar_pins(const std::vector<DesktopApp>& pins) {
    const std::string path = taskbar_pins_config_path();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return;
    for (const auto& app : pins) {
        out << (!app.id.empty() ? app.id : app.name) << '\n';
    }
}

bool toggle_taskbar_pin(const AppRegistry& registry, const DesktopApp& app) {
    std::vector<DesktopApp> pins = load_taskbar_pins(registry);
    for (auto it = pins.begin(); it != pins.end(); ++it) {
        if (it->id == app.id) {
            pins.erase(it);
            save_taskbar_pins(pins);
            return false;
        }
    }
    pins.push_back(app);
    save_taskbar_pins(pins);
    return true;
}

} // namespace lunar
