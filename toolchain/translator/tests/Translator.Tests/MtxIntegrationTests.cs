using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using Translator.Core.Analysis;
using Translator.Core.Analysis.Representation;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Loading;
using Translator.Core.Parsing.Dol;
using Translator.Core.Parsing.Rel;
using Translator.Core.Translation;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public class MtxIntegrationTests
{
    private static string RepositoryRoot => Path.GetFullPath(Path.Combine(
        AppContext.BaseDirectory, "..", "..", "..", "..", "..", ".."));

    private enum ParamKind
    {
        Gpr,
        Fpr
    }

    private enum ReturnKind
    {
        Void,
        Gpr,
        Float
    }

    private record MtxFuncSpec(uint Address, string Name, ReturnKind Return, IReadOnlyList<ParamKind> Params);

    private static (int ExitCode, string Output) RunProcess(string fileName, string arguments, TimeSpan? timeout = null)
    {
        using var process = new Process
        {
            StartInfo = new ProcessStartInfo
            {
                FileName = fileName,
                Arguments = arguments,
                RedirectStandardError = true,
                RedirectStandardOutput = true,
                UseShellExecute = false
            }
        };

        process.Start();
        var stdoutTask = process.StandardOutput.ReadToEndAsync();
        var stderrTask = process.StandardError.ReadToEndAsync();
        var exitedInTime = true;
        if (timeout.HasValue)
        {
            exitedInTime = process.WaitForExit((int)timeout.Value.TotalMilliseconds);
        }
        else
        {
            process.WaitForExit();
        }

        if (!exitedInTime)
        {
            try
            {
                process.Kill(entireProcessTree: true);
            }
            catch
            {
                // ignored: process might have already exited between WaitForExit and Kill
            }

            process.WaitForExit();
            var staleOutput = stdoutTask.GetAwaiter().GetResult() + stderrTask.GetAwaiter().GetResult();
            throw new TimeoutException($"Process '{fileName}' timed out after {timeout!.Value.TotalSeconds:F0}s. Output before kill:\n{staleOutput}");
        }

        process.WaitForExit();
        var output = stdoutTask.GetAwaiter().GetResult() + stderrTask.GetAwaiter().GetResult();
        return (process.ExitCode, output);
    }

    private static FunctionTranslator BuildTranslator()
    {
        var root = RepositoryRoot;
        var dol = DolFile.Load(Path.Combine(root, "assets", "main.dol"));
        var rel = RelFile.Load(Path.Combine(root, "assets", "StaticR.rel"))
            .BuildImage(0x805102E0);
        var image = new ProgramImageBuilder().Build(dol, rel);
        return new FunctionTranslator(image);
    }

    [Fact]
    public void Translated_MTX_Primitives_ExecuteCorrectly()
    {
        var specs = new[]
        {
            Spec(0x80199D04, "PSMTXIdentity_80199d04", ReturnKind.Void, ParamKind.Gpr),
            Spec(0x80199D30, "PSMTXCopy_80199d30", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x80199D64, "PSMTXConcat_80199d64", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x80199E30, "PSMTXConcatArray_80199e30", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x80199FC8, "PSMTXInverse_80199fc8", ReturnKind.Gpr, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x8019A0C0, "PSMTXInvXpose_8019a0c0", ReturnKind.Gpr, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x8019A188, "PSMTXRotRad_8019a188", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Fpr),
            Spec(0x8019A204, "PSMTXRotTrig_8019a204", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Fpr, ParamKind.Fpr),
            Spec(0x8019A2B4, "PSMTXRotAxisRadInternal_8019a2b4", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Fpr, ParamKind.Fpr),
            Spec(0x8019A364, "PSMTXRotAxisRad_8019a364", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Fpr),
            Spec(0x8019A3E0, "PSMTXTrans_8019a3e0", ReturnKind.Void, ParamKind.Gpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr),
            Spec(0x8019A414, "PSMTXTransApply_8019a414", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr),
            Spec(0x8019A460, "PSMTXScale_8019a460", ReturnKind.Void, ParamKind.Gpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr),
            Spec(0x8019A488, "PSMTXScaleApply_8019a488", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr),
            Spec(0x8019A4E0, "PSMTXQuat_8019a4e0", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x8019A584, "C_MTXLookAt_8019a584", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x8019A6F8, "C_MTXLightFrustum_8019a6f8", ReturnKind.Void, ParamKind.Gpr,
                ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr),
            Spec(0x8019A79C, "C_MTXLightPerspective_8019a79c", ReturnKind.Void, ParamKind.Gpr,
                ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr),
            Spec(0x8019A894, "C_MTXLightOrtho_8019a894", ReturnKind.Void, ParamKind.Gpr,
                ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr),
            Spec(0x8019A91C, "PSMTXMultVec_8019a91c", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x8019A970, "PSMTXMultVecSR_8019a970", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x8019A9C4, "C_MTXFrustum_8019a9c4", ReturnKind.Void, ParamKind.Gpr,
                ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr),
            Spec(0x8019AA60, "C_MTXPerspective_8019aa60", ReturnKind.Void, ParamKind.Gpr,
                ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr),
            Spec(0x8019AB4C, "C_MTXOrtho_8019ab4c", ReturnKind.Void, ParamKind.Gpr,
                ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr, ParamKind.Fpr),
            Spec(0x8019ABE4, "MTX__PSVECAdd_8019abe4", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x8019AC08, "MTX__PSVECScale_8019ac08", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Fpr),
            Spec(0x8019AC24, "MTX__PSVECNormalize_8019ac24", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x8019AC68, "MTX__PSVECMag_8019ac68", ReturnKind.Float, ParamKind.Gpr),
            Spec(0x8019ACAC, "MTX__PSVECDotProduct_8019acac", ReturnKind.Float, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x8019ACCC, "MTX__PSVECCrossProduct_8019accc", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x8019AD08, "MTX__C_VECHalfAngle_8019ad08", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x8019ADE0, "MTX__PSVECSquareDistance_8019ade0", ReturnKind.Float, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x8019AE08, "MTX__PSQUATMultiply_8019ae08", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x8019AE64, "MTX__PSQUATScale_8019ae64", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Fpr),
            Spec(0x8019AE80, "MTX__PSQUATDotProduct_8019ae80", ReturnKind.Float, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x8019AEA0, "MTX__PSQUATNormalize_8019aea0", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x8019AEF4, "MTX__PSQUATInverse_8019aef4", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x8019AF48, "MTX__C_QUATMtx_8019af48", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr),
            Spec(0x8019B114, "MTX__C_QUATLerp_8019b114", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Fpr),
            Spec(0x8019B178, "MTX__C_QUATSlerp_8019b178", ReturnKind.Void, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Gpr, ParamKind.Fpr),
        };

        var translator = BuildTranslator();
        var repoRoot = RepositoryRoot;
        var tempRoot = Path.Combine(repoRoot, "test_output", "mtx");
        Directory.CreateDirectory(tempRoot);

        var generatedFiles = new List<string>();
        var translations = TranslateWithDependencies(
            translator,
            specs.Select(s => (s.Address, s.Name)),
            tempRoot,
            generatedFiles);

        var translationMap = translations.ToDictionary(t => t.Name, StringComparer.Ordinal);
        foreach (var spec in specs)
        {
            Assert.True(translationMap.TryGetValue(spec.Name, out var translation), $"Missing translation for {spec.Name}.");
            AssertReturnAbiMatches(spec, translation.AbiClassification);
        }

        var harness = new StringBuilder();
        harness.AppendLine("#include <cassert>");
        harness.AppendLine("#include <cmath>");
        harness.AppendLine("#include <cstdlib>");
        harness.AppendLine("#include <cstdint>");
        harness.AppendLine("#include <cstdio>");
        harness.AppendLine("#include <array>");
        harness.AppendLine("#include <type_traits>");
        harness.AppendLine("#include \"memory.h\"");
        harness.AppendLine("#include \"ppc_runtime.h\"");
        harness.AppendLine("#include \"RuntimeConfig.h\"");

        // Stubs for runtime dependencies
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_Write8(uint8_t) {}");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_Write16(uint16_t) {}");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_Write32(uint32_t) {}");
        harness.AppendLine("extern \"C\" void GX_HLE_FIFO_WriteFloat(float) {}");
        harness.AppendLine("extern \"C\" void InsertAlarm_801a0620(CpuContext*) {}");
        harness.AppendLine("extern \"C\" void OS____TimeToSystemTime_801aade0(CpuContext*) {}");
        harness.AppendLine("void VI_HLE_SimulateRetrace(CpuContext*) {}");
        harness.AppendLine("void DumpRecentPcTrace(size_t) {}");
        harness.AppendLine("void MarkFatalErrorReported() {}");
        harness.AppendLine("extern \"C\" void InitializeDataSections();");

        foreach (var translation in translations)
        {
            harness.AppendLine(PrototypeFor(translation.AbiClassification));
        }

        harness.AppendLine("template <typename T>");
        harness.AppendLine("uint32_t ToGpr(T v) {");
        harness.AppendLine("    if constexpr (std::is_pointer_v<T>) {");
        harness.AppendLine("        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(v));");
        harness.AppendLine("    } else {");
        harness.AppendLine("        return static_cast<uint32_t>(v);");
        harness.AppendLine("    }");
        harness.AppendLine("}");

        harness.AppendLine("static void WriteF32(uint32_t addr, float value) {");
        harness.AppendLine("    Memory::WriteFloat32(addr, value);");
        harness.AppendLine("}");
        harness.AppendLine("static float ReadF32(uint32_t addr) {");
        harness.AppendLine("    return Memory::ReadFloat32(addr);");
        harness.AppendLine("}");
        harness.AppendLine("static void WriteMat34(uint32_t addr, const float* m) {");
        harness.AppendLine("    for (int i = 0; i < 12; ++i) WriteF32(addr + static_cast<uint32_t>(i * 4), m[i]);");
        harness.AppendLine("}");
        harness.AppendLine("static void ReadMat34(uint32_t addr, float* m) {");
        harness.AppendLine("    for (int i = 0; i < 12; ++i) m[i] = ReadF32(addr + static_cast<uint32_t>(i * 4));");
        harness.AppendLine("}");
        harness.AppendLine("static void WriteMat44(uint32_t addr, const float* m) {");
        harness.AppendLine("    for (int i = 0; i < 16; ++i) WriteF32(addr + static_cast<uint32_t>(i * 4), m[i]);");
        harness.AppendLine("}");
        harness.AppendLine("static void ReadMat44(uint32_t addr, float* m) {");
        harness.AppendLine("    for (int i = 0; i < 16; ++i) m[i] = ReadF32(addr + static_cast<uint32_t>(i * 4));");
        harness.AppendLine("}");
        harness.AppendLine("static void WriteVec3(uint32_t addr, const float* v) {");
        harness.AppendLine("    for (int i = 0; i < 3; ++i) WriteF32(addr + static_cast<uint32_t>(i * 4), v[i]);");
        harness.AppendLine("}");
        harness.AppendLine("static void ReadVec3(uint32_t addr, float* v) {");
        harness.AppendLine("    for (int i = 0; i < 3; ++i) v[i] = ReadF32(addr + static_cast<uint32_t>(i * 4));");
        harness.AppendLine("}");
        harness.AppendLine("static void WriteQuat(uint32_t addr, const float* q) {");
        harness.AppendLine("    for (int i = 0; i < 4; ++i) WriteF32(addr + static_cast<uint32_t>(i * 4), q[i]);");
        harness.AppendLine("}");
        harness.AppendLine("static void ReadQuat(uint32_t addr, float* q) {");
        harness.AppendLine("    for (int i = 0; i < 4; ++i) q[i] = ReadF32(addr + static_cast<uint32_t>(i * 4));");
        harness.AppendLine("}");
        harness.AppendLine("static float ReadFprPs0(const CpuContext& cpu, int index) {");
        harness.AppendLine("    return static_cast<float>(PPC_PsToScalar(cpu.fpr[index].d));");
        harness.AppendLine("}");
        harness.AppendLine("static float ReadFprFloat(const CpuContext& cpu, int index) {");
        harness.AppendLine("    return static_cast<float>(cpu.fpr[index].d);");
        harness.AppendLine("}");
        harness.AppendLine("static bool Near(float a, float b, float eps = 1e-4f) {");
        harness.AppendLine("    const float diff = std::fabs(a - b);");
        harness.AppendLine("    const float scale = std::fmax(1.0f, std::fmax(std::fabs(a), std::fabs(b)));\n    return diff <= eps * scale;\n}");
        harness.AppendLine("static float VecDot(const float* a, const float* b) {");
        harness.AppendLine("    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];");
        harness.AppendLine("}");
        harness.AppendLine("static float VecMag(const float* v) {");
        harness.AppendLine("    return std::sqrt(VecDot(v, v));");
        harness.AppendLine("}");
        harness.AppendLine("static void VecNormalize(const float* v, float* out) {");
        harness.AppendLine("    const float m = VecMag(v);\n    if (m == 0.0f) { out[0] = out[1] = out[2] = 0.0f; return; }\n    out[0] = v[0] / m; out[1] = v[1] / m; out[2] = v[2] / m;");
        harness.AppendLine("}");
        harness.AppendLine("static void VecCross(const float* a, const float* b, float* out) {");
        harness.AppendLine("    out[0] = a[1] * b[2] - a[2] * b[1];\n    out[1] = a[2] * b[0] - a[0] * b[2];\n    out[2] = a[0] * b[1] - a[1] * b[0];");
        harness.AppendLine("}");
        harness.AppendLine("static float VecSquareDistance(const float* a, const float* b) {");
        harness.AppendLine("    const float dx = a[0] - b[0]; const float dy = a[1] - b[1]; const float dz = a[2] - b[2];");
        harness.AppendLine("    return dx * dx + dy * dy + dz * dz;");
        harness.AppendLine("}");
        harness.AppendLine("static void VecHalfAngle(const float* a, const float* b, float* out) {");
        harness.AppendLine("    float an[3] = { -a[0], -a[1], -a[2] };");
        harness.AppendLine("    float bn[3] = { -b[0], -b[1], -b[2] };");
        harness.AppendLine("    VecNormalize(an, an);");
        harness.AppendLine("    VecNormalize(bn, bn);");
        harness.AppendLine("    float sum[3] = { an[0] + bn[0], an[1] + bn[1], an[2] + bn[2] };");
        harness.AppendLine("    const float len2 = VecDot(sum, sum);");
        harness.AppendLine("    if (len2 <= 1e-7f) { out[0] = sum[0]; out[1] = sum[1]; out[2] = sum[2]; return; }");
        harness.AppendLine("    VecNormalize(sum, out);");
        harness.AppendLine("}");
        harness.AppendLine("static void QuatMul(const float* a, const float* b, float* out) {");
        harness.AppendLine("    const float ax = a[0], ay = a[1], az = a[2], aw = a[3];");
        harness.AppendLine("    const float bx = b[0], by = b[1], bz = b[2], bw = b[3];");
        harness.AppendLine("    out[0] = aw * bx + bw * ax + ay * bz - az * by;");
        harness.AppendLine("    out[1] = aw * by + bw * ay + az * bx - ax * bz;");
        harness.AppendLine("    out[2] = aw * bz + bw * az + ax * by - ay * bx;");
        harness.AppendLine("    out[3] = aw * bw - (ax * bx + ay * by + az * bz);");
        harness.AppendLine("}");
        harness.AppendLine("static float QuatDot(const float* a, const float* b) {");
        harness.AppendLine("    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];");
        harness.AppendLine("}");
        harness.AppendLine("static void QuatNormalize(const float* q, float* out) {");
        harness.AppendLine("    const float d = QuatDot(q, q);\n    if (d == 0.0f) { out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; return; }\n    const float inv = 1.0f / std::sqrt(d);\n    out[0] = q[0] * inv; out[1] = q[1] * inv; out[2] = q[2] * inv; out[3] = q[3] * inv;");
        harness.AppendLine("}");
        harness.AppendLine("static void QuatInverse(const float* q, float* out) {");
        harness.AppendLine("    const float d = QuatDot(q, q);\n    if (d == 0.0f) { out[0] = out[1] = out[2] = 0.0f; out[3] = 1.0f; return; }\n    const float inv = 1.0f / d;\n    out[0] = -q[0] * inv; out[1] = -q[1] * inv; out[2] = -q[2] * inv; out[3] = q[3] * inv;");
        harness.AppendLine("}");
        harness.AppendLine("static void QuatLerp(const float* a, const float* b, float t, float* out) {");
        harness.AppendLine("    out[0] = a[0] + (b[0] - a[0]) * t;\n    out[1] = a[1] + (b[1] - a[1]) * t;\n    out[2] = a[2] + (b[2] - a[2]) * t;\n    out[3] = a[3] + (b[3] - a[3]) * t;");
        harness.AppendLine("}");
        harness.AppendLine("static void QuatSlerp(const float* a, const float* bIn, float t, float* out) {");
        harness.AppendLine("    float b[4] = { bIn[0], bIn[1], bIn[2], bIn[3] };");
        harness.AppendLine("    float dot = QuatDot(a, b);");
        harness.AppendLine("    if (dot < 0.0f) { dot = -dot; b[0] = -b[0]; b[1] = -b[1]; b[2] = -b[2]; b[3] = -b[3]; }");
        harness.AppendLine("    if (dot > 0.9995f) { QuatLerp(a, b, t, out); QuatNormalize(out, out); return; }");
        harness.AppendLine("    const float theta = std::acos(dot);\n    const float sinT = std::sin(theta);\n    const float w1 = std::sin((1.0f - t) * theta) / sinT;\n    const float w2 = std::sin(t * theta) / sinT;");
        harness.AppendLine("    out[0] = a[0] * w1 + b[0] * w2;\n    out[1] = a[1] * w1 + b[1] * w2;\n    out[2] = a[2] * w1 + b[2] * w2;\n    out[3] = a[3] * w1 + b[3] * w2;");
        harness.AppendLine("}");
        harness.AppendLine("static void ExpectMat34(const float* got, const float* exp, const char* label) {");
        harness.AppendLine("    for (int i = 0; i < 12; ++i) {");
        harness.AppendLine("        if (!Near(got[i], exp[i])) {");
        harness.AppendLine("            std::printf(\"[mtx] %s mismatch at %d: got %.6f exp %.6f\\n\", label, i, got[i], exp[i]);");
        harness.AppendLine("            assert(false);");
        harness.AppendLine("        }");
        harness.AppendLine("    }");
        harness.AppendLine("}");
        harness.AppendLine("static void ExpectMat44(const float* got, const float* exp, const char* label) {");
        harness.AppendLine("    for (int i = 0; i < 16; ++i) {");
        harness.AppendLine("        if (!Near(got[i], exp[i])) {");
        harness.AppendLine("            std::printf(\"[mtx] %s mismatch at %d: got %.6f exp %.6f\\n\", label, i, got[i], exp[i]);");
        harness.AppendLine("            assert(false);");
        harness.AppendLine("        }");
        harness.AppendLine("    }");
        harness.AppendLine("}");

        harness.AppendLine("static void RefIdentity(float* m) {");
        harness.AppendLine("    for (int i = 0; i < 12; ++i) m[i] = 0.0f;");
        harness.AppendLine("    m[0] = 1.0f; m[5] = 1.0f; m[10] = 1.0f;");
        harness.AppendLine("}");
        harness.AppendLine("static void RefScale(float* m, float x, float y, float z) {");
        harness.AppendLine("    for (int i = 0; i < 12; ++i) m[i] = 0.0f;");
        harness.AppendLine("    m[0] = x; m[5] = y; m[10] = z;");
        harness.AppendLine("}");
        harness.AppendLine("static void RefTrans(float* m, float x, float y, float z) {");
        harness.AppendLine("    RefIdentity(m);");
        harness.AppendLine("    m[3] = x; m[7] = y; m[11] = z;");
        harness.AppendLine("}");
        harness.AppendLine("static void RefRotTrig(float* m, char axis, float sinA, float cosA) {");
        harness.AppendLine("    for (int i = 0; i < 12; ++i) m[i] = 0.0f;");
        harness.AppendLine("    switch (axis) {");
        harness.AppendLine("        case 'x': case 'X':");
        harness.AppendLine("            m[0] = 1.0f; m[5] = cosA; m[6] = -sinA; m[9] = sinA; m[10] = cosA; break;");
        harness.AppendLine("        case 'y': case 'Y':");
        harness.AppendLine("            m[0] = cosA; m[2] = sinA; m[5] = 1.0f; m[8] = -sinA; m[10] = cosA; break;");
        harness.AppendLine("        case 'z': case 'Z':");
        harness.AppendLine("            m[0] = cosA; m[1] = -sinA; m[4] = sinA; m[5] = cosA; m[10] = 1.0f; break;");
        harness.AppendLine("        default: break;");
        harness.AppendLine("    }");
        harness.AppendLine("}");
        harness.AppendLine("static void RefRotRad(float* m, char axis, float rad) {");
        harness.AppendLine("    RefRotTrig(m, axis, sinf(rad), cosf(rad));");
        harness.AppendLine("}");
        harness.AppendLine("static void RefRotAxisRadInternal(float* m, const float* axis, float sinA, float cosA) {");
        harness.AppendLine("    float x = axis[0], y = axis[1], z = axis[2];");
        harness.AppendLine("    float len = std::sqrt(x * x + y * y + z * z);");
        harness.AppendLine("    if (len == 0.0f) { RefIdentity(m); return; }");
        harness.AppendLine("    x /= len; y /= len; z /= len;\n    const float t = 1.0f - cosA;\n    m[0] = t * x * x + cosA;\n    m[1] = t * x * y - sinA * z;\n    m[2] = t * x * z + sinA * y;\n    m[3] = 0.0f;\n    m[4] = t * x * y + sinA * z;\n    m[5] = t * y * y + cosA;\n    m[6] = t * y * z - sinA * x;\n    m[7] = 0.0f;\n    m[8] = t * x * z - sinA * y;\n    m[9] = t * y * z + sinA * x;\n    m[10] = t * z * z + cosA;\n    m[11] = 0.0f;\n}");
        harness.AppendLine("static void RefRotAxisRad(float* m, const float* axis, float rad) {");
        harness.AppendLine("    RefRotAxisRadInternal(m, axis, sinf(rad), cosf(rad));");
        harness.AppendLine("}");
        harness.AppendLine("static void RefQuat(float* m, const float* q) {");
        harness.AppendLine("    const float x = q[0], y = q[1], z = q[2], w = q[3];");
        harness.AppendLine("    const float norm = x*x + y*y + z*z + w*w;\n    const float s = norm == 0.0f ? 0.0f : (2.0f / norm);\n    m[0] = 1.0f - (y*y + z*z) * s;\n    m[1] = (x*y - z*w) * s;\n    m[2] = (x*z + y*w) * s;\n    m[3] = 0.0f;\n    m[4] = (x*y + z*w) * s;\n    m[5] = 1.0f - (x*x + z*z) * s;\n    m[6] = (y*z - x*w) * s;\n    m[7] = 0.0f;\n    m[8] = (x*z - y*w) * s;\n    m[9] = (y*z + x*w) * s;\n    m[10] = 1.0f - (x*x + y*y) * s;\n    m[11] = 0.0f;\n}");
        harness.AppendLine("static void RefConcat(const float* a, const float* b, float* out) {");
        harness.AppendLine("    for (int r = 0; r < 3; ++r) {\n        const int r0 = r * 4;\n        out[r0 + 0] = a[r0 + 0] * b[0] + a[r0 + 1] * b[4] + a[r0 + 2] * b[8];\n        out[r0 + 1] = a[r0 + 0] * b[1] + a[r0 + 1] * b[5] + a[r0 + 2] * b[9];\n        out[r0 + 2] = a[r0 + 0] * b[2] + a[r0 + 1] * b[6] + a[r0 + 2] * b[10];\n        out[r0 + 3] = a[r0 + 0] * b[3] + a[r0 + 1] * b[7] + a[r0 + 2] * b[11] + a[r0 + 3];\n    }\n}");
        harness.AppendLine("static void RefConcatArray(const float* a, const float* b, float* out, int count) {");
        harness.AppendLine("    for (int i = 0; i < count; ++i) { RefConcat(a, b + i * 12, out + i * 12); }");
        harness.AppendLine("}");
        harness.AppendLine("static bool RefInverse(const float* src, float* inv) {");
        harness.AppendLine("    const float a00 = src[0], a01 = src[1], a02 = src[2];\n    const float a10 = src[4], a11 = src[5], a12 = src[6];\n    const float a20 = src[8], a21 = src[9], a22 = src[10];\n    const float b00 = a11 * a22 - a12 * a21;\n    const float b01 = a12 * a20 - a10 * a22;\n    const float b02 = a10 * a21 - a11 * a20;\n    const float det = a00 * b00 + a01 * b01 + a02 * b02;\n    if (det == 0.0f) { return false; }\n    const float invDet = 1.0f / det;\n    const float r00 = b00 * invDet;\n    const float r01 = (a02 * a21 - a01 * a22) * invDet;\n    const float r02 = (a01 * a12 - a02 * a11) * invDet;\n    const float r10 = b01 * invDet;\n    const float r11 = (a00 * a22 - a02 * a20) * invDet;\n    const float r12 = (a02 * a10 - a00 * a12) * invDet;\n    const float r20 = b02 * invDet;\n    const float r21 = (a01 * a20 - a00 * a21) * invDet;\n    const float r22 = (a00 * a11 - a01 * a10) * invDet;\n    inv[0] = r00; inv[1] = r01; inv[2] = r02;\n    inv[4] = r10; inv[5] = r11; inv[6] = r12;\n    inv[8] = r20; inv[9] = r21; inv[10] = r22;\n    inv[3] = -(r00 * src[3] + r01 * src[7] + r02 * src[11]);\n    inv[7] = -(r10 * src[3] + r11 * src[7] + r12 * src[11]);\n    inv[11] = -(r20 * src[3] + r21 * src[7] + r22 * src[11]);\n    return true;\n}");
        harness.AppendLine("static bool RefInvXpose(const float* src, float* out) {");
        harness.AppendLine("    const float a00 = src[0], a01 = src[1], a02 = src[2];\n    const float a10 = src[4], a11 = src[5], a12 = src[6];\n    const float a20 = src[8], a21 = src[9], a22 = src[10];\n    const float c00 = a11 * a22 - a12 * a21;\n    const float c01 = a12 * a20 - a10 * a22;\n    const float c02 = a10 * a21 - a11 * a20;\n    const float c10 = a02 * a21 - a01 * a22;\n    const float c11 = a00 * a22 - a02 * a20;\n    const float c12 = a01 * a20 - a00 * a21;\n    const float c20 = a01 * a12 - a02 * a11;\n    const float c21 = a02 * a10 - a00 * a12;\n    const float c22 = a00 * a11 - a01 * a10;\n    const float det = a00 * c00 + a01 * c01 + a02 * c02;\n    if (det == 0.0f) { return false; }\n    const float invDet = 1.0f / det;\n    out[0] = c00 * invDet; out[1] = c01 * invDet; out[2] = c02 * invDet; out[3] = 0.0f;\n    out[4] = c10 * invDet; out[5] = c11 * invDet; out[6] = c12 * invDet; out[7] = 0.0f;\n    out[8] = c20 * invDet; out[9] = c21 * invDet; out[10] = c22 * invDet; out[11] = 0.0f;\n    return true;\n}");
        harness.AppendLine("static void RefTransApply(const float* src, float* dst, float x, float y, float z) {");
        harness.AppendLine("    for (int i = 0; i < 12; ++i) dst[i] = src[i];\n    dst[3] += x; dst[7] += y; dst[11] += z;\n}");
        harness.AppendLine("static void RefScaleApply(const float* src, float* dst, float x, float y, float z) {");
        harness.AppendLine("    for (int i = 0; i < 12; ++i) dst[i] = src[i];\n    for (int c = 0; c < 4; ++c) { dst[c] *= x; dst[4 + c] *= y; dst[8 + c] *= z; }\n}");
        harness.AppendLine("static void RefMultVec(const float* m, const float* v, float* out) {");
        harness.AppendLine("    out[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3];\n    out[1] = m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7];\n    out[2] = m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11];\n}");
        harness.AppendLine("static void RefMultVecSR(const float* m, const float* v, float* out) {");
        harness.AppendLine("    out[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];\n    out[1] = m[4] * v[0] + m[5] * v[1] + m[6] * v[2];\n    out[2] = m[8] * v[0] + m[9] * v[1] + m[10] * v[2];\n}");
        harness.AppendLine("static void RefLookAt(float* m, const float* camPos, const float* camUp, const float* target) {");
        harness.AppendLine("    float f[3] = { camPos[0] - target[0], camPos[1] - target[1], camPos[2] - target[2] };\n    auto norm = [](float* v) { float len = std::sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); if (len != 0.0f) { v[0] /= len; v[1] /= len; v[2] /= len; } };\n    norm(f);\n    float s[3] = { camUp[1] * f[2] - camUp[2] * f[1], camUp[2] * f[0] - camUp[0] * f[2], camUp[0] * f[1] - camUp[1] * f[0] };\n    norm(s);\n    float u[3] = { f[1] * s[2] - f[2] * s[1], f[2] * s[0] - f[0] * s[2], f[0] * s[1] - f[1] * s[0] };\n    m[0] = s[0]; m[1] = s[1]; m[2] = s[2]; m[3] = -(camPos[0]*s[0] + camPos[1]*s[1] + camPos[2]*s[2]);\n    m[4] = u[0]; m[5] = u[1]; m[6] = u[2]; m[7] = -(camPos[0]*u[0] + camPos[1]*u[1] + camPos[2]*u[2]);\n    m[8] = f[0]; m[9] = f[1]; m[10] = f[2]; m[11] = -(camPos[0]*f[0] + camPos[1]*f[1] + camPos[2]*f[2]);\n}");
        harness.AppendLine("static void RefFrustum(float* m, float t, float b, float l, float r, float n, float f) {");
        harness.AppendLine("    for (int i = 0; i < 16; ++i) m[i] = 0.0f;\n    const float invRL = 1.0f / (r - l);\n    const float invTB = 1.0f / (t - b);\n    const float invFN = 1.0f / (f - n);\n    m[0] = 2.0f * n * invRL;\n    m[5] = 2.0f * n * invTB;\n    m[2] = (r + l) * invRL;\n    m[6] = (t + b) * invTB;\n    m[10] = -n * invFN;\n    m[11] = -(f * n) * invFN;\n    m[14] = -1.0f;\n}");
        harness.AppendLine("static void RefPerspective(float* m, float fovY, float aspect, float n, float f) {");
        harness.AppendLine("    for (int i = 0; i < 16; ++i) m[i] = 0.0f;\n    const float fov = 0.5f * (3.14159265358979323846f / 180.0f) * fovY;\n    const float cot = 1.0f / tanf(fov);\n    const float invFN = 1.0f / (f - n);\n    m[0] = cot / aspect;\n    m[5] = cot;\n    m[10] = -n * invFN;\n    m[11] = -(f * n) * invFN;\n    m[14] = -1.0f;\n}");
        harness.AppendLine("static void RefOrtho(float* m, float t, float b, float l, float r, float n, float f) {");
        harness.AppendLine("    for (int i = 0; i < 16; ++i) m[i] = 0.0f;\n    const float invRL = 1.0f / (r - l);\n    const float invTB = 1.0f / (t - b);\n    const float invFN = 1.0f / (f - n);\n    m[0] = 2.0f * invRL;\n    m[3] = -(r + l) * invRL;\n    m[5] = 2.0f * invTB;\n    m[7] = -(t + b) * invTB;\n    m[10] = -1.0f * invFN;\n    m[11] = -f * invFN;\n    m[15] = 1.0f;\n}");
        harness.AppendLine("static void RefLightPerspective(float* m, float fovY, float aspect, float scaleS, float scaleT, float transS, float transT) {");
        harness.AppendLine("    for (int i = 0; i < 12; ++i) m[i] = 0.0f;\n    const float fov = 0.5f * (3.14159265358979323846f / 180.0f) * fovY;\n    const float cot = 1.0f / tanf(fov);\n    m[0] = scaleS * (cot / aspect);\n    m[5] = scaleT * cot;\n    m[2] = -transS;\n    m[6] = -transT;\n    m[10] = -1.0f;\n}");
        harness.AppendLine("static void RefLightFrustum(float* m, float t, float b, float l, float r, float n, float scaleS, float scaleT, float transS, float transT) {");
        harness.AppendLine("    for (int i = 0; i < 12; ++i) m[i] = 0.0f;\n    const float invRL = 1.0f / (r - l);\n    const float invTB = 1.0f / (t - b);\n    m[0] = scaleS * (2.0f * n * invRL);\n    m[2] = scaleS * (invRL * (r + l)) - transS;\n    m[5] = scaleT * (2.0f * n * invTB);\n    m[6] = scaleT * (invTB * (t + b)) - transT;\n    m[10] = -1.0f;\n}");
        harness.AppendLine("static void RefLightOrtho(float* m, float t, float b, float l, float r, float scaleS, float scaleT, float transS, float transT) {");
        harness.AppendLine("    for (int i = 0; i < 12; ++i) m[i] = 0.0f;\n    const float invRL = 1.0f / (r - l);\n    const float invTB = 1.0f / (t - b);\n    m[0] = scaleS * (2.0f * invRL);\n    m[3] = transS + scaleS * (invRL * -(r + l));\n    m[5] = scaleT * (2.0f * invTB);\n    m[7] = transT + scaleT * (invTB * -(t + b));\n    m[11] = 1.0f;\n}");

        harness.AppendLine("int main() {");
        harness.AppendLine("    std::puts(\"mtx harness start\");");
        harness.AppendLine("    Memory::Init(0x02000000);");
        harness.AppendLine("    InitializeDataSections();");
        harness.AppendLine("    CpuContext g_cpu{};");
        harness.AppendLine("    g_cpu.gpr[1] = 0x81700000;");
        harness.AppendLine("    g_cpu.gpr[2] = RuntimeConfig::SDA2_BASE;");
        harness.AppendLine("    g_cpu.gpr[13] = RuntimeConfig::SDA1_BASE;");
        harness.AppendLine("    CpuContextScope cpuScope(&g_cpu);");
        harness.AppendLine("    const uint32_t base = 0x80000000;");
        harness.AppendLine("    const uint32_t matA = base + 0x100;");
        harness.AppendLine("    const uint32_t matB = base + 0x200;");
        harness.AppendLine("    const uint32_t matC = base + 0x300;");
        harness.AppendLine("    const uint32_t matD = base + 0x400;");
        harness.AppendLine("    const uint32_t matArr = base + 0x500;");
        harness.AppendLine("    const uint32_t vecA = base + 0x800;");
        harness.AppendLine("    const uint32_t vecB = base + 0x820;");
        harness.AppendLine("    const uint32_t vecC = base + 0x840;");
        harness.AppendLine("    const uint32_t quatA = base + 0x900;");
        harness.AppendLine("    const uint32_t quatB = base + 0x920;");
        harness.AppendLine("    const uint32_t quatC = base + 0x940;");
        harness.AppendLine("    const uint32_t mat44A = base + 0xA00;");
        harness.AppendLine("    const uint32_t mat44B = base + 0xB00;");
        harness.AppendLine("    float a[12] = { 1.2f, 0.3f, -0.7f, 2.0f, -0.4f, 1.5f, 0.25f, -1.0f, 0.8f, -0.2f, 1.1f, 0.5f };");
        harness.AppendLine("    float b[12] = { 0.9f, -0.1f, 0.4f, -0.3f, 0.2f, 1.3f, -0.6f, 0.7f, -0.5f, 0.8f, 1.2f, -0.9f };");
        harness.AppendLine("    float c[12] = { -0.2f, 1.1f, 0.0f, 0.3f, 0.4f, -0.7f, 0.9f, -0.1f, 0.6f, 0.2f, 1.3f, -0.8f };");
        harness.AppendLine("    float vec[3] = { 1.0f, 2.0f, -3.0f };");
        harness.AppendLine("    float vecOut[3] = { 0.0f, 0.0f, 0.0f };");
        harness.AppendLine("    float quat[4] = { 0.2f, -0.5f, 0.3f, 0.7f };");
        harness.AppendLine("    float zero34[12] = {};");
        harness.AppendLine("    float zero44[16] = {};");

        // PSMTXIdentity
        harness.AppendLine("    { float got[12]; float exp[12]; RefIdentity(exp); WriteMat34(matA, a);");
        EmitCall(harness, specs, "PSMTXIdentity_80199d04", new[] { "matA" });
        harness.AppendLine("      ReadMat34(matA, got); ExpectMat34(got, exp, \"PSMTXIdentity\"); }");

        // PSMTXCopy
        harness.AppendLine("    { float got[12]; WriteMat34(matA, a); WriteMat34(matB, b);");
        EmitCall(harness, specs, "PSMTXCopy_80199d30", new[] { "matA", "matB" });
        harness.AppendLine("      ReadMat34(matB, got); ExpectMat34(got, a, \"PSMTXCopy\"); }");

        // PSMTXConcat
        harness.AppendLine("    { float got[12]; float exp[12]; RefConcat(a, b, exp); WriteMat34(matA, a); WriteMat34(matB, b);");
        EmitCall(harness, specs, "PSMTXConcat_80199d64", new[] { "matA", "matB", "matC" });
        harness.AppendLine("      ReadMat34(matC, got); ExpectMat34(got, exp, \"PSMTXConcat\"); }");

        // PSMTXConcatArray
        harness.AppendLine("    { float got[24]; float exp[24]; WriteMat34(matA, a); WriteMat34(matArr, b); WriteMat34(matArr + 0x30, c);\n      RefConcat(a, b, exp);\n      RefConcat(a, c, exp + 12);\n      for (int i = 0; i < 24; ++i) got[i] = 0.0f;\n      WriteMat34(matC, zero34);\n      WriteMat34(matC + 0x30, zero34);\n      ");
        EmitCall(harness, specs, "PSMTXConcatArray_80199e30", new[] { "matA", "matArr", "matC", "2" });
        harness.AppendLine("      ReadMat34(matC, got); ReadMat34(matC + 0x30, got + 12); ExpectMat34(got, exp, \"PSMTXConcatArray[0]\"); ExpectMat34(got + 12, exp + 12, \"PSMTXConcatArray[1]\"); }");

        // PSMTXInverse
        harness.AppendLine("    { float got[12]; float exp[12]; WriteMat34(matA, a);\n      bool ok = RefInverse(a, exp);\n      assert(ok);\n");
        EmitCall(harness, specs, "PSMTXInverse_80199fc8", new[] { "matA", "matB" }, resultVar: "ret");
        harness.AppendLine("      ReadMat34(matB, got); ExpectMat34(got, exp, \"PSMTXInverse\"); assert(ret == 1); }");

        // PSMTXInvXpose
        harness.AppendLine("    { float got[12]; float exp[12]; WriteMat34(matA, a);\n      bool ok = RefInvXpose(a, exp);\n      assert(ok);\n");
        EmitCall(harness, specs, "PSMTXInvXpose_8019a0c0", new[] { "matA", "matB" }, resultVar: "ret");
        harness.AppendLine("      ReadMat34(matB, got); ExpectMat34(got, exp, \"PSMTXInvXpose\"); assert(ret == 1); }");

        // PSMTXRotRad
        harness.AppendLine("    { float got[12]; float exp[12]; const float rad = 0.7f; RefRotRad(exp, 'y', rad);\n      WriteMat34(matA, a);");
        EmitCall(harness, specs, "PSMTXRotRad_8019a188", new[] { "matA", "'y'", "rad" });
        harness.AppendLine("      ReadMat34(matA, got); ExpectMat34(got, exp, \"PSMTXRotRad\"); }");

        // PSMTXRotTrig (test x,y,z)
        harness.AppendLine("    { float got[12]; float exp[12]; const float rad = 0.4f; const float s = sinf(rad); const float c = cosf(rad);\n");
        harness.AppendLine("      RefRotTrig(exp, 'x', s, c); WriteMat34(matA, a);");
        EmitCall(harness, specs, "PSMTXRotTrig_8019a204", new[] { "matA", "'x'", "s", "c" });
        harness.AppendLine("      ReadMat34(matA, got); ExpectMat34(got, exp, \"PSMTXRotTrigX\");\n      RefRotTrig(exp, 'z', s, c); WriteMat34(matA, a);");
        EmitCall(harness, specs, "PSMTXRotTrig_8019a204", new[] { "matA", "'z'", "s", "c" });
        harness.AppendLine("      ReadMat34(matA, got); ExpectMat34(got, exp, \"PSMTXRotTrigZ\"); }");

        // PSMTXRotAxisRadInternal
        harness.AppendLine("    { float got[12]; float exp[12]; float axis[3] = { 1.0f, 2.0f, 3.0f }; const float rad = 0.9f;\n      const float s = sinf(rad); const float c0 = cosf(rad); RefRotAxisRadInternal(exp, axis, s, c0);\n      WriteVec3(vecA, axis); WriteMat34(matA, a);");
        EmitCall(harness, specs, "PSMTXRotAxisRadInternal_8019a2b4", new[] { "matA", "vecA", "s", "c0" });
        harness.AppendLine("      ReadMat34(matA, got); ExpectMat34(got, exp, \"PSMTXRotAxisRadInternal\"); }");

        // PSMTXRotAxisRad
        harness.AppendLine("    { float got[12]; float exp[12]; float axis[3] = { -0.5f, 1.3f, 0.8f }; const float rad = -0.6f; RefRotAxisRad(exp, axis, rad);\n      WriteVec3(vecA, axis); WriteMat34(matA, a);");
        EmitCall(harness, specs, "PSMTXRotAxisRad_8019a364", new[] { "matA", "vecA", "rad" });
        harness.AppendLine("      ReadMat34(matA, got); ExpectMat34(got, exp, \"PSMTXRotAxisRad\"); }");

        // PSMTXTrans
        harness.AppendLine("    { float got[12]; float exp[12]; RefTrans(exp, 1.5f, -2.0f, 0.75f);\n      WriteMat34(matA, a);");
        EmitCall(harness, specs, "PSMTXTrans_8019a3e0", new[] { "matA", "1.5f", "-2.0f", "0.75f" });
        harness.AppendLine("      ReadMat34(matA, got); ExpectMat34(got, exp, \"PSMTXTrans\"); }");

        // PSMTXTransApply
        harness.AppendLine("    { float got[12]; float exp[12]; RefTransApply(a, exp, 0.25f, -0.5f, 1.2f); WriteMat34(matA, a);");
        EmitCall(harness, specs, "PSMTXTransApply_8019a414", new[] { "matA", "matB", "0.25f", "-0.5f", "1.2f" });
        harness.AppendLine("      ReadMat34(matB, got); ExpectMat34(got, exp, \"PSMTXTransApply\"); }");

        // PSMTXScale
        harness.AppendLine("    { float got[12]; float exp[12]; RefScale(exp, 2.0f, 0.5f, -1.5f); WriteMat34(matA, a);");
        EmitCall(harness, specs, "PSMTXScale_8019a460", new[] { "matA", "2.0f", "0.5f", "-1.5f" });
        harness.AppendLine("      ReadMat34(matA, got); ExpectMat34(got, exp, \"PSMTXScale\"); }");

        // PSMTXScaleApply
        harness.AppendLine("    { float got[12]; float exp[12]; RefScaleApply(a, exp, 1.1f, -0.7f, 0.6f); WriteMat34(matA, a);");
        EmitCall(harness, specs, "PSMTXScaleApply_8019a488", new[] { "matA", "matB", "1.1f", "-0.7f", "0.6f" });
        harness.AppendLine("      ReadMat34(matB, got); ExpectMat34(got, exp, \"PSMTXScaleApply\"); }");

        // PSMTXQuat
        harness.AppendLine("    { float got[12]; float exp[12]; RefQuat(exp, quat);\n      WriteF32(quatA + 0, quat[0]); WriteF32(quatA + 4, quat[1]); WriteF32(quatA + 8, quat[2]); WriteF32(quatA + 12, quat[3]);\n      WriteMat34(matA, a);");
        EmitCall(harness, specs, "PSMTXQuat_8019a4e0", new[] { "matA", "quatA" });
        harness.AppendLine("      ReadMat34(matA, got); ExpectMat34(got, exp, \"PSMTXQuat\"); }");

        // C_MTXLookAt
        harness.AppendLine("    { float got[12]; float exp[12]; float camPos[3] = { 1.0f, 2.0f, 3.0f }; float camUp[3] = { 0.0f, 1.0f, 0.0f }; float target[3] = { 0.0f, 0.0f, 0.0f };\n      RefLookAt(exp, camPos, camUp, target); WriteVec3(vecA, camPos); WriteVec3(vecB, camUp); WriteVec3(vecC, target);\n      WriteMat34(matA, a);");
        EmitCall(harness, specs, "C_MTXLookAt_8019a584", new[] { "matA", "vecA", "vecB", "vecC" });
        harness.AppendLine("      ReadMat34(matA, got); ExpectMat34(got, exp, \"C_MTXLookAt\"); }");

        // C_MTXLightPerspective
        harness.AppendLine("    { float got[12]; float exp[12]; RefLightPerspective(exp, 30.0f, 1.2f, 0.5f, 0.75f, 0.5f, 0.25f); WriteMat34(matA, a);");
        EmitCall(harness, specs, "C_MTXLightPerspective_8019a79c", new[] { "matA", "30.0f", "1.2f", "0.5f", "0.75f", "0.5f", "0.25f" });
        harness.AppendLine("      ReadMat34(matA, got); ExpectMat34(got, exp, \"C_MTXLightPerspective\"); }");

        // C_MTXLightFrustum (with stack arg for transT)
        harness.AppendLine("    { float got[12]; float exp[12]; RefLightFrustum(exp, 1.2f, -0.8f, -1.0f, 1.4f, 0.6f, 0.5f, 0.5f, 0.4f, 0.6f); WriteMat34(matA, a);\n      WriteF32(g_cpu.gpr[1] + 8, 0.6f);");
        EmitCall(harness, specs, "C_MTXLightFrustum_8019a6f8", new[] { "matA", "1.2f", "-0.8f", "-1.0f", "1.4f", "0.6f", "0.5f", "0.5f", "0.4f" });
        harness.AppendLine("      ReadMat34(matA, got); ExpectMat34(got, exp, \"C_MTXLightFrustum\"); }");

        // C_MTXLightOrtho
        harness.AppendLine("    { float got[12]; float exp[12]; RefLightOrtho(exp, 1.0f, -1.0f, -2.0f, 2.0f, 0.5f, 0.7f, 0.2f, 0.3f); WriteMat34(matA, a);");
        EmitCall(harness, specs, "C_MTXLightOrtho_8019a894", new[] { "matA", "1.0f", "-1.0f", "-2.0f", "2.0f", "0.5f", "0.7f", "0.2f", "0.3f" });
        harness.AppendLine("      ReadMat34(matA, got); ExpectMat34(got, exp, \"C_MTXLightOrtho\"); }");

        // PSMTXMultVec
        harness.AppendLine("    { float got[3]; float exp[3]; RefMultVec(a, vec, exp); WriteMat34(matA, a); WriteVec3(vecA, vec);\n");
        EmitCall(harness, specs, "PSMTXMultVec_8019a91c", new[] { "matA", "vecA", "vecB" });
        harness.AppendLine("      ReadVec3(vecB, got); for (int i = 0; i < 3; ++i) { if (!Near(got[i], exp[i])) { std::printf(\"[mtx] PSMTXMultVec mismatch %d\\n\", i); assert(false);} } }");

        // PSMTXMultVecSR
        harness.AppendLine("    { float got[3]; float exp[3]; RefMultVecSR(a, vec, exp); WriteMat34(matA, a); WriteVec3(vecA, vec);\n");
        EmitCall(harness, specs, "PSMTXMultVecSR_8019a970", new[] { "matA", "vecA", "vecB" });
        harness.AppendLine("      ReadVec3(vecB, got); for (int i = 0; i < 3; ++i) { if (!Near(got[i], exp[i])) { std::printf(\"[mtx] PSMTXMultVecSR mismatch %d\\n\", i); assert(false);} } }");

        // C_MTXFrustum
        harness.AppendLine("    { float got[16]; float exp[16]; RefFrustum(exp, 1.0f, -1.0f, -1.5f, 1.5f, 1.0f, 10.0f);\n      WriteMat44(mat44A, zero44);");
        EmitCall(harness, specs, "C_MTXFrustum_8019a9c4", new[] { "mat44A", "1.0f", "-1.0f", "-1.5f", "1.5f", "1.0f", "10.0f" });
        harness.AppendLine("      ReadMat44(mat44A, got); ExpectMat44(got, exp, \"C_MTXFrustum\"); }");

        // C_MTXPerspective
        harness.AppendLine("    { float got[16]; float exp[16]; RefPerspective(exp, 60.0f, 1.3333f, 1.0f, 100.0f);\n      WriteMat44(mat44A, zero44);");
        EmitCall(harness, specs, "C_MTXPerspective_8019aa60", new[] { "mat44A", "60.0f", "1.3333f", "1.0f", "100.0f" });
        harness.AppendLine("      ReadMat44(mat44A, got); ExpectMat44(got, exp, \"C_MTXPerspective\"); }");

        // C_MTXOrtho
        harness.AppendLine("    { float got[16]; float exp[16]; RefOrtho(exp, 1.0f, -1.0f, -2.0f, 2.0f, 0.5f, 20.0f);\n      WriteMat44(mat44A, zero44);");
        EmitCall(harness, specs, "C_MTXOrtho_8019ab4c", new[] { "mat44A", "1.0f", "-1.0f", "-2.0f", "2.0f", "0.5f", "20.0f" });
        harness.AppendLine("      ReadMat44(mat44A, got); ExpectMat44(got, exp, \"C_MTXOrtho\"); }");

        // MTX__PSVECAdd
        harness.AppendLine("    { float aV[3] = { 1.0f, 2.0f, 3.0f }; float bV[3] = { 4.0f, -5.0f, 6.0f }; float got[3]; float exp[3] = { 5.0f, -3.0f, 9.0f };");
        harness.AppendLine("      WriteVec3(vecA, aV); WriteVec3(vecB, bV);");
        EmitCall(harness, specs, "MTX__PSVECAdd_8019abe4", new[] { "vecA", "vecB", "vecC" });
        harness.AppendLine("      ReadVec3(vecC, got); for (int i = 0; i < 3; ++i) { if (!Near(got[i], exp[i])) { std::printf(\"[mtx] PSVECAdd mismatch %d\\n\", i); assert(false); } } }");

        // MTX__PSVECScale
        harness.AppendLine("    { float src[3] = { 1.5f, -2.0f, 3.0f }; float got[3]; float exp[3] = { 3.0f, -4.0f, 6.0f };");
        harness.AppendLine("      WriteVec3(vecA, src);");
        EmitCall(harness, specs, "MTX__PSVECScale_8019ac08", new[] { "vecA", "vecB", "2.0f" });
        harness.AppendLine("      ReadVec3(vecB, got); for (int i = 0; i < 3; ++i) { if (!Near(got[i], exp[i])) { std::printf(\"[mtx] PSVECScale mismatch %d\\n\", i); assert(false); } } }");

        // MTX__PSVECNormalize
        harness.AppendLine("    { float src[3] = { 3.0f, 4.0f, 0.0f }; float got[3]; float exp[3]; VecNormalize(src, exp);");
        harness.AppendLine("      WriteVec3(vecA, src);");
        EmitCall(harness, specs, "MTX__PSVECNormalize_8019ac24", new[] { "vecA", "vecB" });
        harness.AppendLine("      ReadVec3(vecB, got); for (int i = 0; i < 3; ++i) { if (!Near(got[i], exp[i])) { std::printf(\"[mtx] PSVECNormalize mismatch %d\\n\", i); assert(false); } } }");

        // MTX__PSVECMag
        harness.AppendLine("    { float src[3] = { 3.0f, 4.0f, 0.0f }; WriteVec3(vecA, src);");
        EmitCall(harness, specs, "MTX__PSVECMag_8019ac68", new[] { "vecA" });
        harness.AppendLine("      const float got = ReadFprFloat(g_cpu, 1); const float exp = VecMag(src); if (!Near(got, exp)) { std::printf(\"[mtx] PSVECMag mismatch got %.6f exp %.6f\\n\", got, exp); assert(false); } }");

        // MTX__PSVECDotProduct
        harness.AppendLine("    { float aV[3] = { 1.0f, 2.0f, 3.0f }; float bV[3] = { 4.0f, -5.0f, 6.0f }; WriteVec3(vecA, aV); WriteVec3(vecB, bV);");
        EmitCall(harness, specs, "MTX__PSVECDotProduct_8019acac", new[] { "vecA", "vecB" });
        harness.AppendLine("      const float got = ReadFprPs0(g_cpu, 1); const float exp = VecDot(aV, bV); if (!Near(got, exp)) { std::printf(\"[mtx] PSVECDotProduct mismatch got %.6f exp %.6f\\n\", got, exp); assert(false); } }");

        // MTX__PSVECCrossProduct
        harness.AppendLine("    { float aV[3] = { 1.0f, 0.0f, 0.0f }; float bV[3] = { 0.0f, 1.0f, 0.0f }; float got[3]; float exp[3]; VecCross(aV, bV, exp);");
        harness.AppendLine("      WriteVec3(vecA, aV); WriteVec3(vecB, bV);");
        EmitCall(harness, specs, "MTX__PSVECCrossProduct_8019accc", new[] { "vecA", "vecB", "vecC" });
        harness.AppendLine("      ReadVec3(vecC, got); for (int i = 0; i < 3; ++i) { if (!Near(got[i], exp[i])) { std::printf(\"[mtx] PSVECCrossProduct mismatch %d\\n\", i); assert(false); } } }");

        // MTX__C_VECHalfAngle
        harness.AppendLine("    { float aV[3] = { 1.0f, 0.0f, 0.0f }; float bV[3] = { 0.0f, 1.0f, 0.0f }; float got[3]; float exp[3]; VecHalfAngle(aV, bV, exp);");
        harness.AppendLine("      WriteVec3(vecA, aV); WriteVec3(vecB, bV);");
        EmitCall(harness, specs, "MTX__C_VECHalfAngle_8019ad08", new[] { "vecA", "vecB", "vecC" });
        harness.AppendLine("      ReadVec3(vecC, got); for (int i = 0; i < 3; ++i) { if (!Near(got[i], exp[i])) { std::printf(\"[mtx] C_VECHalfAngle mismatch %d\\n\", i); assert(false); } } }");

        // MTX__PSVECSquareDistance
        harness.AppendLine("    { float aV[3] = { 1.0f, 2.0f, 3.0f }; float bV[3] = { 4.0f, -2.0f, 6.0f }; WriteVec3(vecA, aV); WriteVec3(vecB, bV);");
        EmitCall(harness, specs, "MTX__PSVECSquareDistance_8019ade0", new[] { "vecA", "vecB" });
        harness.AppendLine("      const float got = ReadFprPs0(g_cpu, 1); const float exp = VecSquareDistance(aV, bV); if (!Near(got, exp)) { std::printf(\"[mtx] PSVECSquareDistance mismatch got %.6f exp %.6f\\n\", got, exp); assert(false); } }");

        // MTX__PSQUATMultiply
        harness.AppendLine("    { float aQ[4] = { 0.1f, 0.2f, 0.3f, 0.9f }; float bQ[4] = { -0.3f, 0.5f, 0.1f, 0.8f }; float got[4]; float exp[4]; QuatMul(aQ, bQ, exp);");
        harness.AppendLine("      WriteQuat(quatA, aQ); WriteQuat(quatB, bQ);");
        EmitCall(harness, specs, "MTX__PSQUATMultiply_8019ae08", new[] { "quatA", "quatB", "quatC" });
        harness.AppendLine("      ReadQuat(quatC, got); for (int i = 0; i < 4; ++i) { if (!Near(got[i], exp[i])) { std::printf(\"[mtx] PSQUATMultiply mismatch %d\\n\", i); assert(false); } } }");

        // MTX__PSQUATScale
        harness.AppendLine("    { float src[4] = { 1.0f, -2.0f, 3.0f, -4.0f }; float got[4]; float exp[4] = { 0.5f, -1.0f, 1.5f, -2.0f };");
        harness.AppendLine("      WriteQuat(quatA, src);");
        EmitCall(harness, specs, "MTX__PSQUATScale_8019ae64", new[] { "quatA", "quatB", "0.5f" });
        harness.AppendLine("      ReadQuat(quatB, got); for (int i = 0; i < 4; ++i) { if (!Near(got[i], exp[i])) { std::printf(\"[mtx] PSQUATScale mismatch %d\\n\", i); assert(false); } } }");

        // MTX__PSQUATDotProduct
        harness.AppendLine("    { float aQ[4] = { 1.0f, 2.0f, 3.0f, 4.0f }; float bQ[4] = { -2.0f, 0.5f, 1.0f, -1.5f }; WriteQuat(quatA, aQ); WriteQuat(quatB, bQ);");
        EmitCall(harness, specs, "MTX__PSQUATDotProduct_8019ae80", new[] { "quatA", "quatB" });
        harness.AppendLine("      const float got = ReadFprPs0(g_cpu, 1); const float exp = QuatDot(aQ, bQ); if (!Near(got, exp)) { std::printf(\"[mtx] PSQUATDotProduct mismatch got %.6f exp %.6f\\n\", got, exp); assert(false); } }");

        // MTX__PSQUATNormalize
        harness.AppendLine("    { float src[4] = { 0.0f, 0.0f, 0.0f, 2.0f }; float got[4]; float exp[4]; QuatNormalize(src, exp);");
        harness.AppendLine("      WriteQuat(quatA, src);");
        EmitCall(harness, specs, "MTX__PSQUATNormalize_8019aea0", new[] { "quatA", "quatB" });
        harness.AppendLine("      ReadQuat(quatB, got); for (int i = 0; i < 4; ++i) { if (!Near(got[i], exp[i])) { std::printf(\"[mtx] PSQUATNormalize mismatch %d\\n\", i); assert(false); } } }");

        // MTX__PSQUATInverse
        harness.AppendLine("    { float src[4] = { 0.2f, -0.3f, 0.4f, 0.5f }; float got[4]; float exp[4]; QuatInverse(src, exp);");
        harness.AppendLine("      WriteQuat(quatA, src);");
        EmitCall(harness, specs, "MTX__PSQUATInverse_8019aef4", new[] { "quatA", "quatB" });
        harness.AppendLine("      ReadQuat(quatB, got); for (int i = 0; i < 4; ++i) { if (!Near(got[i], exp[i])) { std::printf(\"[mtx] PSQUATInverse mismatch %d got %.6f exp %.6f\\n\", i, got[i], exp[i]); assert(false); } } }");

        // MTX__C_QUATMtx
        harness.AppendLine("    { float ident[12] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f }; float got[4]; float exp[4] = { 0.0f, 0.0f, 0.0f, 1.0f };");
        harness.AppendLine("      WriteMat34(matA, ident);");
        EmitCall(harness, specs, "MTX__C_QUATMtx_8019af48", new[] { "quatA", "matA" });
        harness.AppendLine("      ReadQuat(quatA, got); for (int i = 0; i < 4; ++i) { if (!Near(got[i], exp[i])) { std::printf(\"[mtx] C_QUATMtx mismatch %d\\n\", i); assert(false); } } }");

        // MTX__C_QUATLerp
        harness.AppendLine("    { float aQ[4] = { 0.0f, 0.5f, 0.0f, 0.5f }; float bQ[4] = { 1.0f, -0.5f, 0.5f, 0.0f }; float got[4]; float exp[4]; QuatLerp(aQ, bQ, 0.25f, exp);");
        harness.AppendLine("      WriteQuat(quatA, aQ); WriteQuat(quatB, bQ);");
        EmitCall(harness, specs, "MTX__C_QUATLerp_8019b114", new[] { "quatA", "quatB", "quatC", "0.25f" });
        harness.AppendLine("      ReadQuat(quatC, got); for (int i = 0; i < 4; ++i) { if (!Near(got[i], exp[i])) { std::printf(\"[mtx] C_QUATLerp mismatch %d\\n\", i); assert(false); } } }");

        // MTX__C_QUATSlerp
        harness.AppendLine("    { float aQ[4] = { 0.0f, 0.0f, 0.0f, 1.0f }; float bQ[4] = { 0.1f, 0.2f, 0.3f, 0.9f }; float got[4]; float exp[4]; QuatSlerp(aQ, bQ, 0.5f, exp);");
        harness.AppendLine("      WriteQuat(quatA, aQ); WriteQuat(quatB, bQ);");
        EmitCall(harness, specs, "MTX__C_QUATSlerp_8019b178", new[] { "quatA", "quatB", "quatC", "0.5f" });
        harness.AppendLine("      ReadQuat(quatC, got); for (int i = 0; i < 4; ++i) { if (!Near(got[i], exp[i])) { std::printf(\"[mtx] C_QUATSlerp mismatch %d\\n\", i); assert(false); } } }");

        harness.AppendLine("    return 0;");
        harness.AppendLine("}");

        var harnessPath = Path.Combine(tempRoot, "harness_mtx.cpp");
        File.WriteAllText(harnessPath, harness.ToString());

        var root = RepositoryRoot;
        var runnerBasePath = Path.Combine(tempRoot, "runner_mtx");
        var compileArgs = TranslatorCppTestHarness.BuildCompileArguments(root, tempRoot, generatedFiles, harnessPath, runnerBasePath);
        compileArgs += " -ldbghelp";

        Console.WriteLine("[compile] clang++ (mtx)");
        var (exitCode, compileOutput) = RunProcess("clang++", compileArgs.ToString());
        Assert.True(exitCode == 0, $"clang++ failed: {compileOutput}");

        var runnerPath = runnerBasePath;
        if (OperatingSystem.IsWindows() && !File.Exists(runnerPath))
        {
            runnerPath += ".exe";
        }

        Console.WriteLine($"[run] {runnerPath}");
        var (runExitCode, runOutput) = RunProcess(runnerPath, string.Empty, TimeSpan.FromSeconds(60));
        Console.Write(runOutput);
        Assert.True(runExitCode == 0, $"Runner failed (exit code {runExitCode}). Output:\n{runOutput}");
    }

    /// <summary>
    /// Leaf-inlining admission checked against real MTX bodies. Stackless primitives stay eligible,
    /// while <c>PSMTXConcat</c> must not: its 64-byte r1 frame is unsafe to splice into a caller's stack analysis.
    /// </summary>
    [Fact]
    public void LeafInliningRejectsStackFramedMatrixPrimitives()
    {
        var translator = BuildTranslator();
        var options = TranslationOptions.Default with
        {
            AllowUnsupportedInstructions = true
        };

        LeafInlineCandidate? Classify(uint address, LeafInliningPolicy policy, out LeafInlineRejection rejection)
        {
            var translation = translator.Translate(address, options);
            return LeafFunctionInliner.TryCreateCandidate(
                address, translation.LinearIr, translation.Instructions, policy, out rejection);
        }

        var shipped = new LeafInliningPolicy(
            MaxCalleeGuestInstructions: TranslationOptions.Default.LeafInliningMaxCalleeInstructions);
        var oldCap = new LeafInliningPolicy(MaxCalleeGuestInstructions: 32);

        // PSMTXCopy was admitted before the cap moved and must be admitted in
        // exactly the same form afterwards - a flat statement list, not a
        // spliced region - or every caller that already inlines it changes.
        var copy = Classify(0x80199D30u, shipped, out var copyRejection);
        Assert.Equal(LeafInlineRejection.None, copyRejection);
        Assert.NotNull(copy);
        Assert.Null(copy!.Blocks);
        Assert.Equal(13, copy.GuestInstructionCount);
        Classify(0x80199D30u, oldCap, out var copyAtOldCap);
        Assert.Equal(LeafInlineRejection.None, copyAtOldCap);

        var concat = Classify(0x80199D64u, shipped, out var concatRejection);
        Assert.Equal(LeafInlineRejection.TouchesStackPointer, concatRejection);
        Assert.Null(concat);
        Classify(0x80199D64u, oldCap, out var concatAtOldCap);
        Assert.Equal(LeafInlineRejection.TooLarge, concatAtOldCap);
    }

    /// <summary>
    /// <c>nw4r::ut::List_GetNext</c> is the acyclic multi-block case: eight guest instructions across
    /// four blocks, a compare/branch and two return arms that must converge on one caller continuation.
    /// </summary>
    [Fact]
    public void LeafInliningAdmitsAnAcyclicMultiBlockListWalker()
    {
        var translator = BuildTranslator();
        var options = TranslationOptions.Default with
        {
            AllowUnsupportedInstructions = true
        };
        var translation = translator.Translate(0x800AF180u, options);

        var candidate = LeafFunctionInliner.TryCreateCandidate(
            0x800AF180u,
            translation.LinearIr,
            translation.Instructions,
            new LeafInliningPolicy(),
            out var rejection);

        Assert.Equal(LeafInlineRejection.None, rejection);
        Assert.NotNull(candidate);
        Assert.Equal(8, candidate!.GuestInstructionCount);
        Assert.NotNull(candidate.Blocks);
        Assert.Equal(4, candidate.Blocks!.Count);
        Assert.Equal(2, candidate.Blocks.Count(block => block.Instructions[^1] is IrReturn));
        // Every block leaves through an explicit terminator, so the spliced copy
        // does not depend on where the caller places these blocks.
        Assert.All(
            candidate.Blocks,
            block => Assert.True(block.Instructions[^1] is IrReturn or IrJump or IrBranch));

        // Turning the shape off puts the callee straight back outside the
        // admissible set.
        LeafFunctionInliner.TryCreateCandidate(
            0x800AF180u,
            translation.LinearIr,
            translation.Instructions,
            new LeafInliningPolicy(AllowAcyclicMultiBlockCallees: false),
            out var withoutMultiBlock);
        Assert.Equal(LeafInlineRejection.ControlFlow, withoutMultiBlock);
    }

    /// <summary>
    /// <c>EGG::Math::Sqrt</c> looked like a multi-block leaf from a profile, but
    /// it saves the link register and calls out, so no relaxation of the block
    /// shape can admit it. Pinned so a future rule change has to notice.
    /// </summary>
    [Fact]
    public void EggMathSqrtIsNotALeafAndStaysRefused()
    {
        var translator = BuildTranslator();
        var options = TranslationOptions.Default with
        {
            AllowUnsupportedInstructions = true
        };
        var translation = translator.Translate(0x8022F80Cu, options);

        LeafFunctionInliner.TryCreateCandidate(
            0x8022F80Cu,
            translation.LinearIr,
            translation.Instructions,
            new LeafInliningPolicy(),
            out var rejection);

        Assert.Equal(LeafInlineRejection.TouchesLinkRegister, rejection);
    }

    private static MtxFuncSpec Spec(uint address, string name, ReturnKind ret, params ParamKind[] paramKinds)
        => new(address, name, ret, paramKinds);

    private static void AssertReturnAbiMatches(MtxFuncSpec spec, FunctionAbiClassification classification)
    {
        if (spec.Return == ReturnKind.Void)
        {
            var isFloatReturn = classification.ReturnRepresentation is ValueRepresentation prim && prim.IsFloat;
            Assert.False(isFloatReturn, $"{spec.Name} expected non-float return, got {classification.ReturnRepresentation}.");
        }
        else if (spec.Return == ReturnKind.Float)
        {
            Assert.True(classification.ReturnRepresentation is ValueRepresentation prim && prim.IsFloat,
                $"{spec.Name} expected float return, got {classification.ReturnRepresentation}.");
        }
        else
        {
            Assert.True(classification.ReturnRepresentation is ValueRepresentation prim && !prim.IsFloat,
                $"{spec.Name} expected integer return, got {classification.ReturnRepresentation}.");
        }
    }

    private static IReadOnlyList<FunctionTranslationResult> TranslateWithDependencies(
        FunctionTranslator translator,
        IEnumerable<(uint Address, string Name)> seeds,
        string outputDirectory,
        List<string> generatedFiles)
    {
        const int MaxTranslations = 256;
        var translations = new List<FunctionTranslationResult>();
        var scheduled = new HashSet<uint>();
        var translated = new HashSet<uint>();
        var queue = new Queue<(uint Address, string Name)>();

        foreach (var spec in seeds)
        {
            if (scheduled.Add(spec.Address))
            {
                queue.Enqueue(spec);
            }
        }

        while (queue.Count > 0)
        {
            if (scheduled.Count > MaxTranslations)
            {
                throw new InvalidOperationException($"Auto-translation queue exceeded {MaxTranslations} functions. Check for runaway dependency discovery.");
            }

            var spec = queue.Dequeue();
            if (translated.Contains(spec.Address))
            {
                continue;
            }

            var preferredName = string.IsNullOrWhiteSpace(spec.Name) ? $"func_{spec.Address:X8}" : spec.Name;
            Console.WriteLine($"[translate] {preferredName} 0x{spec.Address:X8}");
            var result = translator.Translate(spec.Address, new TranslationOptions(preferredName));

            var classification = result.AbiClassification;
            Console.WriteLine($"  [abi] {classification.Name} -> {classification.ReturnRepresentation}");

            var path = Path.Combine(outputDirectory, $"{result.Name}.cpp");
            File.WriteAllText(path, result.CxxCode);
            generatedFiles.Add(path);
            translations.Add(result);
            translated.Add(result.EntryPoint);

            foreach (var dependency in DiscoverCallTargets(result))
            {
                if (translated.Contains(dependency))
                {
                    continue;
                }

                if (scheduled.Add(dependency))
                {
                    queue.Enqueue((dependency, $"func_{dependency:X8}"));
                }
            }
        }

        return translations;
    }

    private static IEnumerable<uint> DiscoverCallTargets(FunctionTranslationResult translation)
    {
        var discovered = new HashSet<uint>();
        foreach (var block in translation.LinearIr.Blocks)
        {
            foreach (var instruction in block.Instructions)
            {
                if (instruction is not IrCall call)
                {
                    continue;
                }

                if (string.IsNullOrWhiteSpace(call.Target) || !call.Target.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                if (!uint.TryParse(call.Target.Substring(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var address))
                {
                    continue;
                }

                if (address == translation.EntryPoint)
                {
                    continue;
                }

                if (discovered.Add(address))
                {
                    yield return address;
                }
            }
        }
    }

    private static string PrototypeFor(FunctionAbiClassification signature)
    {
        return $"extern \"C\" void {signature.Name}(CpuContext* ctx);";
    }

    private static void EmitCall(StringBuilder sb, IReadOnlyList<MtxFuncSpec> specs, string name, string[] args, string? resultVar = null)
    {
        var spec = specs.Single(s => s.Name == name);
        var gprIndex = 3;
        var fprIndex = 1;

        for (int i = 0; i < spec.Params.Count; i++)
        {
            var arg = args.Length > i ? args[i] : "0";
            if (spec.Params[i] == ParamKind.Fpr)
            {
                sb.AppendLine($"      g_cpu.fpr[{fprIndex}].d = {arg};");
                fprIndex++;
            }
            else
            {
                sb.AppendLine($"      g_cpu.gpr[{gprIndex}] = ToGpr({arg});");
                gprIndex++;
            }
        }

        sb.AppendLine($"      {name}(&g_cpu);");
        if (!string.IsNullOrWhiteSpace(resultVar))
        {
            sb.AppendLine($"      auto {resultVar} = g_cpu.gpr[3];");
        }
    }
}
