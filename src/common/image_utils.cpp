#include "common/image_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <vector>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <jpeglib.h>
#include <png.h>

namespace fs = std::filesystem;

namespace lunar {

namespace {

uint32_t next_utf8_codepoint(const std::string& s, size_t& i) {
    const auto* b = reinterpret_cast<const uint8_t*>(s.data());
    const size_t n = s.size();
    uint8_t b0 = b[i];
    if (b0 < 0x80) {
        i += 1;
        return b0;
    }
    if ((b0 & 0xE0) == 0xC0 && i + 1 < n && (b[i + 1] & 0xC0) == 0x80) {
        uint32_t cp = ((b0 & 0x1F) << 6) | (b[i + 1] & 0x3F);
        i += 2;
        return cp;
    }
    if ((b0 & 0xF0) == 0xE0 && i + 2 < n &&
        (b[i + 1] & 0xC0) == 0x80 && (b[i + 2] & 0xC0) == 0x80) {
        uint32_t cp = ((b0 & 0x0F) << 12) | ((b[i + 1] & 0x3F) << 6) | (b[i + 2] & 0x3F);
        i += 3;
        return cp;
    }
    if ((b0 & 0xF8) == 0xF0 && i + 3 < n &&
        (b[i + 1] & 0xC0) == 0x80 && (b[i + 2] & 0xC0) == 0x80 && (b[i + 3] & 0xC0) == 0x80) {
        uint32_t cp = ((b0 & 0x07) << 18) | ((b[i + 1] & 0x3F) << 12) |
                      ((b[i + 2] & 0x3F) << 6) | (b[i + 3] & 0x3F);
        i += 4;
        return cp;
    }
    i += 1;
    return '?';
}

std::string resolve_font_path(bool bold) {
    std::string resolved;
    if (FcInit()) {
        const char* query = bold ? "sans:bold" : "sans";
        FcPattern* pat = FcNameParse(reinterpret_cast<const FcChar8*>(query));
        if (pat) {
            FcConfigSubstitute(nullptr, pat, FcMatchPattern);
            FcDefaultSubstitute(pat);
            FcResult res = FcResultMatch;
            FcPattern* match = FcFontMatch(nullptr, pat, &res);
            if (match) {
                FcChar8* file = nullptr;
                if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
                    resolved = reinterpret_cast<const char*>(file);
                }
                FcPatternDestroy(match);
            }
            FcPatternDestroy(pat);
        }
    }

    if (!resolved.empty() && fs::exists(resolved)) {
        return resolved;
    }

