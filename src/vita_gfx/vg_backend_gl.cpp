#include "vg_backend_gl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef __vita__
#   include <psp2/kernel/sysmem.h>
#   include <vitaGL.h>
#else
#   define GL_SILENCE_DEPRECATION
#   include <OpenGL/gl.h>
#   include <OpenGL/glext.h>
#   define glClearDepthf glClearDepth
#endif

#include "shared/rt64_f3d_defines.h"
#include "shared/rt64_other_mode.h"

#include "vg_renderer.h"
#include "vg_texture.h"

#ifdef __vita__
#   include <filesystem>
#   include <vector>
#   define XXH_INLINE_ALL
#   include "xxHash/xxhash.h"
#endif

namespace vg {
#include "vg_font.inl"

#ifdef __vita__
    // Compiled GXP programs are kept in shader_cache/ (in the working directory): a Cg compile takes 100+ ms on the Vita, and the
    // intro compiles ~20 of them. File: vertex size, fragment size (u32 each), then both programs. Keyed by the generated sources.
    static std::string shaderCachePath(const std::string &vertex, const std::string &fragment) {
        const uint64_t hash = XXH3_64bits_withSeed(fragment.data(), fragment.size(), XXH3_64bits(vertex.data(), vertex.size()));
        char name[64];
        snprintf(name, sizeof(name), "shader_cache/%016llx.gxp", (unsigned long long)hash);
        return name;
    }

    static bool loadCachedShaders(const std::string &path, uint32_t &vs, uint32_t &fs) {
        FILE *f = fopen(path.c_str(), "rb");
        if (f == nullptr) {
            return false;
        }
        uint32_t sizes[2] = {};
        std::vector<uint8_t> data;
        bool ok = (fread(sizes, sizeof(sizes), 1, f) == 1) && (sizes[0] > 0) && (sizes[1] > 0) && (sizes[0] + sizes[1] <= (1u << 20));
        if (ok) {
            data.resize(size_t(sizes[0]) + sizes[1]);
            ok = fread(data.data(), 1, data.size(), f) == data.size();
        }
        fclose(f);
        if (!ok) {
            return false;
        }
        vs = glCreateShader(GL_VERTEX_SHADER);
        fs = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderBinary(1, &vs, 0, data.data(), GLsizei(sizes[0]));
        glShaderBinary(1, &fs, 0, data.data() + sizes[0], GLsizei(sizes[1]));
        return true;
    }

