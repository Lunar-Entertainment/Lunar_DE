#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <iostream>
#include <jpeglib.h>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kW = 1920;
constexpr int kH = 1080;

struct RGB {
    float r, g, b;
};

RGB lerp(RGB a, RGB b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

RGB hex(uint32_t c) {
    return {
        static_cast<float>((c >> 16) & 0xFF),
        static_cast<float>((c >> 8) & 0xFF),
        static_cast<float>(c & 0xFF)
    };
}

float hash2d(int x, int y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
}

float smooth_noise(float x, float y) {
    int xi = static_cast<int>(std::floor(x));
    int yi = static_cast<int>(std::floor(y));
    float fx = x - xi;
    float fy = y - yi;
    float ux = fx * fx * (3.0f - 2.0f * fx);
    float uy = fy * fy * (3.0f - 2.0f * fy);
    float a = hash2d(xi, yi);
    float b = hash2d(xi + 1, yi);
    float c = hash2d(xi, yi + 1);
    float d = hash2d(xi + 1, yi + 1);
    return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
}

float fbm(float x, float y, int octaves = 4) {
    float sum = 0.0f;
    float amp = 0.5f;
    float freq = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * smooth_noise(x * freq, y * freq);
        freq *= 2.05f;
        amp *= 0.5f;
    }
    return sum;
}

float ridge_height(float u, float seed, float base_y, float amp) {
    float n = fbm(u * 3.8f + seed, seed * 1.7f, 5);
    float detail = std::sin(u * 9.0f + seed * 3.1f) * 0.025f;
    return base_y - (n - 0.35f) * amp + detail;
}

bool write_jpeg(const fs::path& path, const std::vector<uint8_t>& rgb) {
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return false;
    jpeg_compress_struct cinfo{};
    jpeg_error_mgr jerr{};
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);
    cinfo.image_width = kW;
    cinfo.image_height = kH;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 94, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        const uint8_t* row = rgb.data() + static_cast<size_t>(cinfo.next_scanline) * kW * 3;
        JSAMPROW ptr = const_cast<JSAMPROW>(row);
        jpeg_write_scanlines(&cinfo, &ptr, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    std::fclose(fp);
    return true;
}

template <typename Shader>
void render_wallpaper(const fs::path& out_path, Shader shader) {
    std::vector<uint8_t> rgb(static_cast<size_t>(kW) * kH * 3);
    for (int y = 0; y < kH; ++y) {
        const float v = static_cast<float>(y) / static_cast<float>(kH - 1);
        for (int x = 0; x < kW; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(kW - 1);
            RGB c = shader(u, v, x, y);
            // Subtle film dithering prevents 8-bit gradient banding
            const float dither = (hash2d(x, y) - 0.5f) * 1.4f;
            const size_t idx = (static_cast<size_t>(y) * kW + static_cast<size_t>(x)) * 3;
            rgb[idx + 0] = static_cast<uint8_t>(std::clamp(c.r + dither, 0.0f, 255.0f));
            rgb[idx + 1] = static_cast<uint8_t>(std::clamp(c.g + dither, 0.0f, 255.0f));
            rgb[idx + 2] = static_cast<uint8_t>(std::clamp(c.b + dither, 0.0f, 255.0f));
        }
    }
    write_jpeg(out_path, rgb);
}

RGB render_layered_mountains(float u, float v, RGB sky_top, RGB sky_bot,
                             const std::array<RGB, 4>& layers, float seed) {
    RGB c = lerp(sky_top, sky_bot, std::pow(v, 0.85f));
    for (int i = 0; i < 4; ++i) {
        const float base = 0.48f + i * 0.115f;
        const float amp = 0.24f - i * 0.03f;
        const float h = ridge_height(u, seed + i * 7.3f, base, amp);
        if (v >= h) {
            const float mist = std::exp(-std::max(0.0f, (v - h) * 10.0f)) * 0.16f;
            c = lerp(layers[i], sky_bot, mist);
        }
    }
    return c;
}

} // namespace