    const char* fallbacks[] = {
        bold ? "/usr/share/fonts/truetype/croscore/Arimo-Bold.ttf"
             : "/usr/share/fonts/truetype/croscore/Arimo-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    };
    for (const char* path : fallbacks) {
        if (fs::exists(path)) return path;
    }
    return {};
}

std::string format_wallpaper_label(const fs::path& p) {
    std::string stem = p.stem().string();
    // Strip leading "01_" index prefix if present
    if (stem.size() > 3 && std::isdigit(static_cast<unsigned char>(stem[0])) &&
        std::isdigit(static_cast<unsigned char>(stem[1])) && stem[2] == '_') {
        stem = stem.substr(3);
    }
    bool cap_next = true;
    for (char& c : stem) {
        if (c == '_' || c == '-') {
            c = ' ';
            cap_next = true;
        } else if (cap_next) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            cap_next = false;
        }
    }
    return stem;
}

fs::path find_bundled_wallpapers_dir() {
    std::error_code ec;
    char exe_buf[512]{};
    const ssize_t len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (len > 0) {
        exe_buf[len] = '\0';
        const fs::path exe_dir = fs::path(exe_buf).parent_path();
        const fs::path cand1 = exe_dir.parent_path() / "assets" / "wallpapers";
        if (fs::exists(cand1, ec)) return cand1;
        const fs::path cand2 = exe_dir / "assets" / "wallpapers";
        if (fs::exists(cand2, ec)) return cand2;
    }
    if (fs::exists("assets/wallpapers", ec)) return fs::path("assets/wallpapers");
    if (fs::exists("/usr/local/share/lunar-de/assets/wallpapers", ec)) {
        return fs::path("/usr/local/share/lunar-de/assets/wallpapers");
    }
    if (fs::exists("/usr/share/lunar-de/assets/wallpapers", ec)) {
        return fs::path("/usr/share/lunar-de/assets/wallpapers");
    }
    if (fs::exists("/home/lunar/Lunar_DE/assets/wallpapers", ec)) {
        return fs::path("/home/lunar/Lunar_DE/assets/wallpapers");
    }
    return {};
}

ImageBuffer cover_scale_rgb(const std::vector<uint8_t>& src_rgb, uint32_t src_w, uint32_t src_h,
                            uint32_t comps, uint16_t target_w, uint16_t target_h) {
    ImageBuffer result;
    if (src_w == 0 || src_h == 0 || comps < 3 || target_w == 0 || target_h == 0) return result;

    const double scale_x = static_cast<double>(target_w) / static_cast<double>(src_w);
    const double scale_y = static_cast<double>(target_h) / static_cast<double>(src_h);
    const double scale = std::max(scale_x, scale_y);
    const double crop_w = static_cast<double>(target_w) / scale;
    const double crop_h = static_cast<double>(target_h) / scale;
    const double off_x = (static_cast<double>(src_w) - crop_w) * 0.5;
    const double off_y = (static_cast<double>(src_h) - crop_h) * 0.5;

    result.width = target_w;
    result.height = target_h;
    result.pixels.resize(static_cast<size_t>(target_w) * target_h);

    for (uint16_t y = 0; y < target_h; ++y) {
        const double sy = std::clamp(off_y + (y + 0.5) / scale - 0.5, 0.0, static_cast<double>(src_h - 1));
        const uint32_t y0 = static_cast<uint32_t>(sy);
        const uint32_t y1 = std::min(y0 + 1, src_h - 1);
        const double fy = sy - y0;

        for (uint16_t x = 0; x < target_w; ++x) {
            const double sx = std::clamp(off_x + (x + 0.5) / scale - 0.5, 0.0, static_cast<double>(src_w - 1));
            const uint32_t x0 = static_cast<uint32_t>(sx);
            const uint32_t x1 = std::min(x0 + 1, src_w - 1);
            const double fx = sx - x0;

            const size_t i00 = (static_cast<size_t>(y0) * src_w + x0) * comps;
            const size_t i10 = (static_cast<size_t>(y0) * src_w + x1) * comps;
            const size_t i01 = (static_cast<size_t>(y1) * src_w + x0) * comps;
            const size_t i11 = (static_cast<size_t>(y1) * src_w + x1) * comps;

            const double w00 = (1.0 - fx) * (1.0 - fy);
            const double w10 = fx * (1.0 - fy);
            const double w01 = (1.0 - fx) * fy;
            const double w11 = fx * fy;

            const uint32_t r = static_cast<uint32_t>(
                src_rgb[i00] * w00 + src_rgb[i10] * w10 + src_rgb[i01] * w01 + src_rgb[i11] * w11);
            const uint32_t g = static_cast<uint32_t>(
                src_rgb[i00 + 1] * w00 + src_rgb[i10 + 1] * w10 +
                src_rgb[i01 + 1] * w01 + src_rgb[i11 + 1] * w11);
            const uint32_t b = static_cast<uint32_t>(
                src_rgb[i00 + 2] * w00 + src_rgb[i10 + 2] * w10 +
                src_rgb[i01 + 2] * w01 + src_rgb[i11 + 2] * w11);

            result.pixels[static_cast<size_t>(y) * target_w + x] = (r << 16) | (g << 8) | b;
        }
    }

    return result;
}

ImageBuffer load_png_cover_scaled(const std::string& path, uint16_t target_w, uint16_t target_h) {
    ImageBuffer result;
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return result;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png) {
        std::fclose(fp);
        return result;
    }
    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, nullptr, nullptr);
        std::fclose(fp);
        return result;
    }
    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(fp);
        return result;
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
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png);
    }
    if (color_type & PNG_COLOR_MASK_ALPHA) {
        png_set_strip_alpha(png);
    }

    png_read_update_info(png, info);
    if (src_w == 0 || src_h == 0 || src_w > 8192 || src_h > 8192) {
        png_destroy_read_struct(&png, &info, nullptr);
        std::fclose(fp);
        return result;
    }

    std::vector<uint8_t> rgb(static_cast<size_t>(src_w) * src_h * 3);
    std::vector<png_bytep> rows(src_h);
    for (png_uint_32 y = 0; y < src_h; ++y) {
        rows[y] = rgb.data() + static_cast<size_t>(y) * src_w * 3;
    }
    png_read_image(png, rows.data());
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(fp);

    return cover_scale_rgb(rgb, src_w, src_h, 3, target_w, target_h);
}

} // namespace

