using System.Text.Json;
using static Translator.Core.Mods.Mkwii.RetroWfcJson;

namespace Translator.Core.Mods.Mkwii;

public sealed record RetroWfcStaticBytePatchPlan(
    int RecordIndex,
    string Intent,
    uint Address,
    int Size,
    string SectionName,
    bool SectionExecutable,
    uint? ContainingFunctionStart,
    uint? ContainingFunctionEnd,
    string SourceSha256,
    string BytesHex,
    string LoweringKind);

public sealed record RetroWfcExecutableHookPlan(
    int RecordIndex,
    string Intent,
    string TypeName,
    uint Address,
    int InstructionCount,
    uint ContainingFunctionStart,
    uint ContainingFunctionEnd,
    string SectionName,
    IReadOnlyList<string> SemanticActionIds,
    string LoweringKind,
    uint? TargetAddress,
    string? TargetActionId,
    string? TargetKind,
    string? TargetSymbol,
    uint ContinuationAddress);

public sealed record RetroWfcStaticPointerPlan(
    int RecordIndex,
    uint Address,
    string SectionName,
    bool SectionExecutable,
    uint? ContainingFunctionStart,
    uint? ContainingFunctionEnd,
    IReadOnlyList<string> SemanticActionIds,
    string LoweringKind,
    uint? TargetAddress,
    string? TargetActionId,
    string? TargetKind,
    string? TargetSymbol);

public sealed record RetroWfcLoweringIssue(
    string Severity,
    string Code,
    string Message,
    string? Path);

public sealed record RetroWfcContractLoweringPlan(
    string? Source,
    int PatchRecordCount,
    IReadOnlyList<RetroWfcStaticBytePatchPlan> StaticBytePatches,
    IReadOnlyList<RetroWfcExecutableHookPlan> ExecutableHooks,
    IReadOnlyList<RetroWfcStaticPointerPlan> StaticPointers,
    IReadOnlyList<RetroWfcLoweringIssue> Issues)
{
    public bool IsPlannable => Issues.All(i => !string.Equals(i.Severity, "error", StringComparison.OrdinalIgnoreCase));

    public string BuildTextReport()
    {
        using var writer = new StringWriter();
        writer.WriteLine("Retro WFC recomp contract lowering plan");
        if (!string.IsNullOrWhiteSpace(Source))
        {
            writer.WriteLine($"  source: {Source}");
        }
        writer.WriteLine($"  patch records: {PatchRecordCount:N0}");
        writer.WriteLine($"  static byte patches: {StaticBytePatches.Count:N0}");
        writer.WriteLine($"  executable hooks: {ExecutableHooks.Count:N0}");
        writer.WriteLine($"  static pointer writes: {StaticPointers.Count:N0}");
        writer.WriteLine($"  plannable: {IsPlannable}");
        writer.WriteLine();

        writer.WriteLine("Static byte patches:");
        foreach (var patch in StaticBytePatches.OrderBy(p => p.Address))
        {
            var function = patch.ContainingFunctionStart.HasValue
                ? $" function=0x{patch.ContainingFunctionStart.Value:X8}-0x{patch.ContainingFunctionEnd!.Value:X8}"
                : "";
            writer.WriteLine($"  #{patch.RecordIndex} 0x{patch.Address:X8}+0x{patch.Size:X} {patch.LoweringKind} section={patch.SectionName}{function}");
        }
        if (StaticBytePatches.Count == 0)
        {
            writer.WriteLine("  none");
        }
        writer.WriteLine();

        writer.WriteLine("Executable hooks:");
        foreach (var hook in ExecutableHooks.OrderBy(h => h.Address))
        {
            writer.WriteLine($"  #{hook.RecordIndex} 0x{hook.Address:X8} {hook.TypeName}/{hook.Intent} instructions={hook.InstructionCount} function=0x{hook.ContainingFunctionStart:X8}-0x{hook.ContainingFunctionEnd:X8}");
            writer.WriteLine($"    actions: {string.Join(", ", hook.SemanticActionIds)}");
            writer.WriteLine(hook.TargetAddress.HasValue
                ? $"    target: 0x{hook.TargetAddress.Value:X8} ({hook.TargetKind}, {hook.TargetActionId}) continuation=0x{hook.ContinuationAddress:X8}"
                : $"    target: unresolved continuation=0x{hook.ContinuationAddress:X8}");
        }
        if (ExecutableHooks.Count == 0)
        {
            writer.WriteLine("  none");
        }
        writer.WriteLine();

        writer.WriteLine("Static pointer writes:");
        foreach (var pointer in StaticPointers.OrderBy(p => p.Address))
        {
            var function = pointer.ContainingFunctionStart.HasValue
                ? $" function=0x{pointer.ContainingFunctionStart.Value:X8}-0x{pointer.ContainingFunctionEnd!.Value:X8}"
                : "";
            writer.WriteLine($"  #{pointer.RecordIndex} 0x{pointer.Address:X8} {pointer.LoweringKind} section={pointer.SectionName}{function}");
            writer.WriteLine($"    actions: {string.Join(", ", pointer.SemanticActionIds)}");
            writer.WriteLine(pointer.TargetAddress.HasValue
                ? $"    target: 0x{pointer.TargetAddress.Value:X8} ({pointer.TargetKind}, {pointer.TargetActionId})"
                : "    target: unresolved");
        }
        if (StaticPointers.Count == 0)
        {
            writer.WriteLine("  none");
        }
        writer.WriteLine();

        var errors = Issues.Where(i => string.Equals(i.Severity, "error", StringComparison.OrdinalIgnoreCase)).ToList();
        var warnings = Issues.Where(i => string.Equals(i.Severity, "warning", StringComparison.OrdinalIgnoreCase)).ToList();
        writer.WriteLine($"Errors: {errors.Count:N0}");
        foreach (var issue in errors)
        {
            writer.WriteLine($"  [{issue.Code}] {issue.Message}{FormatIssuePath(issue.Path)}");
        }
        writer.WriteLine($"Warnings: {warnings.Count:N0}");
        foreach (var issue in warnings)
        {
            writer.WriteLine($"  [{issue.Code}] {issue.Message}{FormatIssuePath(issue.Path)}");
        }

        return writer.ToString();
    }

    private static string FormatIssuePath(string? path) =>
        string.IsNullOrWhiteSpace(path) ? "" : $" ({path})";
}

