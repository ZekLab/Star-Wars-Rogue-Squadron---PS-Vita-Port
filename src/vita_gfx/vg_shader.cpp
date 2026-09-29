#include "vg_shader.h"

#include <cstdio>

#include "shared/rt64_blender.h"
#include "shared/rt64_color_combiner.h"
#include "shared/rt64_other_mode.h"

#include "vg_renderer.h"

namespace vg {
    using interop::Blender;
    using interop::ColorCombiner;
    using interop::OtherMode;

    ShaderKey makeShaderKey(const DrawState &s) {
        ShaderKey k;
        k.combineL = s.combineL;
        k.combineH = s.combineH;
        k.otherModeL = s.otherModeL & (0xFFFF0000U | (3U << G_MDSFT_ALPHACOMPARE) | CVG_X_ALPHA | FORCE_BL);
        k.otherModeH = s.otherModeH & (3U << G_MDSFT_CYCLETYPE);
        k.flags = (s.kind == DrawKind::Rect) ? SHADER_FLAG_RECT : 0;
        return k;
    }

    bool shaderUsesTexture(const ShaderKey &key, uint32_t index) {
        const ColorCombiner cc = { key.combineL, key.combineH };
        const OtherMode om = { key.otherModeL, key.otherModeH };
        if (om.cycleType() == G_CYC_FILL) {
            return false;
        }
        return cc.usesTexture(om, index, false);
    }

    static std::string splat3(const std::string &x) {
        return "float3(" + x + ", " + x + ", " + x + ")";
    }

    static std::string colorInput(ColorCombiner::ColorInput in, bool secondCycle) {
        switch (in) {
        case ColorCombiner::C_COMBINED: return "cc.rgb";
        case ColorCombiner::C_TEXEL0: return secondCycle ? "t1.rgb" : "t0.rgb";
        case ColorCombiner::C_TEXEL1: return secondCycle ? "t0.rgb" : "t1.rgb";
        case ColorCombiner::C_PRIMITIVE: return "uPrim.rgb";
        case ColorCombiner::C_SHADE: return "shade.rgb";
        case ColorCombiner::C_ENVIRONMENT: return "uEnv.rgb";
        case ColorCombiner::C_COMBINED_ALPHA: return splat3("cc.a");
        case ColorCombiner::C_TEXEL0_ALPHA: return splat3(secondCycle ? "t1.a" : "t0.a");
        case ColorCombiner::C_TEXEL1_ALPHA: return splat3(secondCycle ? "t0.a" : "t1.a");
        case ColorCombiner::C_PRIMITIVE_ALPHA: return splat3("uPrim.a");
        case ColorCombiner::C_SHADE_ALPHA: return splat3("shade.a");
        case ColorCombiner::C_ENV_ALPHA: return splat3("uEnv.a");
        case ColorCombiner::C_LOD_FRACTION: return splat3("uMisc.x");
        case ColorCombiner::C_PRIM_LOD_FRAC: return splat3("uMisc.y");
        case ColorCombiner::C_NOISE: return splat3("0.5");
        case ColorCombiner::C_K4: return splat3("uMisc.z");
        case ColorCombiner::C_K5: return splat3("uMisc.w");
        case ColorCombiner::C_ONE: return splat3("1.0");
        case ColorCombiner::C_KEY_CENTER:
        case ColorCombiner::C_KEY_SCALE:
        case ColorCombiner::C_ZERO:
        default: return splat3("0.0");
        }
    }

    static std::string alphaInput(ColorCombiner::AlphaInput in, bool secondCycle) {
        switch (in) {
        case ColorCombiner::A_COMBINED: return "cc.a";
        case ColorCombiner::A_TEXEL0: return secondCycle ? "t1.a" : "t0.a";
        case ColorCombiner::A_TEXEL1: return secondCycle ? "t0.a" : "t1.a";
        case ColorCombiner::A_PRIMITIVE: return "uPrim.a";
        case ColorCombiner::A_SHADE: return "shade.a";
        case ColorCombiner::A_ENVIRONMENT: return "uEnv.a";
        case ColorCombiner::A_LOD_FRACTION: return "uMisc.x";
        case ColorCombiner::A_PRIM_LOD_FRAC: return "uMisc.y";
        case ColorCombiner::A_ONE: return "1.0";
        case ColorCombiner::A_ZERO:
        default: return "0.0";
        }
    }