namespace {
fs::path get_wallpaper_config_file() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        if (*xdg) return fs::path(xdg) / "lunar-de" / "wallpaper";
    }
    if (const char* home = std::getenv("HOME")) {
        if (*home) return fs::path(home) / ".config" / "lunar-de" / "wallpaper";
    }
    return {};
}
} // namespace

bool is_wallpaper_cycle_enabled() {
    std::error_code ec;
    const fs::path cfg = get_wallpaper_config_file();
    if (cfg.empty() || !fs::exists(cfg, ec)) return false;
    std::ifstream in(cfg);
    std::string saved;
    if (!std::getline(in, saved)) return false;
    while (!saved.empty() && (saved.back() == '\n' || saved.back() == '\r')) {
        saved.pop_back();
    }
    return saved == kWallpaperCycleToken;
}

bool write_cycle_active_wallpaper(const std::string& path) {
    std::ofstream out(kCycleActiveFile, std::ios::trunc);
    if (!out.is_open()) return false;
    out << path << '\n';
    out.flush();
    return !out.fail();
}

std::string resolve_wallpaper_path() {
    std::error_code ec;
    const fs::path cfg = get_wallpaper_config_file();
    if (!cfg.empty() && fs::exists(cfg, ec)) {
        std::ifstream in(cfg);
        std::string saved;
        if (std::getline(in, saved)) {
            while (!saved.empty() && (saved.back() == '\n' || saved.back() == '\r')) {
                saved.pop_back();
            }
            if (saved == kWallpaperCycleToken) {
                std::ifstream cin(kCycleActiveFile);
                std::string active;
                if (std::getline(cin, active)) {
                    while (!active.empty() && (active.back() == '\n' || active.back() == '\r')) {
                        active.pop_back();
                    }
                    if (!active.empty() && fs::exists(active, ec)) {
                        return active;
                    }
                }
            } else if (!saved.empty() && fs::exists(saved, ec)) {
                return saved;
            }
        }
    }

    if (const char* env_wp = std::getenv("LUNAR_WALLPAPER")) {
        if (*env_wp && fs::exists(env_wp, ec)) {
            return env_wp;
        }
    }

    const char* candidates[] = {
        "/usr/share/backgrounds/Dolomites-by-Wirestock.jpg",
        "/usr/share/backgrounds/Zorin.jpg",
        "/usr/share/backgrounds/Snowy-Mountains-by-Luke-Peterson.jpg",
    };
    for (const char* p : candidates) {
        if (fs::exists(p, ec)) return p;
    }

    const fs::path bundled = find_bundled_wallpapers_dir() / "01_lunar_eclipse.jpg";
    if (fs::exists(bundled, ec)) return bundled.string();
    return {};
}

bool save_wallpaper_path(const std::string& path) {
    const fs::path cfg = get_wallpaper_config_file();
    if (cfg.empty()) return false;
    std::error_code ec;
    fs::create_directories(cfg.parent_path(), ec);
    std::ofstream out(cfg, std::ios::trunc);
    if (!out.is_open()) return false;
    out << path << '\n';
    out.flush();
    out.close();
    return !out.fail();
}