public static class RetroWfcContractLoweringPlanner
{
    public static RetroWfcContractLoweringPlan Build(
        string json,
        BaseManifest baseManifest,
        string? source = null,
        IReadOnlyDictionary<string, RetroWfcSemanticActionTarget>? semanticActionTargets = null)
    {
        using var document = JsonDocument.Parse(json);
        var root = document.RootElement;
        var issues = new List<RetroWfcLoweringIssue>();
        var staticBytePatches = new List<RetroWfcStaticBytePatchPlan>();
        var executableHooks = new List<RetroWfcExecutableHookPlan>();
        var staticPointers = new List<RetroWfcStaticPointerPlan>();
        var functionIndex = new BaseFunctionIndex(baseManifest.Functions);

        if (!root.TryGetProperty("patchRecords", out var records) || records.ValueKind != JsonValueKind.Array)
        {
            issues.Add(Error("MissingPatchRecords", "Contract is missing patchRecords.", "$.patchRecords"));
            return new RetroWfcContractLoweringPlan(source, 0, staticBytePatches, executableHooks, staticPointers, issues);
        }

        var ordinal = 0;
        foreach (var record in records.EnumerateArray())
        {
            var path = $"$.patchRecords[{ordinal}]";
            var recordIndex = GetInt(record, "index") ?? ordinal;
            var intent = GetString(record, "intent") ?? "";
            switch (intent)
            {
                case "staticBytes":
                    TryPlanStaticBytes(record, recordIndex, path, baseManifest, functionIndex, staticBytePatches, issues);
                    break;
                case "executableHook":
                case "executableHookWithContinuation":
                case "executableCall":
                case "executableCtrHook":
                case "executableCtrCall":
                    TryPlanExecutableHook(record, recordIndex, path, baseManifest, functionIndex, semanticActionTargets, executableHooks, issues);
                    break;
                case "staticPointer":
                    TryPlanStaticPointer(record, recordIndex, path, baseManifest, functionIndex, semanticActionTargets, staticPointers, issues);
                    break;
                default:
                    issues.Add(Error("UnsupportedIntent", $"Unsupported Retro WFC patch intent '{intent}'.", path + ".intent"));
                    break;
            }
            ordinal++;
        }

        return new RetroWfcContractLoweringPlan(source, records.GetArrayLength(), staticBytePatches, executableHooks, staticPointers, issues);
    }