    // One combiner cycle as in ColorCombiner::runCycle, including the second-cycle input wrap.
    static void emitCycle(std::string &code, const ColorCombiner &cc, uint32_t cycle, bool twoCycle) {
        const bool secondCycleInputs = (cycle == 1);
        const bool secondCycle = twoCycle && secondCycleInputs;
        const auto CA = cc.decodeColorInput(0, secondCycleInputs);
        const auto CB = cc.decodeColorInput(1, secondCycleInputs);
        const auto CC = cc.decodeColorInput(2, secondCycleInputs);
        const auto CD = cc.decodeColorInput(3, secondCycleInputs);
        const auto AA = cc.decodeAlphaInput(0, secondCycleInputs);
        const auto AB = cc.decodeAlphaInput(1, secondCycleInputs);
        const auto AC = cc.decodeAlphaInput(2, secondCycleInputs);
        const auto AD = cc.decodeAlphaInput(3, secondCycleInputs);
        if (secondCycle) {
            code += (AC == ColorCombiner::A_COMBINED) ? "  cc.a = wrap1(cc.a, WRAP_C_LO, WRAP_C_HI);\n" : "  cc.a = wrap1(cc.a, WRAP_ABD_LO, WRAP_ABD_HI);\n";
            code += (CC == ColorCombiner::C_COMBINED) ? "  cc.rgb = wrap3(cc.rgb, WRAP_C_LO, WRAP_C_HI);\n" : "  cc.rgb = wrap3(cc.rgb, WRAP_ABD_LO, WRAP_ABD_HI);\n";
        }

        code += "  {\n";
        code += "    float3 c = (" + colorInput(CA, secondCycle) + " - " + colorInput(CB, secondCycle) + ") * " +
            colorInput(CC, secondCycle) + " + " + colorInput(CD, secondCycle) + ";\n";
        code += "    float a = (" + alphaInput(AA, secondCycle) + " - " + alphaInput(AB, secondCycle) + ") * " +
            alphaInput(AC, secondCycle) + " + " + alphaInput(AD, secondCycle) + ";\n";
        code += "    cc = float4(c, a);\n";
        code += "  }\n";
    }

    static std::string blendPM(Blender::InputPM pm, const std::string &ccOrBlender) {
        switch (pm) {
        case Blender::PM_CC_OR_BLENDER: return ccOrBlender;
        case Blender::PM_BLEND_COLOR: return "uBlend.rgb";
        case Blender::PM_FOG_COLOR: return "uFog.rgb";
        default: return "float3(1.0, 1.0, 1.0)";
        }
    }

    static std::string blendA(Blender::InputA a) {
        switch (a) {
        case Blender::A_CC_ALPHA: return "cc.a";
        case Blender::A_FOG_ALPHA: return "uFog.a";
        case Blender::A_SHADE_ALPHA: return "shade.a";
        default: return "0.0";
        }
    }

    static std::string blendB(Blender::InputB b, const std::string &aMul) {
        switch (b) {
        case Blender::B_ONE_MINUS_A: return "(1.0 - " + aMul + ")";
        case Blender::B_FRAMEBUFFER_ALPHA: return "1.0";
        case Blender::B_ONE: return "1.0";
        default: return "0.0";
        }
    }

    struct BlendGen {
        std::string code;
        std::string color = "float3(0.0, 0.0, 0.0)";
        std::string alpha = "0.0";
        bool passthroughEnabled = false;
        Blender::InputPM passthroughInput = Blender::PM_CC_OR_BLENDER;
        int temp = 0;