std::vector<WallpaperEntry> collect_wallpapers() {
    std::vector<WallpaperEntry> entries;
    std::error_code ec;

    auto is_supported_img = [](const fs::path& p) {
        std::error_code sz_ec;
        if (fs::file_size(p, sz_ec) <= 32 || sz_ec) return false;
        std::string ext = p.extension().string();
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return ext == ".jpg" || ext == ".jpeg" || ext == ".png";
    };

    // 1. 32 Free Bundled Lunar Wallpapers + 33rd Cycle All option
    const fs::path bundled_dir = find_bundled_wallpapers_dir();
    if (!bundled_dir.empty() && fs::exists(bundled_dir, ec)) {
        std::vector<fs::path> paths;
        for (const auto& entry : fs::directory_iterator(bundled_dir, ec)) {
            if (entry.is_regular_file(ec) && is_supported_img(entry.path())) {
                paths.push_back(entry.path());
            }
        }
        std::sort(paths.begin(), paths.end());
        for (const auto& p : paths) {
            entries.push_back({format_wallpaper_label(p), p.string(), "32 Free"});
        }
        entries.push_back({"Cycle All (15s)", kWallpaperCycleToken, "32 Free"});
    }

    // 2. Zorin OS System Wallpapers
    const fs::path sys_dir("/usr/share/backgrounds");
    if (fs::exists(sys_dir, ec)) {
        std::vector<fs::path> paths;
        for (const auto& entry : fs::directory_iterator(sys_dir, ec)) {
            if (entry.is_regular_file(ec) && is_supported_img(entry.path())) {
                paths.push_back(entry.path());
            }
        }
        std::sort(paths.begin(), paths.end());
        for (const auto& p : paths) {
            entries.push_back({format_wallpaper_label(p), p.string(), "Zorin OS"});
        }
    }

    // 3. Custom User Backgrounds (~/Pictures and ~/.config/lunar-de/wallpapers)
    if (const char* home = std::getenv("HOME")) {
        const std::vector<fs::path> custom_dirs = {
            fs::path(home) / "Pictures",
            fs::path(home) / "Pictures" / "Wallpapers",
            fs::path(home) / ".config" / "lunar-de" / "wallpapers",
        };
        for (const auto& cdir : custom_dirs) {
            if (!fs::exists(cdir, ec)) continue;
            for (const auto& entry : fs::directory_iterator(cdir, ec)) {
                if (entry.is_regular_file(ec) && is_supported_img(entry.path())) {
                    entries.push_back({format_wallpaper_label(entry.path()),
                                       entry.path().string(), "Custom"});
                }
            }
        }
    }

    return entries;
}

namespace {
struct SafeJpegErrorMgr {
    jpeg_error_mgr pub;
    jmp_buf env;
};

void safe_jpeg_error_exit(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<SafeJpegErrorMgr*>(cinfo->err);
    longjmp(err->env, 1);
}
} // namespace

ImageBuffer load_jpeg_cover_scaled(const std::string& path, uint16_t target_w, uint16_t target_h) {
    ImageBuffer result;
    if (path.empty() || target_w == 0 || target_h == 0) return result;

    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return result;

    jpeg_decompress_struct cinfo{};
    SafeJpegErrorMgr jerr{};
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = safe_jpeg_error_exit;

    if (setjmp(jerr.env)) {
        jpeg_destroy_decompress(&cinfo);
        std::fclose(fp);
        return {};
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        std::fclose(fp);
        return result;
    }

    cinfo.out_color_space = JCS_RGB;
    uint32_t scale_denom = 1;
    while (scale_denom < 8 &&
           (cinfo.image_width / (scale_denom * 2)) >= target_w &&
           (cinfo.image_height / (scale_denom * 2)) >= target_h) {
        scale_denom *= 2;
    }
    cinfo.scale_num = 1;
    cinfo.scale_denom = scale_denom;

    jpeg_start_decompress(&cinfo);
    const uint32_t src_w = cinfo.output_width;
    const uint32_t src_h = cinfo.output_height;
    const uint32_t comps = cinfo.output_components;

    std::vector<uint8_t> src_rgb(static_cast<size_t>(src_w) * src_h * comps);
    while (cinfo.output_scanline < src_h) {
        uint8_t* row_ptr = src_rgb.data() + static_cast<size_t>(cinfo.output_scanline) * src_w * comps;
        jpeg_read_scanlines(&cinfo, &row_ptr, 1);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    std::fclose(fp);

    return cover_scale_rgb(src_rgb, src_w, src_h, comps, target_w, target_h);
}

ImageBuffer load_image_cover_scaled(const std::string& path, uint16_t target_w, uint16_t target_h) {
    if (path.empty()) return {};
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) return {};
    uint8_t magic[4]{};
    const size_t n = std::fread(magic, 1, sizeof(magic), fp);
    std::fclose(fp);

    if (n >= 4 && magic[0] == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47) {
        return load_png_cover_scaled(path, target_w, target_h);
    }
    return load_jpeg_cover_scaled(path, target_w, target_h);
}