    private static void TryPlanStaticBytes(
        JsonElement record,
        int recordIndex,
        string path,
        BaseManifest baseManifest,
        BaseFunctionIndex functionIndex,
        List<RetroWfcStaticBytePatchPlan> result,
        List<RetroWfcLoweringIssue> issues)
    {
        if (!TryReadAddress(record, "address", out var address))
        {
            issues.Add(Error("MissingPatchAddress", "Static byte patch is missing a valid address.", path + ".address"));
            return;
        }
        var section = FindSection(baseManifest, address);
        if (section is null)
        {
            issues.Add(Error("PatchAddressOutsideBase", $"Static byte patch address 0x{address:X8} is outside known base sections.", path + ".address"));
            return;
        }

        var sourceBytes = GetObject(record, "sourceBytes");
        if (sourceBytes is null || GetString(sourceBytes.Value, "kind") != "payloadSlice")
        {
            issues.Add(Error("StaticBytesNotResolved", "Static byte patch must contain sourceBytes kind payloadSlice.", path + ".sourceBytes"));
            return;
        }
        var hex = GetString(sourceBytes.Value, "hex");
        var sha = GetString(sourceBytes.Value, "sha256");
        if (string.IsNullOrWhiteSpace(hex) || !IsEvenHex(hex))
        {
            issues.Add(Error("InvalidStaticBytesHex", "Static byte patch sourceBytes.hex must be even-length hex.", path + ".sourceBytes.hex"));
            return;
        }
        var size = hex.Length / 2;
        if ((ulong)address + (ulong)size > section.GuestEnd)
        {
            issues.Add(Error("StaticBytesCrossSectionBoundary", $"Static byte patch 0x{address:X8}+0x{size:X} crosses section '{section.Name}'.", path + ".address"));
            return;
        }
        if (GetObject(record, "target") is not { } target ||
            GetString(target, "resolvedBy") != "sourceBytes" ||
            GetBool(target, "resolutionRequired") != false)
        {
            issues.Add(Error("StaticBytesTargetNotResolved", "Static byte patch target must be resolved by sourceBytes.", path + ".target"));
            return;
        }

        BaseFunctionRangeMetadata? function = null;
        if (section.Executable)
        {
            function = functionIndex.FindContaining(address);
            if (function is null)
            {
                issues.Add(Error("StaticExecutablePatchMissingFunction", $"Executable static byte patch 0x{address:X8} has no containing function.", path + ".address"));
                return;
            }
        }

        result.Add(new RetroWfcStaticBytePatchPlan(
            recordIndex,
            "staticBytes",
            address,
            size,
            section.Name,
            section.Executable,
            function?.Start,
            function?.End,
            sha ?? "",
            hex,
            section.Executable ? "overlayBytePatch" : "dataBytePatch"));
    }

    private static void TryPlanExecutableHook(
        JsonElement record,
        int recordIndex,
        string path,
        BaseManifest baseManifest,
        BaseFunctionIndex functionIndex,
        IReadOnlyDictionary<string, RetroWfcSemanticActionTarget>? semanticActionTargets,
        List<RetroWfcExecutableHookPlan> result,
        List<RetroWfcLoweringIssue> issues)
    {
        if (!TryReadAddress(record, "address", out var address))
        {
            issues.Add(Error("MissingHookAddress", "Executable hook is missing a valid address.", path + ".address"));
            return;
        }
        var section = FindSection(baseManifest, address);
        if (section is null || !section.Executable)
        {
            issues.Add(Error("HookAddressNotExecutable", $"Executable hook address 0x{address:X8} is not in an executable base section.", path + ".address"));
            return;
        }
        var function = functionIndex.FindContaining(address);
        if (function is null)
        {
            issues.Add(Error("HookMissingFunction", $"Executable hook address 0x{address:X8} has no containing function.", path + ".address"));
            return;
        }

        var actionIds = ReadStringArray(record, "semanticActionIds");
        if (actionIds.Count == 0)
        {
            issues.Add(Error("HookMissingSemanticAction", "Executable hook has no semantic action id.", path + ".semanticActionIds"));
            return;
        }

        var typeName = GetString(record, "typeName") ?? "";
        var instructionCount = GetInt(record, "instructionCount") ?? (typeName is "branchCtr" or "branchCtrLink" ? 4 : 1);
        if (instructionCount <= 0)
        {
            issues.Add(Error("InvalidHookInstructionCount", "Executable hook instruction count must be positive.", path + ".instructionCount"));
            return;
        }
        var target = ResolveActionTarget(actionIds, semanticActionTargets, issues, path + ".semanticActionIds");
        var continuationAddress = checked(address + (uint)(instructionCount * 4));

        result.Add(new RetroWfcExecutableHookPlan(
            recordIndex,
            GetString(record, "intent") ?? "",
            typeName,
            address,
            instructionCount,
            function.Start,
            function.End,
            section.Name,
            actionIds,
            target is null ? "overlayHookPatchPendingAction" : "overlayHookPatch",
            target?.GuestAddress,
            target?.ActionId,
            target?.Kind,
            target?.Symbol,
            continuationAddress));
    }