        std::string newTemp(const std::string &type, const std::string &expr) {
            const std::string name = "bl" + std::to_string(temp++);
            code += "  " + type + " " + name + " = " + expr + ";\n";
            return name;
        }
    };

    // Blender::runCycle resolved at generation time.
    static void emitBlendCycle(BlendGen &g, bool forceBlend, bool lastCycle, bool notFirstCycle,
        Blender::InputPM P, Blender::InputPM M, Blender::InputA A, Blender::InputB B)
    {
        const bool replaceCcWithBlender = notFirstCycle && !g.passthroughEnabled;
        const std::string inputColor = replaceCcWithBlender ? g.color : std::string("cc.rgb");
        if (lastCycle && !forceBlend) {
            if (P == Blender::PM_FRAMEBUFFER_COLOR) {
                g.alpha = "0.0";
            }
            else {
                g.color = g.newTemp("float3", blendPM(P, inputColor));
                g.alpha = "1.0";
            }
            return;
        }

        const bool anyInputIsZero = (A == Blender::A_ZERO) || (B == Blender::B_ZERO);
        const bool duplicateInput1MA = (P == M) && (B == Blender::B_ONE_MINUS_A);
        const bool passthrough = anyInputIsZero || duplicateInput1MA;
        bool framebufferColor = (P == Blender::PM_FRAMEBUFFER_COLOR) || (M == Blender::PM_FRAMEBUFFER_COLOR);
        if (g.passthroughEnabled) {
            if (P == Blender::PM_CC_OR_BLENDER) {
                P = g.passthroughInput;
                framebufferColor |= (g.passthroughInput == Blender::PM_FRAMEBUFFER_COLOR);
            }
            else if (M == Blender::PM_CC_OR_BLENDER) {
                M = g.passthroughInput;
                framebufferColor |= (g.passthroughInput == Blender::PM_FRAMEBUFFER_COLOR);
            }
        }

        if (passthrough) {
            if (!lastCycle) {
                g.passthroughInput = (A == Blender::A_ZERO) ? M : P;
                g.passthroughEnabled = true;
            }
            else if (framebufferColor) {
                g.alpha = "0.0";
            }
            else {
                g.color = g.newTemp("float3", blendPM((A == Blender::A_ZERO) ? M : P, inputColor));
                g.alpha = "1.0";
            }
        }
        else if (framebufferColor) {
            if (P == Blender::PM_FRAMEBUFFER_COLOR) {
                g.color = g.newTemp("float3", blendPM(M, inputColor));
                g.alpha = g.newTemp("float", "1.0 - " + blendA(A));
            }
            else if (M == Blender::PM_FRAMEBUFFER_COLOR) {
                g.color = g.newTemp("float3", blendPM(P, inputColor));
                g.alpha = g.newTemp("float", blendA(A));
            }
        }
        else {
            const std::string aMul = g.newTemp("float", blendA(A));
            const std::string bMul = g.newTemp("float", blendB(B, aMul));
            const std::string num = g.newTemp("float3", "fmod(" + blendPM(P, inputColor) + " * " + aMul + " + " + blendPM(M, inputColor) + " * " + bMul + ", " + splat3("BLEND_OVERFLOW") + ")");
            g.color = g.newTemp("float3", num + " / max(" + aMul + " + " + bMul + ", 1.0 / 255.0)");
            g.alpha = "1.0";
        }
    }