bool upload_pixels_to_pixmap(xcb_connection_t* conn, const xcb_screen_t* screen,
                             xcb_pixmap_t pixmap, uint16_t width, uint16_t height,
                             const std::vector<uint32_t>& pixels) {
    if (!conn || !screen || pixmap == XCB_PIXMAP_NONE ||
        pixels.size() < static_cast<size_t>(width) * height) {
        return false;
    }

    xcb_gcontext_t gc = xcb_generate_id(conn);
    xcb_create_gc(conn, gc, pixmap, 0, nullptr);

    const uint16_t rows_per_chunk =
        static_cast<uint16_t>(std::max<size_t>(1, 65536 / (static_cast<size_t>(width) * 4)));

    for (uint16_t y = 0; y < height; y = static_cast<uint16_t>(y + rows_per_chunk)) {
        const uint16_t h = static_cast<uint16_t>(std::min<int>(rows_per_chunk, height - y));
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(
            pixels.data() + static_cast<size_t>(y) * width);
        const uint32_t byte_len = static_cast<uint32_t>(width) * h * 4;
        xcb_put_image(conn, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, gc,
                      width, h, 0, static_cast<int16_t>(y), 0, screen->root_depth,
                      byte_len, ptr);
    }

    xcb_free_gc(conn, gc);
    return true;
}

struct FontRenderer::Impl {
    FT_Library lib = nullptr;
    FT_Face face = nullptr;
    int current_px_size = 0;
};

FontRenderer::~FontRenderer() {
    if (impl_) {
        if (impl_->face) FT_Done_Face(impl_->face);
        if (impl_->lib) FT_Done_FreeType(impl_->lib);
        delete impl_;
        impl_ = nullptr;
    }
}

bool FontRenderer::init(bool bold) {
    const std::string path = resolve_font_path(bold);
    if (path.empty()) return false;

    impl_ = new Impl();
    if (FT_Init_FreeType(&impl_->lib) != 0) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    if (FT_New_Face(impl_->lib, path.c_str(), 0, &impl_->face) != 0) {
        FT_Done_FreeType(impl_->lib);
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    return true;
}

int FontRenderer::measure_text(const std::string& utf8, int px_size) const {
    if (!impl_ || !impl_->face || utf8.empty()) return 0;
    if (impl_->current_px_size != px_size) {
        FT_Set_Pixel_Sizes(impl_->face, 0, static_cast<FT_UInt>(px_size));
        impl_->current_px_size = px_size;
    }

    int width_26_6 = 0;
    size_t i = 0;
    while (i < utf8.size()) {
        const uint32_t cp = next_utf8_codepoint(utf8, i);
        if (FT_Load_Char(impl_->face, cp, FT_LOAD_DEFAULT) == 0) {
            width_26_6 += static_cast<int>(impl_->face->glyph->advance.x);
        }
    }
    return (width_26_6 + 32) >> 6;
}

void FontRenderer::draw_text(ImageBuffer& target, int x, int baseline_y, const std::string& utf8,
                             int px_size, uint32_t rgb) const {
    if (!impl_ || !impl_->face || utf8.empty()) return;
    if (impl_->current_px_size != px_size) {
        FT_Set_Pixel_Sizes(impl_->face, 0, static_cast<FT_UInt>(px_size));
        impl_->current_px_size = px_size;
    }

    int pen_x = x << 6;
    size_t i = 0;
    while (i < utf8.size()) {
        const uint32_t cp = next_utf8_codepoint(utf8, i);
        if (FT_Load_Char(impl_->face, cp, FT_LOAD_RENDER) != 0) continue;

        FT_GlyphSlot g = impl_->face->glyph;
        const int gx = (pen_x >> 6) + g->bitmap_left;
        const int gy = baseline_y - g->bitmap_top;

        for (unsigned int row = 0; row < g->bitmap.rows; ++row) {
            for (unsigned int col = 0; col < g->bitmap.width; ++col) {
                const uint8_t cov = g->bitmap.buffer[row * static_cast<unsigned int>(g->bitmap.pitch) + col];
                if (cov > 0) {
                    blend_pixel(target, gx + static_cast<int>(col), gy + static_cast<int>(row),
                                rgb, static_cast<float>(cov) / 255.0f);
                }
            }
        }
        pen_x += static_cast<int>(g->advance.x);
    }
}

void blend_pixel(ImageBuffer& buf, int x, int y, uint32_t rgb, float alpha) {
    if (x < 0 || y < 0 || x >= buf.width || y >= buf.height || alpha <= 0.0f) return;
    alpha = std::min(1.0f, alpha);
    const size_t idx = static_cast<size_t>(y) * buf.width + static_cast<size_t>(x);
    const uint32_t dst = buf.pixels[idx];

    const float inv = 1.0f - alpha;
    const uint32_t sr = (rgb >> 16) & 0xFF;
    const uint32_t sg = (rgb >> 8) & 0xFF;
    const uint32_t sb = rgb & 0xFF;

    const uint32_t dr = (dst >> 16) & 0xFF;
    const uint32_t dg = (dst >> 8) & 0xFF;
    const uint32_t db = dst & 0xFF;

    const uint32_t r = static_cast<uint32_t>(sr * alpha + dr * inv + 0.5f);
    const uint32_t g = static_cast<uint32_t>(sg * alpha + dg * inv + 0.5f);
    const uint32_t b = static_cast<uint32_t>(sb * alpha + db * inv + 0.5f);

    buf.pixels[idx] = (r << 16) | (g << 8) | b;
}

void fill_rect_alpha(ImageBuffer& buf, int x, int y, int w, int h, uint32_t rgb, float alpha) {
    const int x0 = std::max(0, x);
    const int y0 = std::max(0, y);
    const int x1 = std::min<int>(buf.width, x + w);
    const int y1 = std::min<int>(buf.height, y + h);
    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            blend_pixel(buf, px, py, rgb, alpha);
        }
    }
}

