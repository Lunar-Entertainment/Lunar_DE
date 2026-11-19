#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <dlfcn.h>
#include <limits.h>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xcb/xcb_ewmh.h>

namespace lunar {

struct FreeDeleter {
    void operator()(void* ptr) const noexcept { std::free(ptr); }
};

template <typename T>
using UniqueMalloc = std::unique_ptr<T, FreeDeleter>;

struct Rect {
    int16_t x = 0;
    int16_t y = 0;
    uint16_t width = 0;
    uint16_t height = 0;
};

struct MonitorInfo {
    Rect rect{};
    bool primary = false;
};

namespace theme {
inline constexpr uint32_t kRootBg         = 0x1e293b; // Fallback dark slate desktop background
inline constexpr uint32_t kPanelBg        = 0x161920; // Translucent-tinted charcoal bar base
inline constexpr uint32_t kPanelBorder    = 0x2a2f3a; // Subtle bottom hairline
inline constexpr uint32_t kAccent         = 0x38bdf8; // Sky cyan accent
inline constexpr uint32_t kAccentBg       = 0x2b303b; // Subtle pill background
inline constexpr uint32_t kBorderFocused  = 0x38bdf8; // 2px focused window border
inline constexpr uint32_t kBorderUnfocused= 0x334155; // 2px unfocused window border
inline constexpr uint32_t kTextPrimary    = 0xf3f4f6; // Crisp off-white text
inline constexpr uint32_t kTextMuted      = 0x9ca3af; // Muted gray text
inline constexpr uint16_t kBorderWidth    = 2;
inline constexpr uint16_t kPanelHeight    = 28;
} // namespace theme

namespace ipc {
inline constexpr const char* kPanelWindowAtom = "_LUNAR_PANEL_WINDOW";
inline constexpr const char* kPanelActionAtom = "_LUNAR_PANEL_ACTION";
inline constexpr const char* kWmActionAtom    = "_LUNAR_WM_ACTION";

inline constexpr uint32_t kPanelToggleAppMenu          = 1;
inline constexpr uint32_t kPanelToggleQuickSettings    = 2;
inline constexpr uint32_t kPanelOpenContextMenu        = 3;
inline constexpr uint32_t kPanelDismissPopups          = 4;
inline constexpr uint32_t kPanelOpenWallpaperPicker    = 5;
inline constexpr uint32_t kPanelReloadWallpaper        = 6;
inline constexpr uint32_t kPanelToggleGeminiLive       = 7;
inline constexpr uint32_t kPanelOpenAppearance         = 8;
inline constexpr uint32_t kPanelToggleRunDialog        = 9;

inline constexpr uint32_t kWmExit                      = 1;
inline constexpr uint32_t kWmReloadWallpaper           = 2;
inline constexpr uint32_t kWmToggleMinimize            = 3;
inline constexpr uint32_t kWmReloadDesktopIcons        = 4;
inline constexpr uint32_t kWmRestart                   = 5;
} // namespace ipc

inline std::string resolve_self_exe_path(const char* fallback) {
    char buf[PATH_MAX]{};
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string path(buf);
        const std::string deleted_suffix = " (deleted)";
        if (path.size() > deleted_suffix.size() &&
            path.compare(path.size() - deleted_suffix.size(),
                         deleted_suffix.size(), deleted_suffix) == 0) {
            path.erase(path.size() - deleted_suffix.size());
        }
        return path;
    }
    return fallback ? std::string(fallback) : std::string{};
}

inline void prepare_x11_app_env() {
    unsetenv("WAYLAND_DISPLAY");
    unsetenv("SESSION_MANAGER");
    unsetenv("GNOME_DESKTOP_SESSION_ID");
    unsetenv("GNOME_SHELL_SESSION_MODE");
    unsetenv("GTK_MODULES");
    unsetenv("QT_ACCESSIBILITY");
    setenv("GDK_BACKEND", "x11", 1);
    setenv("QT_QPA_PLATFORM", "xcb", 1);
    setenv("SDL_VIDEODRIVER", "x11", 1);
    setenv("CLUTTER_BACKEND", "x11", 1);
    setenv("ELECTRON_OZONE_PLATFORM_HINT", "x11", 1);
    setenv("GTK_USE_PORTAL", "0", 1);
    unsetenv("GIO_USE_VFS");
    setenv("NO_AT_BRIDGE", "1", 1);
    setenv("GTK_A11Y", "none", 1);
    setenv("XDG_CURRENT_DESKTOP", "Lunar", 1);
}