    // Blender::run; returns the float4 expression and whether GL alpha blending is needed.
    static std::string emitBlender(std::string &code, const OtherMode &om, bool &alphaBlend) {
        alphaBlend = Blender::usesAlphaBlend(om);
        const uint32_t blenderInputs = om.blenderInputs();
        const auto P0 = Blender::decodeInputP(blenderInputs, false);
        const auto M0 = Blender::decodeInputM(blenderInputs, false);
        const auto A0 = Blender::decodeInputA(blenderInputs, false);
        const auto B0 = Blender::decodeInputB(blenderInputs, false);
        const auto P1 = Blender::decodeInputP(blenderInputs, true);
        const auto M1 = Blender::decodeInputM(blenderInputs, true);
        const auto A1 = Blender::decodeInputA(blenderInputs, true);
        const auto B1 = Blender::decodeInputB(blenderInputs, true);

        const Blender::EmulationRequirements reqs = Blender::checkEmulationRequirements(om);
        if (!reqs.simpleEmulation) {
            if (reqs.approximateEmulation == Blender::Approximation::CombinerFramebuffer1MA_SquareMix) {
                return "float4(cc.rgb, cc.a * cc.a)";
            }
            if (reqs.approximateEmulation == Blender::Approximation::AnyFramebuffer1MA_MultiplyMix) {
                return "float4(" + blendPM(P0, "cc.rgb") + ", " + blendA(A0) + " * " + blendA(A1) + ")";
            }
        }

        const uint32_t combinerCycles = Blender::combineCycleCount(om);
        if (combinerCycles == 0) {
            return "float4(cc.rgb, 1.0)";
        }

        BlendGen g;
        const bool forceBlend = om.forceBlend();
        emitBlendCycle(g, forceBlend, (combinerCycles == 1), false, P0, M0, A0, B0);
        if (combinerCycles > 1) {
            emitBlendCycle(g, forceBlend, true, true, P1, M1, A1, B1);
        }

        code += g.code;
        return "float4(" + g.color + ", " + g.alpha + ")";
    }

    static const char *kFragmentHelpers =
        "#define WRAP_C_LO (-1.0 - 1.0 / 255.0)\n"
        "#define WRAP_C_HI (1.0 + 1.0 / 255.0)\n"
        "#define WRAP_ABD_LO (-0.5 - 1.0 / 255.0)\n"
        "#define WRAP_ABD_HI (1.5 + 1.0 / 255.0)\n"
        "#define BLEND_OVERFLOW (1.0 + 8.0 / 255.0)\n"
        "float wrap1(float i, float lo, float hi) {\n"
        "  float r = hi - lo;\n"
        "  i += r * step(i, lo);\n"
        "  i -= r * step(hi, i);\n"
        "  return i;\n"
        "}\n"
        "float3 wrap3(float3 i, float lo, float hi) {\n"
        "  float r = hi - lo;\n"
        "  i += r * step(i, float3(lo, lo, lo));\n"
        "  i -= r * step(float3(hi, hi, hi), i);\n"
        "  return i;\n"
        "}\n";

    static const char *kGlslPrelude =
        "#version 120\n"
        "#define float2 vec2\n"
        "#define float3 vec3\n"
        "#define float4 vec4\n"
        "#define lerp mix\n"
        "#define frac fract\n"
        "#define fmod mod\n"
        "#define tex2D texture2D\n";

    static const char *kUniformList[][2] = {
        { "float4", "uPrim" },
        { "float4", "uEnv" },
        { "float4", "uFog" },
        { "float4", "uBlend" },
        { "float4", "uFill" },
        { "float4", "uMisc" },
        { "float4", "uTile0" },
        { "float4", "uTile1" },
        { "float4", "uTexSize0" },
        { "float4", "uTexSize1" }
    };