void fill_rounded_rect_aa(ImageBuffer& buf, float x, float y, float w, float h,
                          float radius, uint32_t rgb, float alpha) {
    const float r = std::min({radius, w * 0.5f, h * 0.5f});
    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.5f;
    const float hx = w * 0.5f - r;
    const float hy = h * 0.5f - r;

    const int x0 = std::max(0, static_cast<int>(std::floor(x - 1.0f)));
    const int y0 = std::max(0, static_cast<int>(std::floor(y - 1.0f)));
    const int x1 = std::min<int>(buf.width, static_cast<int>(std::ceil(x + w + 1.0f)));
    const int y1 = std::min<int>(buf.height, static_cast<int>(std::ceil(y + h + 1.0f)));

    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            const float dx = std::max(0.0f, std::fabs((px + 0.5f) - cx) - hx);
            const float dy = std::max(0.0f, std::fabs((py + 0.5f) - cy) - hy);
            const float dist = std::hypot(dx, dy) - r;
            const float cov = std::clamp(0.5f - dist, 0.0f, 1.0f);
            if (cov > 0.0f) {
                blend_pixel(buf, px, py, rgb, alpha * cov);
            }
        }
    }
}

void stroke_rounded_rect_aa(ImageBuffer& buf, float x, float y, float w, float h,
                            float radius, float thickness, uint32_t rgb, float alpha) {
    const float r = std::min({radius, w * 0.5f, h * 0.5f});
    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.5f;
    const float hx = w * 0.5f - r;
    const float hy = h * 0.5f - r;
    const float half_t = thickness * 0.5f;

    const int x0 = std::max(0, static_cast<int>(std::floor(x - half_t - 1.0f)));
    const int y0 = std::max(0, static_cast<int>(std::floor(y - half_t - 1.0f)));
    const int x1 = std::min<int>(buf.width, static_cast<int>(std::ceil(x + w + half_t + 1.0f)));
    const int y1 = std::min<int>(buf.height, static_cast<int>(std::ceil(y + h + half_t + 1.0f)));

    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            const float px_d = std::fabs((px + 0.5f) - cx) - hx;
            const float py_d = std::fabs((py + 0.5f) - cy) - hy;
            const float out_d = std::hypot(std::max(0.0f, px_d), std::max(0.0f, py_d));
            const float in_d = std::min(0.0f, std::max(px_d, py_d));
            const float sdf = out_d + in_d - r;
            const float d = std::fabs(sdf) - half_t;
            const float cov = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f) {
                blend_pixel(buf, px, py, rgb, alpha * cov);
            }
        }
    }
}