inline std::vector<MonitorInfo> query_monitors(xcb_connection_t* conn, const xcb_screen_t* screen) {
    std::vector<MonitorInfo> out;
    if (!conn || !screen) return out;

    const xcb_query_extension_reply_t* ext = xcb_get_extension_data(conn, &xcb_randr_id);
    if (ext && ext->present) {
        auto cookie = xcb_randr_get_monitors(conn, screen->root, 1);
        UniqueMalloc<xcb_randr_get_monitors_reply_t> reply(
            xcb_randr_get_monitors_reply(conn, cookie, nullptr));
        if (reply) {
            auto iter = xcb_randr_get_monitors_monitors_iterator(reply.get());
            while (iter.rem > 0) {
                const xcb_randr_monitor_info_t* m = iter.data;
                if (m->width > 0 && m->height > 0) {
                    MonitorInfo info;
                    info.rect = Rect{m->x, m->y, m->width, m->height};
                    info.primary = (m->primary != 0);
                    out.push_back(info);
                }
                xcb_randr_monitor_info_next(&iter);
            }
        }
    }

    if (out.empty()) {
        out.push_back(MonitorInfo{
            Rect{0, 0, screen->width_in_pixels, screen->height_in_pixels},
            true
        });
    } else {
        bool has_primary = false;
        for (const auto& m : out) {
            if (m.primary) {
                has_primary = true;
                break;
            }
        }
        if (!has_primary) {
            out.front().primary = true;
        }
        std::sort(out.begin(), out.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
            if (a.rect.x != b.rect.x) return a.rect.x < b.rect.x;
            return a.rect.y < b.rect.y;
        });
    }
    return out;
}

inline MonitorInfo primary_monitor(const std::vector<MonitorInfo>& mons) {
    for (const auto& m : mons) {
        if (m.primary) return m;
    }
    if (!mons.empty()) return mons.front();
    return MonitorInfo{Rect{0, 0, 1280, 720}, true};
}

inline Rect monitor_for_point(const std::vector<MonitorInfo>& mons, int16_t x, int16_t y) {
    for (const auto& m : mons) {
        if (x >= m.rect.x && x < m.rect.x + static_cast<int>(m.rect.width) &&
            y >= m.rect.y && y < m.rect.y + static_cast<int>(m.rect.height)) {
            return m.rect;
        }
    }
    return primary_monitor(mons).rect;
}

inline Rect monitor_for_rect(const std::vector<MonitorInfo>& mons, const Rect& r) {
    const int16_t cx = static_cast<int16_t>(r.x + static_cast<int>(r.width) / 2);
    const int16_t cy = static_cast<int16_t>(r.y + static_cast<int>(r.height) / 2);
    return monitor_for_point(mons, cx, cy);
}

inline xcb_screen_t* get_screen(xcb_connection_t* conn, int screen_idx) {
    const xcb_setup_t* setup = xcb_get_setup(conn);
    if (!setup) return nullptr;
    xcb_screen_iterator_t iter = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_idx && iter.rem > 0; ++i) {
        xcb_screen_next(&iter);
    }
    return iter.rem > 0 ? iter.data : nullptr;
}

inline xcb_atom_t intern_atom(xcb_connection_t* conn, const char* name, bool only_if_exists = false) {
    auto cookie = xcb_intern_atom(conn, only_if_exists ? 1 : 0,
                                  static_cast<uint16_t>(std::char_traits<char>::length(name)), name);
    UniqueMalloc<xcb_intern_atom_reply_t> reply(xcb_intern_atom_reply(conn, cookie, nullptr));
    return reply ? reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
}

inline uint16_t detect_numlock_mask(xcb_connection_t* conn, xcb_keycode_t numlock_keycode) {
    if (numlock_keycode == XCB_NO_SYMBOL) return XCB_MOD_MASK_2;
    auto cookie = xcb_get_modifier_mapping(conn);
    UniqueMalloc<xcb_get_modifier_mapping_reply_t> reply(
        xcb_get_modifier_mapping_reply(conn, cookie, nullptr));
    if (!reply) return XCB_MOD_MASK_2;

    const xcb_keycode_t* modmap = xcb_get_modifier_mapping_keycodes(reply.get());
    const int keycodes_per_mod = reply->keycodes_per_modifier;
    for (int mod = 0; mod < 8; ++mod) {
        for (int k = 0; k < keycodes_per_mod; ++k) {
            if (modmap[mod * keycodes_per_mod + k] == numlock_keycode) {
                return static_cast<uint16_t>(1U << mod);
            }
        }
    }
    return XCB_MOD_MASK_2;
}