    ShaderSource generateShader(const ShaderKey &key, bool cg) {
        ShaderSource src;
        const ColorCombiner cc = { key.combineL, key.combineH };
        const OtherMode om = { key.otherModeL, key.otherModeH };
        const uint32_t cycleType = om.cycleType();
        src.usesTexture[0] = shaderUsesTexture(key, 0);
        src.usesTexture[1] = shaderUsesTexture(key, 1);
        src.alphaBlend = false;

        std::string body;
        body += "  float4 shade = vColor;\n";
        body += "  float4 t0 = float4(0.0, 0.0, 0.0, 1.0);\n";
        body += "  float4 t1 = float4(0.0, 0.0, 0.0, 1.0);\n";
        for (int i = 0; i < 2; i++) {
            if (src.usesTexture[i]) {
                const std::string n = std::to_string(i);
                body += "  t" + n + " = tex2D(uTex" + n + ", (vTex * uTile" + n + ".xy - uTile" + n + ".zw) * uTexSize" + n + ".xy);\n";
            }
        }

        if (cycleType == G_CYC_FILL) {
            body += "  float4 oc = uFill;\n";
        }
        else {
            body += "  float4 cc = float4(0.0, 0.0, 0.0, 0.0);\n";
            const bool twoCycle = (cycleType == G_CYC_2CYCLE);
            if (cycleType == G_CYC_COPY) {
                body += "  cc = t0;\n";
            }
            else {
                emitCycle(body, cc, twoCycle ? 0 : 1, twoCycle);
            }

            body += "  float acv = cc.a;\n";
            if (twoCycle) {
                emitCycle(body, cc, 1, twoCycle);
            }

            body += "  cc.rgb = clamp(wrap3(cc.rgb, WRAP_ABD_LO, WRAP_ABD_HI), float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0));\n";
            body += "  cc.a = clamp(wrap1(cc.a, WRAP_ABD_LO, WRAP_ABD_HI), 0.0, 1.0);\n";
            body += "  acv = clamp(wrap1(acv, WRAP_ABD_LO, WRAP_ABD_HI), 0.0, 1.0);\n";

            const uint32_t alphaCompare = om.alphaCompare();
            if (alphaCompare == G_AC_THRESHOLD) {
                body += "  if (acv < uBlend.a) discard;\n";
            }
            else if (alphaCompare == G_AC_DITHER) {
                body += "  if (acv < 0.5) discard;\n";
            }

            if (om.cvgXAlpha()) {
                body += "  if (cc.a < 0.125) discard;\n";
            }

            if (cycleType == G_CYC_COPY) {
                body += "  float4 oc = cc;\n";
            }
            else {
                std::string blendCode;
                const std::string outExpr = emitBlender(blendCode, om, src.alphaBlend);
                body += blendCode;
                body += "  float4 oc = " + outExpr + ";\n";
            }
        }

        if (cg) {
            src.vertex =
                "void main(float4 aPos, float4 aColor, float2 aTex,\n"
                "  out float4 gl_Position : POSITION, out float4 vColor : TEXCOORD1, out float2 vTex : TEXCOORD0) {\n"
                "  gl_Position = aPos;\n"
                "  vColor = aColor;\n"
                "  vTex = aTex;\n"
                "}\n";
            std::string params = "float4 vColor : TEXCOORD1, float2 vTex : TEXCOORD0";
            for (const auto &u : kUniformList) {
                params += std::string(",\n  uniform ") + u[0] + " " + u[1];
            }
            params += ",\n  uniform sampler2D uTex0 : TEXUNIT0, uniform sampler2D uTex1 : TEXUNIT1";
            src.fragment = std::string(kFragmentHelpers) + "float4 main(" + params + ") : COLOR {\n" + body + "  return oc;\n}\n";
        }
        else {
            src.vertex = std::string(kGlslPrelude) +
                "attribute vec4 aPos;\n"
                "attribute vec4 aColor;\n"
                "attribute vec2 aTex;\n"
                "varying vec4 vColor;\n"
                "varying vec2 vTex;\n"
                "void main() {\n"
                "  gl_Position = aPos;\n"
                "  vColor = aColor;\n"
                "  vTex = aTex;\n"
                "}\n";
            std::string decls = "varying vec4 vColor;\nvarying vec2 vTex;\nuniform sampler2D uTex0;\nuniform sampler2D uTex1;\n";
            for (const auto &u : kUniformList) {
                decls += std::string("uniform ") + u[0] + " " + u[1] + ";\n";
            }
            src.fragment = std::string(kGlslPrelude) + decls + kFragmentHelpers + "void main() {\n" + body + "  gl_FragColor = oc;\n}\n";
        }

        return src;
    }
}