int main(int argc, char** argv) {
    const fs::path out_dir = (argc > 1) ? fs::path(argv[1]) : fs::path("assets/wallpapers");
    fs::create_directories(out_dir);

    struct Spec {
        const char* filename;
        std::function<RGB(float, float, int, int)> shader;
    };

    const std::vector<Spec> specs = {
        {"01_lunar_eclipse.jpg", [](float u, float v, int x, int y) {
            RGB bg = lerp(hex(0x060a14), hex(0x0f1b33), v * 0.85f);
            float dx = (u - 0.5f) * (16.0f / 9.0f);
            float dy = v - 0.48f;
            float dist = std::hypot(dx, dy);
            float glow = std::exp(-std::fabs(dist - 0.22f) * 16.0f);
            float outer = std::exp(-dist * 3.5f) * 0.35f;
            bg = lerp(bg, hex(0x38bdf8), std::clamp(glow * 0.85f + outer, 0.0f, 1.0f));
            if (dist < 0.215f) {
                float rim = std::exp(-(0.215f - dist) * 45.0f);
                bg = lerp(hex(0x04070d), hex(0x7dd3fc), rim * 0.7f);
            }
            float star = hash2d(x / 2, y / 2);
            if (star > 0.9965f && dist > 0.23f) {
                float tw = (star - 0.9965f) * 280.0f;
                bg = lerp(bg, hex(0xe0f2fe), std::min(1.0f, tw * 0.7f));
            }
            return bg;
        }},
        {"02_nordic_aurora.jpg", [](float u, float v, int x, int y) {
            RGB sky = lerp(hex(0x040b18), hex(0x0d2137), v);
            float wave = std::sin(u * 5.2f + fbm(u * 3.0f, v * 2.0f) * 2.4f) * 0.08f + 0.32f;
            float band = std::exp(-std::fabs(v - wave) * 11.0f) * (0.6f + 0.4f * std::sin(u * 14.0f));
            float curtain = fbm(u * 18.0f, v * 2.5f) * band;
            sky = lerp(sky, hex(0x10b981), std::clamp(curtain * 1.4f, 0.0f, 0.9f));
            sky = lerp(sky, hex(0x8b5cf6), std::clamp(band * 0.45f * (1.0f - v), 0.0f, 0.6f));
            float star = hash2d(x, y);
            if (star > 0.9985f && v < 0.55f) sky = lerp(sky, hex(0xf8fafc), 0.65f);
            const std::array<RGB, 4> m = {hex(0x0f1f33), hex(0x0b1726), hex(0x07101b), hex(0x04080f)};
            for (int i = 0; i < 4; ++i) {
                if (v >= ridge_height(u, 2.1f + i * 5.1f, 0.58f + i * 0.09f, 0.22f)) return m[i];
            }
            return sky;
        }},
        {"03_sunset_dunes.jpg", [](float u, float v, int, int) {
            RGB sky = lerp(hex(0x1e1b4b), hex(0xf97316), std::pow(std::clamp(v * 1.5f, 0.0f, 1.0f), 1.3f));
            float sun_d = std::hypot((u - 0.68f) * 1.77f, v - 0.36f);
            sky = lerp(sky, hex(0xfef08a), std::exp(-sun_d * 8.0f) * 0.9f);
            const std::array<RGB, 4> dunes = {hex(0xc2410c), hex(0x9a3412), hex(0x7c2d12), hex(0x431407)};
            for (int i = 0; i < 4; ++i) {
                float curve = 0.50f + i * 0.11f + std::sin(u * (3.2f + i) + i * 1.8f) * 0.055f;
                if (v >= curve) {
                    float shade = std::clamp((v - curve) * 6.0f, 0.0f, 1.0f);
                    return lerp(dunes[i], hex(0x270d05), shade * 0.35f);
                }
            }
            return sky;
        }},
        {"04_ocean_horizon.jpg", [](float u, float v, int, int) {
            if (v < 0.52f) {
                float t = v / 0.52f;
                RGB sky = lerp(hex(0x0f172a), hex(0x38bdf8), std::pow(t, 1.4f));
                float sun = std::exp(-std::hypot((u - 0.5f) * 1.77f, v - 0.47f) * 6.5f);
                return lerp(sky, hex(0xe0f2fe), sun * 0.65f);
            }
            float wt = (v - 0.52f) / 0.48f;
            float rip = std::sin(u * 40.0f + fbm(u * 8.0f, v * 28.0f) * 6.0f) * 0.5f + 0.5f;
            RGB sea = lerp(hex(0x0e7490), hex(0x042f2e), std::pow(wt, 0.75f));
            float glint = std::exp(-std::fabs(u - 0.5f) * (5.0f + wt * 6.0f)) * rip;
            return lerp(sea, hex(0xa5f3fc), glint * 0.5f);
        }},
        {"05_cyber_synthwave.jpg", [](float u, float v, int, int) {
            if (v < 0.55f) {
                RGB sky = lerp(hex(0x090418), hex(0x7e22ce), v / 0.55f);
                float d = std::hypot((u - 0.5f) * 1.77f, v - 0.42f);
                if (d < 0.24f) {
                    bool stripe = (v > 0.36f) && (std::fmod(v * 65.0f, 2.0f) < 0.55f);
                    if (!stripe) {
                        float st = std::clamp((v - 0.18f) / 0.37f, 0.0f, 1.0f);
                        return lerp(hex(0xfde047), hex(0xf43f5e), st);
                    }
                }
                return lerp(sky, hex(0xec4899), std::exp(-d * 4.0f) * 0.45f);
            }
            float z = (v - 0.55f) / 0.45f + 0.04f;
            float gx = std::fabs(std::fmod(((u - 0.5f) / z) * 4.0f + 100.0f, 1.0f) - 0.5f);
            float gz = std::fabs(std::fmod(1.4f / z, 1.0f) - 0.5f);
            RGB ground = lerp(hex(0x2e1065), hex(0x05020d), std::sqrt(z));
            if (gx < 0.03f / std::sqrt(z) || gz < 0.04f) {
                return lerp(ground, hex(0x22d3ee), 0.75f);
            }
            return ground;
        }},
        {"06_crimson_peaks.jpg", [](float u, float v, int, int) {
            return render_layered_mountains(
                u, v, hex(0x1f122b), hex(0xfb7185),
                {hex(0x881337), hex(0x6b1031), hex(0x4c0519), hex(0x1f0710)}, 6.4f);
        }},
        {"07_emerald_forest.jpg", [](float u, float v, int, int) {
            RGB base = render_layered_mountains(
                u, v, hex(0x064e3b), hex(0xa7f3d0),
                {hex(0x047857), hex(0x065f46), hex(0x064e3b), hex(0x022c22)}, 12.7f);
            float beam = std::exp(-std::fabs((u - v * 0.45f) - 0.25f) * 6.0f) * (1.0f - v) * 0.25f;
            return lerp(base, hex(0xecfdf5), beam);
        }},
        {"08_tokyo_midnight.jpg", [](float u, float v, int, int) {
            RGB bg = lerp(hex(0x0b1020), hex(0x1e293b), v);
            for (int i = 0; i < 18; ++i) {
                float bx = std::fmod(hash2d(i, 31) * 1.2f, 1.0f);
                float by = 0.25f + hash2d(i, 77) * 0.55f;
                float br = 0.04f + hash2d(i, 99) * 0.12f;
                float d = std::hypot((u - bx) * 1.77f, v - by);
                RGB col = (i % 3 == 0) ? hex(0x38bdf8) : ((i % 3 == 1) ? hex(0xf59e0b) : hex(0xf43f5e));
                bg = lerp(bg, col, std::exp(- (d * d) / (br * br)) * 0.26f);
            }
            return bg;
        }},
        {"09_cotton_candy_sky.jpg", [](float u, float v, int, int) {
            float n = fbm(u * 2.8f, v * 2.8f, 4);
            RGB top = lerp(hex(0x60a5fa), hex(0xc084fc), u * 0.6f + n * 0.4f);
            RGB bot = lerp(hex(0xf472b6), hex(0xfde68a), u * 0.5f + 0.2f);
            RGB c = lerp(top, bot, std::pow(v, 0.9f));
            return lerp(c, hex(0xffffff), std::clamp((n - 0.55f) * 0.9f, 0.0f, 0.45f));
        }},
        {"10_obsidian_mesh.jpg", [](float u, float v, int, int) {
            float w1 = std::sin(u * 6.0f + v * 4.0f) * 0.5f + 0.5f;
            float w2 = fbm(u * 4.5f, v * 4.5f, 4);
            RGB bg = lerp(hex(0x09090b), hex(0x18181b), w2);
            float bloom = std::exp(-std::hypot((u - 0.72f) * 1.77f, v - 0.35f) * 2.6f);
            bg = lerp(bg, hex(0x2563eb), bloom * 0.52f * w1);
            float bloom2 = std::exp(-std::hypot((u - 0.22f) * 1.77f, v - 0.75f) * 2.8f);
            return lerp(bg, hex(0x06b6d4), bloom2 * 0.38f);
        }},
        {"11_solar_flare.jpg", [](float u, float v, int, int) {
            float wave = std::sin(u * 4.0f + fbm(u * 3.0f, v * 3.0f) * 3.5f) * 0.5f + 0.5f;
            RGB c = lerp(hex(0x450a0a), hex(0xea580c), wave * 0.75f + v * 0.25f);
            return lerp(c, hex(0xfacc15), std::pow(wave, 3.0f) * 0.55f);
        }},
        {"12_arctic_glacier.jpg", [](float u, float v, int, int) {
            return render_layered_mountains(
                u, v, hex(0x0c4a6e), hex(0xe0f2fe),
                {hex(0x7dd3fc), hex(0x38bdf8), hex(0x0284c7), hex(0x075985)}, 19.2f);
        }},
        {"13_cosmic_nebula.jpg", [](float u, float v, int x, int y) {
            float n1 = fbm(u * 3.6f, v * 3.6f, 5);
            float n2 = fbm(u * 6.2f + 4.0f, v * 6.2f + 2.0f, 4);
            RGB bg = lerp(hex(0x030712), hex(0x312e81), n1);
            bg = lerp(bg, hex(0xdb2777), std::clamp((n2 - 0.4f) * 1.6f, 0.0f, 0.75f));
            bg = lerp(bg, hex(0x38bdf8), std::clamp((n1 - 0.52f) * 1.8f, 0.0f, 0.6f));
            float star = hash2d(x, y);
            if (star > 0.9978f) bg = lerp(bg, hex(0xffffff), 0.85f);
            return bg;
        }},
        {"14_matcha_zen.jpg", [](float u, float v, int, int) {
            RGB c = lerp(hex(0x1a2e22), hex(0x3f624d), v);
            float arch = 0.55f + std::sin(u * 2.5f + 0.6f) * 0.14f;
            if (v > arch) c = lerp(hex(0x65a30d), hex(0x14532d), (v - arch) * 2.2f);
            float sun = std::hypot((u - 0.34f) * 1.77f, v - 0.35f);
            if (sun < 0.17f) c = lerp(c, hex(0xfef3c7), 0.88f);
            return c;
        }},
        {"15_royal_amethyst.jpg", [](float u, float v, int, int) {
            float r1 = std::sin(u * 3.5f - v * 2.5f + fbm(u * 2.5f, v * 2.5f) * 2.2f) * 0.5f + 0.5f;
            RGB c = lerp(hex(0x1e1b4b), hex(0x7e22ce), r1);
            return lerp(c, hex(0xa855f7), std::pow(r1, 2.5f) * 0.5f);
        }},
        {"16_monochrome_slate.jpg", [](float u, float v, int, int) {
            float diag = u * 0.45f + v * 0.55f;
            float fold = std::sin(diag * 7.0f + fbm(u * 3.0f, v * 3.0f) * 1.5f) * 0.5f + 0.5f;
            return lerp(hex(0x0f172a), hex(0x475569), fold * 0.65f + diag * 0.25f);
        }},
    };

    for (const auto& s : specs) {
        const fs::path p = out_dir / s.filename;
        render_wallpaper(p, s.shader);
        std::cout << "Generated " << p << '\n';
    }
    return 0;
}