    private static void TryPlanStaticPointer(
        JsonElement record,
        int recordIndex,
        string path,
        BaseManifest baseManifest,
        BaseFunctionIndex functionIndex,
        IReadOnlyDictionary<string, RetroWfcSemanticActionTarget>? semanticActionTargets,
        List<RetroWfcStaticPointerPlan> result,
        List<RetroWfcLoweringIssue> issues)
    {
        if (!TryReadAddress(record, "address", out var address))
        {
            issues.Add(Error("MissingStaticPointerAddress", "Static pointer write is missing a valid address.", path + ".address"));
            return;
        }
        var section = FindSection(baseManifest, address);
        if (section is null)
        {
            issues.Add(Error("StaticPointerOutsideBase", $"Static pointer address 0x{address:X8} is outside known base sections.", path + ".address"));
            return;
        }

        var actionIds = ReadStringArray(record, "semanticActionIds");
        if (actionIds.Count == 0)
        {
            issues.Add(Error("StaticPointerMissingSemanticAction", "Static pointer write has no semantic action id.", path + ".semanticActionIds"));
            return;
        }

        BaseFunctionRangeMetadata? function = null;
        if (section.Executable)
        {
            function = functionIndex.FindContaining(address);
            if (function is null)
            {
                issues.Add(Error("StaticPointerExecutableMissingFunction", $"Executable static pointer address 0x{address:X8} has no containing function.", path + ".address"));
                return;
            }
        }
        var target = ResolveActionTarget(actionIds, semanticActionTargets, issues, path + ".semanticActionIds");

        result.Add(new RetroWfcStaticPointerPlan(
            recordIndex,
            address,
            section.Name,
            section.Executable,
            function?.Start,
            function?.End,
            actionIds,
            target is null
                ? (section.Executable ? "overlayPointerPatchPendingAction" : "dataPointerPatchPendingAction")
                : (section.Executable ? "overlayPointerPatch" : "dataPointerPatch"),
            target?.GuestAddress,
            target?.ActionId,
            target?.Kind,
            target?.Symbol));
    }

    private static RetroWfcSemanticActionTarget? ResolveActionTarget(
        IReadOnlyList<string> actionIds,
        IReadOnlyDictionary<string, RetroWfcSemanticActionTarget>? semanticActionTargets,
        List<RetroWfcLoweringIssue> issues,
        string path)
    {
        if (semanticActionTargets is null || semanticActionTargets.Count == 0)
        {
            return null;
        }

        var matches = actionIds
            .Select(id => semanticActionTargets.TryGetValue(id, out var target) ? target : null)
            .Where(target => target is not null)
            .Cast<RetroWfcSemanticActionTarget>()
            .ToList();
        if (matches.Count == 0)
        {
            return null;
        }
        if (matches.Count != actionIds.Count)
        {
            issues.Add(Error("PartialSemanticResolution", "Only some semantic action IDs for this record have resolutions.", path));
            return null;
        }

        var first = matches[0];
        if (matches.Any(m => m.GuestAddress != first.GuestAddress || !string.Equals(m.Kind, first.Kind, StringComparison.Ordinal)))
        {
            issues.Add(Error("ConflictingSemanticResolution", "Semantic action IDs for this record resolve to conflicting targets.", path));
            return null;
        }

        return first;
    }

    private static BaseSectionMetadata? FindSection(BaseManifest manifest, uint address) =>
        manifest.Sections.FirstOrDefault(section => address >= section.GuestStart && address < section.GuestEnd);

    private static bool IsEvenHex(string text) =>
        text.Length % 2 == 0 && text.All(c => char.IsAsciiHexDigit(c));

    private static RetroWfcLoweringIssue Error(string code, string message, string? path) =>
        new("error", code, message, path);
}