void fill_circle_aa(ImageBuffer& buf, float cx, float cy, float r, uint32_t rgb, float alpha) {
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - r - 1.0f)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - r - 1.0f)));
    const int x1 = std::min<int>(buf.width, static_cast<int>(std::ceil(cx + r + 1.0f)));
    const int y1 = std::min<int>(buf.height, static_cast<int>(std::ceil(cy + r + 1.0f)));

    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            const float dist = std::hypot((px + 0.5f) - cx, (py + 0.5f) - cy) - r;
            const float cov = std::clamp(0.5f - dist, 0.0f, 1.0f);
            if (cov > 0.0f) {
                blend_pixel(buf, px, py, rgb, alpha * cov);
            }
        }
    }
}

void stroke_circle_aa(ImageBuffer& buf, float cx, float cy, float r, float thickness,
                      uint32_t rgb, float alpha) {
    const float half_t = thickness * 0.5f;
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - r - half_t - 1.0f)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - r - half_t - 1.0f)));
    const int x1 = std::min<int>(buf.width, static_cast<int>(std::ceil(cx + r + half_t + 1.0f)));
    const int y1 = std::min<int>(buf.height, static_cast<int>(std::ceil(cy + r + half_t + 1.0f)));

    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            const float d = std::fabs(std::hypot((px + 0.5f) - cx, (py + 0.5f) - cy) - r) - half_t;
            const float cov = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f) {
                blend_pixel(buf, px, py, rgb, alpha * cov);
            }
        }
    }
}

void draw_line_aa(ImageBuffer& buf, float x0, float y0, float x1, float y1,
                  float thickness, uint32_t rgb, float alpha) {
    const float half_t = thickness * 0.5f;
    const int min_x = std::max(0, static_cast<int>(std::floor(std::min(x0, x1) - half_t - 1.0f)));
    const int min_y = std::max(0, static_cast<int>(std::floor(std::min(y0, y1) - half_t - 1.0f)));
    const int max_x = std::min<int>(buf.width, static_cast<int>(std::ceil(std::max(x0, x1) + half_t + 1.0f)));
    const int max_y = std::min<int>(buf.height, static_cast<int>(std::ceil(std::max(y0, y1) + half_t + 1.0f)));

    const float vx = x1 - x0;
    const float vy = y1 - y0;
    const float len2 = vx * vx + vy * vy;

    for (int py = min_y; py < max_y; ++py) {
        for (int px = min_x; px < max_x; ++px) {
            const float wx = (px + 0.5f) - x0;
            const float wy = (py + 0.5f) - y0;
            const float t = (len2 > 1e-5f) ? std::clamp((wx * vx + wy * vy) / len2, 0.0f, 1.0f) : 0.0f;
            const float dist = std::hypot(wx - t * vx, wy - t * vy) - half_t;
            const float cov = std::clamp(0.5f - dist, 0.0f, 1.0f);
            if (cov > 0.0f) {
                blend_pixel(buf, px, py, rgb, alpha * cov);
            }
        }
    }
}

void draw_arc_aa(ImageBuffer& buf, float cx, float cy, float r,
                 float start_deg, float end_deg, float thickness,
                 uint32_t rgb, float alpha) {
    constexpr float kDegToRad = 3.14159265f / 180.0f;
    const float half_t = thickness * 0.5f;
    const int x0 = std::max(0, static_cast<int>(std::floor(cx - r - half_t - 1.0f)));
    const int y0 = std::max(0, static_cast<int>(std::floor(cy - r - half_t - 1.0f)));
    const int x1 = std::min<int>(buf.width, static_cast<int>(std::ceil(cx + r + half_t + 1.0f)));
    const int y1 = std::min<int>(buf.height, static_cast<int>(std::ceil(cy + r + half_t + 1.0f)));

    for (int py = y0; py < y1; ++py) {
        for (int px = x0; px < x1; ++px) {
            const float dx = (px + 0.5f) - cx;
            const float dy = (py + 0.5f) - cy;
            const float ang = std::atan2(dy, dx) / kDegToRad;
            if (ang < start_deg || ang > end_deg) continue;
            const float d = std::fabs(std::hypot(dx, dy) - r) - half_t;
            const float cov = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f) {
                blend_pixel(buf, px, py, rgb, alpha * cov);
            }
        }
    }
}

} // namespace lunar