inline std::vector<xcb_char2b_t> utf8_to_char2b(const std::string& utf8) {
    std::vector<xcb_char2b_t> out;
    out.reserve(utf8.size());
    const auto* bytes = reinterpret_cast<const uint8_t*>(utf8.data());
    size_t i = 0;
    const size_t n = utf8.size();

    while (i < n && out.size() < 255) {
        uint32_t cp = '?';
        uint8_t b0 = bytes[i];
        if (b0 < 0x80) {
            cp = b0;
            i += 1;
        } else if ((b0 & 0xE0) == 0xC0 && i + 1 < n && (bytes[i + 1] & 0xC0) == 0x80) {
            cp = ((b0 & 0x1F) << 6) | (bytes[i + 1] & 0x3F);
            i += 2;
        } else if ((b0 & 0xF0) == 0xE0 && i + 2 < n &&
                   (bytes[i + 1] & 0xC0) == 0x80 && (bytes[i + 2] & 0xC0) == 0x80) {
            cp = ((b0 & 0x0F) << 12) | ((bytes[i + 1] & 0x3F) << 6) | (bytes[i + 2] & 0x3F);
            i += 3;
        } else if ((b0 & 0xF8) == 0xF0 && i + 3 < n) {
            // Outside UCS-2 BMP; replace with '?'
            cp = '?';
            i += 4;
        } else {
            i += 1;
        }
        if (cp > 0xFFFF) cp = '?';
        out.push_back(xcb_char2b_t{
            static_cast<uint8_t>((cp >> 8) & 0xFF),
            static_cast<uint8_t>(cp & 0xFF)
        });
    }
    return out;
}

inline void apply_rounded_shape(xcb_connection_t* conn, xcb_window_t win,
                                uint16_t width, uint16_t height,
                                uint16_t radius, int16_t offset = 0) {
    if (!conn || win == XCB_WINDOW_NONE || width == 0 || height == 0) return;

    using ShapeMaskFn = xcb_void_cookie_t (*)(xcb_connection_t*, uint8_t, uint8_t,
                                              xcb_window_t, int16_t, int16_t, xcb_pixmap_t);
    static ShapeMaskFn shape_mask_fn = []() -> ShapeMaskFn {
        void* handle = dlopen("libxcb-shape.so.0", RTLD_LAZY | RTLD_LOCAL);
        if (!handle) return nullptr;
        return reinterpret_cast<ShapeMaskFn>(dlsym(handle, "xcb_shape_mask"));
    }();
    if (!shape_mask_fn) return;

    constexpr uint8_t kShapeSet = 0;
    constexpr uint8_t kShapeBounding = 0;

    if (radius == 0) {
        shape_mask_fn(conn, kShapeSet, kShapeBounding, win, 0, 0, XCB_PIXMAP_NONE);
        return;
    }

    const uint16_t r = std::min<uint16_t>({radius, static_cast<uint16_t>(width / 2),
                                           static_cast<uint16_t>(height / 2)});
    if (r == 0) {
        shape_mask_fn(conn, kShapeSet, kShapeBounding, win, 0, 0, XCB_PIXMAP_NONE);
        return;
    }

    xcb_pixmap_t mask = xcb_generate_id(conn);
    xcb_create_pixmap(conn, 1, mask, win, width, height);

    xcb_gcontext_t gc = xcb_generate_id(conn);
    uint32_t fg = 0;
    xcb_create_gc(conn, gc, mask, XCB_GC_FOREGROUND, &fg);

    const xcb_rectangle_t full{0, 0, width, height};
    xcb_poly_fill_rectangle(conn, mask, gc, 1, &full);

    fg = 1;
    xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &fg);

    const uint16_t d = static_cast<uint16_t>(r * 2);
    const xcb_rectangle_t rects[2] = {
        {static_cast<int16_t>(r), 0, static_cast<uint16_t>(width - d), height},
        {0, static_cast<int16_t>(r), width, static_cast<uint16_t>(height - d)},
    };
    xcb_poly_fill_rectangle(conn, mask, gc, 2, rects);

    const xcb_arc_t arcs[4] = {
        {0, 0, d, d, 0, 360 * 64},
        {static_cast<int16_t>(width - d), 0, d, d, 0, 360 * 64},
        {0, static_cast<int16_t>(height - d), d, d, 0, 360 * 64},
        {static_cast<int16_t>(width - d), static_cast<int16_t>(height - d), d, d, 0, 360 * 64},
    };
    xcb_poly_fill_arc(conn, mask, gc, 4, arcs);

    shape_mask_fn(conn, kShapeSet, kShapeBounding, win, offset, offset, mask);
    xcb_free_gc(conn, gc);
    xcb_free_pixmap(conn, mask);
}

} // namespace lunar
