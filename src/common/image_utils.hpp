#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <xcb/xcb.h>

namespace lunar {

struct ImageBuffer {
    uint16_t width = 0;
    uint16_t height = 0;
    // Pixel format: 0x00RRGGBB (little-endian BGRX matching X11 24-bit ZPixmap)
    std::vector<uint32_t> pixels;
};

struct WallpaperEntry {
    std::string name;
    std::string path;
    std::string group; // "32 Free", "Zorin OS", "Custom"
};

inline constexpr const char* kWallpaperCycleToken = "CYCLE_ALL";
inline constexpr const char* kCycleActiveFile = "/tmp/lunar-cycle-wallpaper";

std::string resolve_wallpaper_path();
bool is_wallpaper_cycle_enabled();
bool save_wallpaper_path(const std::string& path);
bool write_cycle_active_wallpaper(const std::string& path);
std::vector<WallpaperEntry> collect_wallpapers();

ImageBuffer load_jpeg_cover_scaled(const std::string& path, uint16_t target_w, uint16_t target_h);
ImageBuffer load_image_cover_scaled(const std::string& path, uint16_t target_w, uint16_t target_h);
bool upload_pixels_to_pixmap(xcb_connection_t* conn, const xcb_screen_t* screen,
                             xcb_pixmap_t pixmap, uint16_t width, uint16_t height,
                             const std::vector<uint32_t>& pixels);

class FontRenderer {
public:
    FontRenderer() = default;
    ~FontRenderer();

    FontRenderer(const FontRenderer&) = delete;
    FontRenderer& operator=(const FontRenderer&) = delete;

    bool init(bool bold = true);
    int measure_text(const std::string& utf8, int px_size) const;
    void draw_text(ImageBuffer& target, int x, int baseline_y, const std::string& utf8,
                   int px_size, uint32_t rgb) const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

// Anti-aliased 2D software drawing helpers on 0x00RRGGBB ImageBuffer
void blend_pixel(ImageBuffer& buf, int x, int y, uint32_t rgb, float alpha);
void fill_rect_alpha(ImageBuffer& buf, int x, int y, int w, int h, uint32_t rgb, float alpha);
void fill_rounded_rect_aa(ImageBuffer& buf, float x, float y, float w, float h,
                          float radius, uint32_t rgb, float alpha);
void stroke_rounded_rect_aa(ImageBuffer& buf, float x, float y, float w, float h,
                            float radius, float thickness, uint32_t rgb, float alpha);
void fill_circle_aa(ImageBuffer& buf, float cx, float cy, float r, uint32_t rgb, float alpha);
void stroke_circle_aa(ImageBuffer& buf, float cx, float cy, float r, float thickness,
                      uint32_t rgb, float alpha);
void draw_line_aa(ImageBuffer& buf, float x0, float y0, float x1, float y1,
                  float thickness, uint32_t rgb, float alpha);
void draw_arc_aa(ImageBuffer& buf, float cx, float cy, float r,
                 float start_deg, float end_deg, float thickness,
                 uint32_t rgb, float alpha);

} // namespace lunar