    static void saveCachedShaders(const std::string &path, uint32_t vs, uint32_t fs) {
        std::vector<uint8_t> vsData(1u << 18), fsData(1u << 18);
        GLsizei vsSize = 0, fsSize = 0;
        vglGetShaderBinary(vs, GLsizei(vsData.size()), &vsSize, vsData.data());
        vglGetShaderBinary(fs, GLsizei(fsData.size()), &fsSize, fsData.data());
        if ((vsSize <= 0) || (fsSize <= 0)) {
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories("shader_cache", ec);
        FILE *f = fopen(path.c_str(), "wb");
        if (f == nullptr) {
            return;
        }
        const uint32_t sizes[2] = { uint32_t(vsSize), uint32_t(fsSize) };
        const bool ok = (fwrite(sizes, sizeof(sizes), 1, f) == 1) && (fwrite(vsData.data(), 1, size_t(vsSize), f) == size_t(vsSize)) &&
            (fwrite(fsData.data(), 1, size_t(fsSize), f) == size_t(fsSize));
        fclose(f);
        if (!ok) {
            std::filesystem::remove(path, ec);
        }
    }
#endif

    static float shiftScale(uint8_t shift) {
        if (shift <= 10) {
            return 1.0f / float(1U << shift);
        }
        return float(1U << (16 - shift));
    }

    static void fillColorToFloat(uint32_t fill, uint8_t siz, float out[4]) {
        if (siz == G_IM_SIZ_32b) {
            out[0] = ((fill >> 24) & 0xFF) / 255.0f;
            out[1] = ((fill >> 16) & 0xFF) / 255.0f;
            out[2] = ((fill >> 8) & 0xFF) / 255.0f;
            out[3] = (fill & 0xFF) / 255.0f;
        }
        else {
            const uint32_t c = fill >> 16;
            const uint32_t r = (c >> 11) & 0x1F, g = (c >> 6) & 0x1F, b = (c >> 1) & 0x1F;
            out[0] = ((r << 3) | (r >> 2)) / 255.0f;
            out[1] = ((g << 3) | (g >> 2)) / 255.0f;
            out[2] = ((b << 3) | (b >> 2)) / 255.0f;
            out[3] = (c & 1) ? 1.0f : 0.0f;
        }
    }

    static void logFreeMemory(const char *when) {
#ifdef __vita__
        SceKernelFreeMemorySizeInfo mem{};
        mem.size = sizeof(mem);
        sceKernelGetFreeMemorySize(&mem);
        fprintf(stderr, "[vita-gfx] free memory %s: user %d KB, cdram %d KB, phycont %d KB, vitaGL ram %u KB, vram %u KB\n", when,
            mem.size_user / 1024, mem.size_cdram / 1024, mem.size_phycont / 1024,
            unsigned(vglMemFree(VGL_MEM_RAM) / 1024), unsigned(vglMemFree(VGL_MEM_VRAM) / 1024));
        fflush(stderr);
#else
        (void)when;
#endif
    }

    uint32_t Backend::freeVramKB() {
#ifdef __vita__
        return uint32_t(vglMemFree(VGL_MEM_VRAM) / 1024);
#else
        return 0;
#endif
    }

    void Backend::setEnabled(uint32_t cap, bool enabled, int &cached) {
        if (cached != int(enabled)) {
            if (enabled) {
                glEnable(cap);
            }
            else {
                glDisable(cap);
            }
            cached = int(enabled);
        }
    }

    Backend::Backend() {
#ifdef __vita__
        cg = true;
        swapCallback = []() { vglSwapBuffers(GL_FALSE); };
#endif
    }

    static bool envEnabled(const char *name) {
        const char *v = std::getenv(name);
        return !(v && (v[0] == '0'));
    }

    bool Backend::init() {
        useStateCache = envEnabled("ROGUESQ_VG_GL_STATE_CACHE");
        fprintf(stderr, "[vita-gfx] gl state cache %d\n", int(useStateCache));
#ifdef __vita__
        SceKernelFreeMemorySizeInfo mem{};
        mem.size = sizeof(mem);
        sceKernelGetFreeMemorySize(&mem);
        fprintf(stderr, "[vita-gfx] free memory before vitaGL: user %d KB, cdram %d KB, phycont %d KB\n", mem.size_user / 1024, mem.size_cdram / 1024, mem.size_phycont / 1024);
        // FBO render targets default to one GXM scene per frame; every rebind of a target starts a new scene.
        vglSetupRenderTargetScenesNum(1, MaxScenesPerFrame);
        // Explicit pools: vitaGL's default takes all free main memory, but sceGxmCreateRenderTarget allocates each FBO's
        // render target from what is left, so most main memory stays free.
        const int ramPool = 32 * 1024 * 1024;
        const int cdramPool = std::max(0, mem.size_cdram - 16 * 1024 * 1024);
        const int phycontPool = std::min(mem.size_phycont, 8 * 1024 * 1024);
        // The return value only reports a resolution fallback (GL_TRUE), not success.
        if (vglInitWithCustomSizes(0, 960, 544, ramPool, cdramPool, phycontPool, 0, SCE_GXM_MULTISAMPLE_NONE)) {
            fprintf(stderr, "[vita-gfx] vitaGL fell back to a lower display resolution\n");
        }
        vglWaitVblankStart(GL_FALSE);
        logFreeMemory("after vitaGL init");
#endif
        const char *presentVs;
        const char *presentFs;
        if (cg) {
            presentVs =
                "void main(float4 aPos, float4 aColor, float2 aTex, out float4 gl_Position : POSITION, out float2 vTex : TEXCOORD0) {\n"
                "  gl_Position = aPos;\n"
                "  vTex = aTex;\n"
                "}\n";
            presentFs =
                "float4 main(float2 vTex : TEXCOORD0, uniform sampler2D uTex0 : TEXUNIT0) : COLOR {\n"
                "  return float4(tex2D(uTex0, vTex).rgb, 1.0);\n"
                "}\n";
        }
        else {
            presentVs =
                "#version 120\n"
                "attribute vec4 aPos;\nattribute vec4 aColor;\nattribute vec2 aTex;\nvarying vec2 vTex;\n"
                "void main() { gl_Position = aPos; vTex = aTex; }\n";
            presentFs =
                "#version 120\n"
                "varying vec2 vTex;\nuniform sampler2D uTex0;\n"
                "void main() { gl_FragColor = vec4(texture2D(uTex0, vTex).rgb, 1.0); }\n";
        }

        fprintf(stderr, "[vita-gfx] compiling present shader\n");
        fflush(stderr);
        const uint32_t vs = compile(GL_VERTEX_SHADER, presentVs);
        const uint32_t fs = compile(GL_FRAGMENT_SHADER, presentFs);
        presentProgram = (vs && fs) ? link(vs, fs, true) : 0;
        if (presentProgram == 0) {
            fprintf(stderr, "[vita-gfx] present shader failed\n");
            return false;
        }
        glUseProgram(presentProgram);
        glUniform1i(glGetUniformLocation(presentProgram, "uTex0"), 0);

        // Overlay: vertex color times the font atlas coverage.
        const char *overlayVs;
        const char *overlayFs;
        if (cg) {
            overlayVs =
                "void main(float4 aPos, float4 aColor, float2 aTex, out float4 gl_Position : POSITION, out float4 vColor : TEXCOORD1, out float2 vTex : TEXCOORD0) {\n"
                "  gl_Position = aPos;\n"
                "  vColor = aColor;\n"
                "  vTex = aTex;\n"
                "}\n";
            overlayFs =
                "float4 main(float4 vColor : TEXCOORD1, float2 vTex : TEXCOORD0, uniform sampler2D uTex0 : TEXUNIT0) : COLOR {\n"
                "  return float4(vColor.rgb, vColor.a * tex2D(uTex0, vTex).a);\n"
                "}\n";
        }
        else {
            overlayVs =
                "#version 120\n"
                "attribute vec4 aPos;\nattribute vec4 aColor;\nattribute vec2 aTex;\nvarying vec4 vColor;\nvarying vec2 vTex;\n"
                "void main() { gl_Position = aPos; vColor = aColor; vTex = aTex; }\n";
            overlayFs =
                "#version 120\n"
                "varying vec4 vColor;\nvarying vec2 vTex;\nuniform sampler2D uTex0;\n"
                "void main() { gl_FragColor = vec4(vColor.rgb, vColor.a * texture2D(uTex0, vTex).a); }\n";
        }
        const uint32_t ovs = compile(GL_VERTEX_SHADER, overlayVs);
        const uint32_t ofs = compile(GL_FRAGMENT_SHADER, overlayFs);
        overlayProgram = (ovs && ofs) ? link(ovs, ofs, true) : 0;
        if (overlayProgram != 0) {
            glUseProgram(overlayProgram);
            glUniform1i(glGetUniformLocation(overlayProgram, "uTex0"), 0);
            // White texels with the glyph coverage as alpha; the space glyph's corner is made opaque for solid panels.
            std::vector<uint8_t> rgba(size_t(FontAtlasWidth) * FontAtlasHeight * 4, 255);
            for (int i = 0; i < FontAtlasWidth * FontAtlasHeight; i++) {
                rgba[size_t(i) * 4 + 3] = FontAtlas[i];
            }
            for (int y = 0; y < 2; y++) {
                for (int x = 0; x < 2; x++) {
                    rgba[(size_t(y) * FontAtlasWidth + x) * 4 + 3] = 255;
                }
            }
            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, FontAtlasWidth, FontAtlasHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            fontTexture = tex;
        }
        fprintf(stderr, "[vita-gfx] backend ready (%s, scale %d)\n", cg ? "cg" : "glsl", resScale);
        return true;
    }

    void Backend::drawOverlay(const std::vector<OverlayItem> &overlay, float x0, float x1, float yTop, float yBottom, int viewW, int viewH) {
        if ((overlayProgram == 0) || overlay.empty()) {
            return;
        }

        // Image rectangle in viewport pixels (y down), then clip space per vertex.
        const float left = (x0 + 1.0f) * 0.5f * viewW, right = (x1 + 1.0f) * 0.5f * viewW;
        const float top = (1.0f - yTop) * 0.5f * viewH, bottom = (1.0f - yBottom) * 0.5f * viewH;
        const float imageW = right - left, imageH = bottom - top;
        std::vector<Vertex> vertices;
        auto quad = [&](float px0, float py0, float px1, float py1, float s0, float t0, float s1, float t1, uint32_t color) {
            const float cx0 = -1.0f + 2.0f * px0 / viewW, cx1 = -1.0f + 2.0f * px1 / viewW;
            const float cy0 = 1.0f - 2.0f * py0 / viewH, cy1 = 1.0f - 2.0f * py1 / viewH;
            const float r = ((color >> 24) & 0xFF) / 255.0f, g = ((color >> 16) & 0xFF) / 255.0f;
            const float b = ((color >> 8) & 0xFF) / 255.0f, a = (color & 0xFF) / 255.0f;
            const Vertex v[4] = {
                { cx0, cy0, 0.0f, 1.0f, r, g, b, a, s0, t0 },
                { cx1, cy0, 0.0f, 1.0f, r, g, b, a, s1, t0 },
                { cx0, cy1, 0.0f, 1.0f, r, g, b, a, s0, t1 },
                { cx1, cy1, 0.0f, 1.0f, r, g, b, a, s1, t1 },
            };
            const int order[6] = { 0, 1, 2, 3, 2, 1 };
            for (int i : order) {
                vertices.push_back(v[i]);
            }
        };

        // Panels sample the atlas's opaque corner texel; alphaScale fades a vertex (the rim of rounded corners).
        const float solidS = 0.5f / FontAtlasWidth, solidT = 0.5f / FontAtlasHeight;
        auto vertex = [&](float px, float py, uint32_t color, float alphaScale) {
            const float r = ((color >> 24) & 0xFF) / 255.0f, g = ((color >> 16) & 0xFF) / 255.0f;
            const float b = ((color >> 8) & 0xFF) / 255.0f, a = (color & 0xFF) / 255.0f * alphaScale;
            vertices.push_back({ -1.0f + 2.0f * px / viewW, 1.0f - 2.0f * py / viewH, 0.0f, 1.0f, r, g, b, a, solidS, solidT });
        };
        auto roundedPanel = [&](float px0, float py0, float px1, float py1, float radius, uint32_t color) {
            const float rad = std::min(radius, std::min(px1 - px0, py1 - py0) * 0.5f);
            quad(px0 + rad, py0, px1 - rad, py1, solidS, solidT, solidS, solidT, color);
            if (rad <= 0.0f) {
                return;
            }
            quad(px0, py0 + rad, px0 + rad, py1 - rad, solidS, solidT, solidS, solidT, color);
            quad(px1 - rad, py0 + rad, px1, py1 - rad, solidS, solidT, solidS, solidT, color);
            // Each corner is a fan out to rad - 0.5 px plus a 1 px rim fading out, which smooths the curve without multisampling.
            const float corners[4][3] = { { px0 + rad, py0 + rad, 180.0f }, { px1 - rad, py0 + rad, 270.0f }, { px1 - rad, py1 - rad, 0.0f }, { px0 + rad, py1 - rad, 90.0f } };
            constexpr int Segments = 8;
            const float inner = std::max(0.0f, rad - 0.5f), outer = rad + 0.5f;
            for (const auto &c : corners) {
                for (int i = 0; i < Segments; i++) {
                    const float a0 = (c[2] + 90.0f * i / Segments) * 3.14159265f / 180.0f;
                    const float a1 = (c[2] + 90.0f * (i + 1) / Segments) * 3.14159265f / 180.0f;
                    const float ix0 = c[0] + std::cos(a0) * inner, iy0 = c[1] + std::sin(a0) * inner;
                    const float ix1 = c[0] + std::cos(a1) * inner, iy1 = c[1] + std::sin(a1) * inner;
                    const float ox0 = c[0] + std::cos(a0) * outer, oy0 = c[1] + std::sin(a0) * outer;
                    const float ox1 = c[0] + std::cos(a1) * outer, oy1 = c[1] + std::sin(a1) * outer;
                    vertex(c[0], c[1], color, 1.0f);
                    vertex(ix0, iy0, color, 1.0f);
                    vertex(ix1, iy1, color, 1.0f);
                    vertex(ix0, iy0, color, 1.0f);
                    vertex(ix1, iy1, color, 1.0f);
                    vertex(ox0, oy0, color, 0.0f);
                    vertex(ix1, iy1, color, 1.0f);
                    vertex(ox1, oy1, color, 0.0f);
                    vertex(ox0, oy0, color, 0.0f);
                }
            }
        };

        for (const OverlayItem &item : overlay) {
            const float px = left + item.x * imageW, py = top + item.y * imageH;
            if (item.kind == OverlayItem::Kind::Panel) {
                roundedPanel(px, py, px + item.w * imageW, py + item.h * imageH, item.radius * imageH, item.color);
                continue;
            }

            const float scale = (item.h * imageH) / float(FontCellHeight);
            float width = 0.0f;
            for (char c : item.text) {
                if ((c >= FontFirst) && (c <= FontLast)) {
                    width += FontGlyphs[c - FontFirst][2] * scale;
                }
            }
            float pen = px;
            if (item.align == OverlayItem::Align::Right) {
                pen = px - width;
            }
            else if (item.align == OverlayItem::Align::Center) {
                pen = px - width * 0.5f;
            }
            for (char c : item.text) {
                if ((c < FontFirst) || (c > FontLast)) {
                    continue;
                }
                const float *glyph = FontGlyphs[c - FontFirst];
                if (c != ' ') {
                    const float gx = pen - FontPad * scale, gy = py;
                    quad(gx, gy, gx + FontCellWidth * scale, gy + FontCellHeight * scale,
                        glyph[0] / FontAtlasWidth, glyph[1] / FontAtlasHeight,
                        (glyph[0] + FontCellWidth) / FontAtlasWidth, (glyph[1] + FontCellHeight) / FontAtlasHeight, item.color);
                }
                pen += glyph[2] * scale;
            }
        }

        glEnable(GL_BLEND);
        // The screen's alpha stays opaque under translucent panels.
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
        glUseProgram(overlayProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, fontTexture);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        const uint8_t *base = reinterpret_cast<const uint8_t *>(vertices.data());
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), base + offsetof(Vertex, x));
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), base + offsetof(Vertex, r));
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), base + offsetof(Vertex, s));
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(vertices.size()));
        glDisable(GL_BLEND);
        glc = GLStateCache{};
    }

    uint32_t Backend::compile(uint32_t type, const std::string &source) {
#ifdef __vita__
        // vitaGL runs GL_*_SHADER sources through its GLSL translator; the generated sources are already Cg.
        const GLuint shader = glCreateShader((type == GL_VERTEX_SHADER) ? GL_CG_VERTEX_SHADER_EXT : GL_CG_FRAGMENT_SHADER_EXT);
#else
        const GLuint shader = glCreateShader(type);
#endif
        const GLchar *text = source.c_str();
        const GLint length = GLint(source.size());
        glShaderSource(shader, 1, &text, &length);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
            fprintf(stderr, "[vita-gfx] shader compile failed: %s\n----\n%s\n----\n", log, source.c_str());
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    uint32_t Backend::link(uint32_t vs, uint32_t fs, bool) {
        const GLuint program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glBindAttribLocation(program, 0, "aPos");
        glBindAttribLocation(program, 1, "aColor");
        glBindAttribLocation(program, 2, "aTex");
        glLinkProgram(program);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
            fprintf(stderr, "[vita-gfx] program link failed: %s\n", log);
            glDeleteProgram(program);
            return 0;
        }
        return program;
    }

    Backend::Program *Backend::programFor(const ShaderKey &key) {
        auto it = programs.find(key);
        if (it != programs.end()) {
            return &it->second;
        }

        Program p;
        const ShaderSource src = generateShader(key, cg);
#ifdef __vita__
        const std::string cachePath = shaderCachePath(src.vertex, src.fragment);
        uint32_t cachedVs = 0, cachedFs = 0;
        if (loadCachedShaders(cachePath, cachedVs, cachedFs)) {
            p.program = link(cachedVs, cachedFs, true);
            if (p.program != 0) {
                statShaderCacheHits++;
            }
            else {
                // A stale or damaged entry: drop it and compile from source.
                glDeleteShader(cachedVs);
                glDeleteShader(cachedFs);
                std::error_code ec;
                std::filesystem::remove(cachePath, ec);
            }
        }
#endif
        if (p.program == 0) {
            const uint32_t vs = compile(GL_VERTEX_SHADER, src.vertex);
            const uint32_t fs = compile(GL_FRAGMENT_SHADER, src.fragment);
            if (vs && fs) {
                p.program = link(vs, fs, true);
#ifdef __vita__
                if (p.program != 0) {
                    saveCachedShaders(cachePath, vs, fs);
                }
#endif
            }
            statShaderCompiles++;
        }
        if (p.program != 0) {
            const GLuint g = p.program;
            p.locPrim = glGetUniformLocation(g, "uPrim");
            p.locEnv = glGetUniformLocation(g, "uEnv");
            p.locFog = glGetUniformLocation(g, "uFog");
            p.locBlend = glGetUniformLocation(g, "uBlend");
            p.locFill = glGetUniformLocation(g, "uFill");
            p.locMisc = glGetUniformLocation(g, "uMisc");
            p.locTile[0] = glGetUniformLocation(g, "uTile0");
            p.locTile[1] = glGetUniformLocation(g, "uTile1");
            p.locTexSize[0] = glGetUniformLocation(g, "uTexSize0");
            p.locTexSize[1] = glGetUniformLocation(g, "uTexSize1");
            p.locTex[0] = glGetUniformLocation(g, "uTex0");
            p.locTex[1] = glGetUniformLocation(g, "uTex1");
            glUseProgram(g);
            glc = GLStateCache{};
            if (p.locTex[0] >= 0) {
                glUniform1i(p.locTex[0], 0);
            }
            if (p.locTex[1] >= 0) {
                glUniform1i(p.locTex[1], 1);
            }
        }
        p.usesTexture[0] = src.usesTexture[0];
        p.usesTexture[1] = src.usesTexture[1];
        p.alphaBlend = src.alphaBlend;
        return &programs.emplace(key, p).first->second;
    }

    uint32_t Backend::depthBufferFor(uint16_t width, uint16_t height) {
        const uint32_t key = (uint32_t(width) << 16) | height;
        auto it = depthBuffers.find(key);
        if (it != depthBuffers.end()) {
            return it->second;
        }
        GLuint rb = 0;
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width * resScale, height * resScale);
        depthBuffers[key] = rb;
        return rb;
    }

    Backend::Target *Backend::targetFor(uint32_t address, uint16_t width, uint16_t height, uint8_t) {
        for (auto it = targets.begin(); it != targets.end(); ++it) {
            Target *t = it->get();
            if ((t->address == address) && (t->width == width)) {
                if (t->height >= height) {
                    return t;
                }
                // The parser learned a taller scissor for this image: recreate it at the new height.
                releaseTarget(t);
                targets.erase(it);
                break;
            }
        }

        auto t = std::make_unique<Target>();
        t->address = address;
        t->lastUse = frameCounter;
        t->width = width;
        t->height = height;
        const int w = t->width * resScale, h = t->height * resScale;
        GLuint tex = 0, fbo = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        t->depth = depthBufferFor(t->width, t->height);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
#ifdef __vita__
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->depth);
#else
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, t->depth);
#endif
        t->fbo = fbo;
        t->color = tex;
        fprintf(stderr, "[vita-gfx] new target 0x%06X %ux%u\n", address, t->width, t->height);
        fflush(stderr);
        // The clear begins the GXM scene on the new FBO (vitaGL creates its render target there); set the viewport inside it.
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClearDepthf(1.0f);
        glDepthMask(GL_TRUE);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glc = GLStateCache{};
        glViewport(0, 0, w, h);
        boundTarget = t.get();
        t->scenesThisFrame = 1;
        logFreeMemory("after new target");
        targets.push_back(std::move(t));
        return targets.back().get();
    }

    bool Backend::bindTarget(Target *target) {
        if (boundTarget != target) {
#ifdef __vita__
            if (target->scenesThisFrame >= MaxScenesPerFrame) {
                return false;
            }
#endif
            target->scenesThisFrame++;
            glBindFramebuffer(GL_FRAMEBUFFER, target->fbo);
            glViewport(0, 0, target->width * resScale, target->height * resScale);
            boundTarget = target;
        }
        if (target->depthClearPending) {
            glDisable(GL_SCISSOR_TEST);
            glDepthMask(GL_TRUE);
            glClearDepthf(1.0f);
            glClear(GL_DEPTH_BUFFER_BIT);
            glc = GLStateCache{};
            target->depthClearPending = false;
        }
        return true;
    }

    // Texture lifetime is decided by the parsing thread (vg_submit.cpp): pixels arrive once per hash, deletions explicitly.
    Backend::CachedTexture *Backend::textureFor(const TextureRef &ref) {
        auto it = textures.find(ref.hash);
        if (it != textures.end()) {
            return &it->second;
        }
        if (ref.pixels.empty()) {
            return nullptr;
        }

        CachedTexture ct;
        ct.width = ref.width;
        ct.height = ref.height;
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ref.width, ref.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, ref.pixels.data());
        ct.texture = tex;
        statTexUploads++;
        return &textures.emplace(ref.hash, ct).first->second;
    }

    void Backend::deleteTextures(const std::vector<uint64_t> &hashes) {
        for (uint64_t hash : hashes) {
            auto it = textures.find(hash);
            if (it != textures.end()) {
                GLuint tex = it->second.texture;
                glDeleteTextures(1, &tex);
                textures.erase(it);
            }
        }
    }

    void Backend::releaseTarget(Target *t) {
        if (boundTarget == t) {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            boundTarget = nullptr;
        }
        GLuint fbo = t->fbo, tex = t->color;
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        fprintf(stderr, "[vita-gfx] released target 0x%06X %ux%u\n", t->address, t->width, t->height);
    }

    // Color images not drawn for two seconds are released; VRAM is only ~96 MB.
    void Backend::evictTargets() {
        for (auto it = targets.begin(); it != targets.end();) {
            Target *t = it->get();
            if ((frameCounter - t->lastUse) > 120) {
                releaseTarget(t);
                it = targets.erase(it);
            }
            else {
                ++it;
            }
        }
    }

    void Backend::draw(const DrawState &s, const Vertex *vertices, uint32_t vertexCount, const TextureRef texRefs[2]) {
        if (!useStateCache) {
            glc = GLStateCache{};
        }
        const interop::OtherMode om = { s.otherModeL, s.otherModeH };
        const uint32_t cycleType = om.cycleType();
        if ((s.colorImageWidth == 0) || (s.colorImageWidth > 1024)) {
            statSkipped++;
            return;
        }

        // Fills into the depth image are depth clears; anything else drawn there is not emulated.
        if (s.colorImage == s.depthImage) {
            if ((s.kind == DrawKind::Rect) && (cycleType == G_CYC_FILL)) {
                for (auto &t : targets) {
                    if (t->width == s.colorImageWidth) {
                        t->depthClearPending = true;
                    }
                }
                if ((boundTarget != nullptr) && boundTarget->depthClearPending) {
                    bindTarget(boundTarget);
                }
            }
            else {
                statSkipped++;
            }
            return;
        }

        Target *target = targetFor(s.colorImage, s.colorImageWidth, s.colorImageHeight, s.colorImageSiz);
        if (!bindTarget(target)) {
            statSkipped++;
            return;
        }
        target->drawnSincePresent = true;
        target->drawsSincePresent++;
        target->lastUse = frameCounter;
        lastDrawnAddress = target->address;

        Program *p = programFor(makeShaderKey(s));
        if (p->program == 0) {
            statSkipped++;
            return;
        }
        if (glc.program != p->program) {
            glUseProgram(p->program);
            glc.program = p->program;
        }

        // Scissor in target pixels, GL bottom-left origin.
        const int32_t sx0 = std::clamp<int32_t>(s.scissorX0, 0, target->width);
        const int32_t sy0 = std::clamp<int32_t>(s.scissorY0, 0, target->height);
        const int32_t sx1 = std::clamp<int32_t>(s.scissorX1, 0, target->width);
        const int32_t sy1 = std::clamp<int32_t>(s.scissorY1, 0, target->height);
        if ((sx1 <= sx0) || (sy1 <= sy0)) {
            return;
        }
        setEnabled(GL_SCISSOR_TEST, true, glc.scissorTest);
        const int scissor[4] = { sx0 * resScale, (target->height - sy1) * resScale, (sx1 - sx0) * resScale, (sy1 - sy0) * resScale };
        if (memcmp(scissor, glc.scissor, sizeof(scissor)) != 0) {
            glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
            memcpy(glc.scissor, scissor, sizeof(scissor));
        }

        const bool rasterModes = (cycleType == G_CYC_FILL) || (cycleType == G_CYC_COPY);
        const bool zCmp = !rasterModes && om.zCmp();
        const bool zUpd = !rasterModes && om.zUpd();
        setEnabled(GL_DEPTH_TEST, zCmp || zUpd, glc.depthTest);
        const int depthFunc = zCmp ? GL_LEQUAL : GL_ALWAYS;
        if ((zCmp || zUpd) && (glc.depthFunc != depthFunc)) {
            glDepthFunc(depthFunc);
            glc.depthFunc = depthFunc;
        }
        if (glc.depthMask != int(zUpd)) {
            glDepthMask(zUpd ? GL_TRUE : GL_FALSE);
            glc.depthMask = int(zUpd);
        }

        const bool decal = zCmp && (om.zMode() == ZMODE_DEC);
        setEnabled(GL_POLYGON_OFFSET_FILL, decal, glc.polygonOffset);
        if (decal && !glc.polygonOffsetSet) {
            glPolygonOffset(-1.0f, -1.0f);
            glc.polygonOffsetSet = true;
        }

        const bool blend = !rasterModes && p->alphaBlend;
        setEnabled(GL_BLEND, blend, glc.blend);
        if (blend && !glc.blendFuncSet) {
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glc.blendFuncSet = true;
        }
        setEnabled(GL_CULL_FACE, false, glc.cullFace);

        float fill[4];
        fillColorToFloat(s.fillColor, s.colorImageSiz, fill);
        const float lodFraction = (om.textLOD() == G_TL_LOD) ? 0.0f : 1.0f;
        const float misc[4] = { lodFraction, s.primLodFrac, s.k4, s.k5 };
        if (p->locPrim >= 0) glUniform4fv(p->locPrim, 1, s.prim);
        if (p->locEnv >= 0) glUniform4fv(p->locEnv, 1, s.env);
        if (p->locFog >= 0) glUniform4fv(p->locFog, 1, s.fog);
        if (p->locBlend >= 0) glUniform4fv(p->locBlend, 1, s.blend);
        if (p->locFill >= 0) glUniform4fv(p->locFill, 1, fill);
        if (p->locMisc >= 0) glUniform4fv(p->locMisc, 1, misc);

        const bool isRect = (s.kind == DrawKind::Rect);
        const float persp = (isRect || (om.textPersp() == G_TP_PERSP)) ? 1.0f : 0.5f;
        const bool bilerp = (om.textFilt() != G_TF_POINT) && (cycleType != G_CYC_COPY);
        for (uint32_t i = 0; i < 2; i++) {
            if (!p->usesTexture[i]) {
                continue;
            }
            const TileState &tile = s.tiles[i];
            // Select the unit first: creating a texture binds it to the active unit.
            glActiveTexture(GL_TEXTURE0 + i);
            CachedTexture *tex = textureFor(texRefs[i]);
            if (tex == nullptr) {
                glActiveTexture(GL_TEXTURE0);
                statSkipped++;
                return;
            }
            glBindTexture(GL_TEXTURE_2D, tex->texture);
            const bool clampS = (tile.masks == 0) || (tile.cms & G_TX_CLAMP);
            const bool clampT = (tile.maskt == 0) || (tile.cmt & G_TX_CLAMP);
            const int wrapS = clampS ? GL_CLAMP_TO_EDGE : ((tile.cms & G_TX_MIRROR) ? GL_MIRRORED_REPEAT : GL_REPEAT);
            const int wrapT = clampT ? GL_CLAMP_TO_EDGE : ((tile.cmt & G_TX_MIRROR) ? GL_MIRRORED_REPEAT : GL_REPEAT);
            const int filter = bilerp ? GL_LINEAR : GL_NEAREST;
            if ((tex->wrapS != wrapS) || (tex->wrapT != wrapT) || (tex->filter != filter)) {
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrapS);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrapT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
                tex->wrapS = wrapS;
                tex->wrapT = wrapT;
                tex->filter = filter;
            }

            // N64 bilinear weights texel floor(uv) with frac(uv); GL LINEAR does that at (uv + 0.5) / size.
            const float half = bilerp ? 0.5f : 0.0f;
            const float tileParams[4] = { shiftScale(tile.shifts) * persp, shiftScale(tile.shiftt) * persp, tile.uls / 4.0f - half, tile.ult / 4.0f - half };
            const float texSize[4] = { 1.0f / tex->width, 1.0f / tex->height, 0.0f, 0.0f };
            if (p->locTile[i] >= 0) glUniform4fv(p->locTile[i], 1, tileParams);
            if (p->locTexSize[i] >= 0) glUniform4fv(p->locTexSize[i], 1, texSize);
        }
        glActiveTexture(GL_TEXTURE0);

        if (!glc.attribsEnabled) {
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glEnableVertexAttribArray(2);
            glc.attribsEnabled = true;
        }
        const uint8_t *base = reinterpret_cast<const uint8_t *>(vertices);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), base + offsetof(Vertex, x));
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), base + offsetof(Vertex, r));
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), base + offsetof(Vertex, s));
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(vertexCount));
        statDraws++;
    }

    // Stale targets can overlap newer ones (0x656000 spans the mission's 0x690000): take the closest start address.
    Backend::Target *Backend::targetForOrigin(uint32_t viOrigin) {
        Target *found = nullptr;
        for (auto &t : targets) {
            const uint32_t bytes = uint32_t(t->width) * t->height * 2;
            if ((viOrigin >= t->address) && (viOrigin < t->address + bytes) && ((found == nullptr) || (t->address > found->address))) {
                found = t.get();
            }
        }
        return found;
    }

    bool Backend::readTarget(uint32_t viOrigin, std::vector<uint8_t> &rgba, int &width, int &height) {
        if (Target *t = targetForOrigin(viOrigin)) {
            width = t->width * resScale;
            height = t->height * resScale;
            std::vector<uint8_t> raw(size_t(width) * height * 4);
            bindTarget(t);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, raw.data());
            rgba.resize(raw.size());
            const size_t row = size_t(width) * 4;
            for (int y = 0; y < height; y++) {
                memcpy(&rgba[size_t(y) * row], &raw[size_t(height - 1 - y) * row], row);
            }
            return true;
        }
        return false;
    }

    bool Backend::present(const ViView &vi, const std::vector<OverlayItem> &overlay, bool force) {
        // VI_H_START / VI_V_START hold start << 16 | end (pixels / half-lines); scales are 2.10 fixed point in the low 12 bits.
        const uint32_t hStart = (vi.hStart >> 16) & 0x3FF, hEnd = vi.hStart & 0x3FF;
        const uint32_t vStart = (vi.vStart >> 16) & 0x3FF, vEnd = vi.vStart & 0x3FF;
        const uint32_t xScale = vi.xScale & 0xFFF, yScale = vi.yScale & 0xFFF;
        const bool useView = (vi.width != 0);
        const bool blank = useView && ((hEnd <= hStart) || (vEnd <= vStart) || (xScale == 0) || (yScale == 0));
        Target *shown = blank ? nullptr : targetForOrigin(vi.origin);
        size_t overlayHash = overlay.size();
        for (const OverlayItem &item : overlay) {
            overlayHash = overlayHash * 31 + std::hash<std::string>()(item.text) + size_t(item.color);
        }
        const bool viewChanged = (memcmp(&vi, &lastPresentedView, sizeof(ViView)) != 0) || (overlayHash != lastOverlayHash);
        if (!force) {
            if (blank ? lastPresentedBlank : ((shown == nullptr) || (!shown->drawnSincePresent && (shown->address == lastPresentedAddress) && !viewChanged))) {
                return false;
            }
        }
        lastPresentedBlank = blank;
        lastPresentedView = vi;
        lastOverlayHash = overlayHash;

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        boundTarget = nullptr;
        glViewport(0, 0, screenWidth, screenHeight);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glDepthMask(GL_TRUE);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glc = GLStateCache{};
        if (shown != nullptr) {
            int viewW = screenWidth, viewH = screenHeight;
            if (stretchToScreen) {
                glViewport(0, 0, screenWidth, screenHeight);
            }
            else {
                // 4:3 pillarbox.
                viewW = std::min(screenWidth, (screenHeight * 4) / 3);
                viewH = (viewW * 3) / 4;
                glViewport((screenWidth - viewW) / 2, (screenHeight - viewH) / 2, viewW, viewH);
            }
            glUseProgram(presentProgram);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, shown->color);

            // Source window in image pixels: the lines and columns the VI scans out from its origin. Cinematics show only ~334 of
            // 480 lines (letterbox), the intro stretches ~335 lines over the screen, gameplay shows 442 of 448.
            const float w = float(shown->width), h = float(shown->height);
            float c0 = 0.0f, c1 = w, r0 = 0.0f, r1 = h;
            if (useView) {
                const uint32_t offsetPixels = (vi.origin - shown->address) / 2;
                r0 = float(offsetPixels / shown->width);
                c0 = float(offsetPixels % shown->width);
                // VI_WIDTH is the VI's line stride in pixels; a stride of two image rows shows one field of an interlaced image, all of whose rows are drawn here.
                const float rowsPerLine = float(vi.width) / w;
                const float lines = (float(vEnd - vStart) * 0.5f) * (float(yScale) / 1024.0f);
                r1 = std::min(h, r0 + lines * rowsPerLine);
                c1 = std::min(w, c0 + float(hEnd - hStart) * (float(xScale) / 1024.0f));
            }
            // The rightmost column is left out: the game's 3D scissor stops one pixel short of it (511 of 512), which the TV hid in overscan.
            c1 = std::min(c1, w - 1.0f);
            const float u0 = c0 / w, u1 = c1 / w, vTop = 1.0f - r0 / h, vBottom = 1.0f - r1 / h;

            // Destination: the whole screen when stretched; in 4:3, where the VI places the window on the TV (letterbox bars stay).
            float x0 = -1.0f, x1 = 1.0f, yTop = 1.0f, yBottom = -1.0f;
            if (useView && !stretchToScreen) {
                constexpr float HFirst = 0x6C, HLast = 0x2EC, VFirst = 0x23, VLast = 0x1FF;
                x0 = std::clamp(-1.0f + 2.0f * (float(hStart) - HFirst) / (HLast - HFirst), -1.0f, 1.0f);
                x1 = std::clamp(-1.0f + 2.0f * (float(hEnd) - HFirst) / (HLast - HFirst), -1.0f, 1.0f);
                yTop = std::clamp(1.0f - 2.0f * (float(vStart) - VFirst) / (VLast - VFirst), -1.0f, 1.0f);
                yBottom = std::clamp(1.0f - 2.0f * (float(vEnd) - VFirst) / (VLast - VFirst), -1.0f, 1.0f);
            }
            const float quad[] = {
                x0, yTop,    0.0f, 1.0f,  u0, vTop,
                x1, yTop,    0.0f, 1.0f,  u1, vTop,
                x0, yBottom, 0.0f, 1.0f,  u0, vBottom,
                x1, yBottom, 0.0f, 1.0f,  u1, vBottom,
                x1, yTop,    0.0f, 1.0f,  u1, vTop,
                x0, yBottom, 0.0f, 1.0f,  u0, vBottom
            };
            glDisableVertexAttribArray(1);
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), quad);
            glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), quad + 4);
            glDrawArrays(GL_TRIANGLES, 0, 6);
            drawOverlay(overlay, x0, x1, yTop, yBottom, viewW, viewH);
            const uint32_t draws = shown->drawsSincePresent;
            if (draws == 0) {
                statStalePresents++;
            }
            else {
                if ((avgPresentDraws > 0.0f) && (float(draws) * 4.0f < avgPresentDraws)) {
                    statLowPresents++;
                }
                avgPresentDraws = (avgPresentDraws == 0.0f) ? float(draws) : (avgPresentDraws * 0.9f + float(draws) * 0.1f);
            }
            shown->drawsSincePresent = 0;
            shown->drawnSincePresent = false;
            shown->lastUse = frameCounter;
            lastPresentedAddress = shown->address;
        }

        if (swapCallback) {
            swapCallback();
        }
        for (auto &t : targets) {
            t->scenesThisFrame = 0;
        }
        frameCounter++;
        evictTargets();
        statPresents++;
        return true;
    }
}
