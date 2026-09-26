using System.Text.Json;
using Translator.Core.Parsing.Kamek;

namespace Translator.Core.Mods;

public enum KamekPatchClassificationKind
{
    ModuleCommand,
    BaseExecutablePatch,
    BaseDataPatch,
    DroppedReservedRegion,
    Unsupported
}

public sealed record KamekPatchClassification(
    KamekPatchClassificationKind Kind,
    KamekCommandId CommandId,
    uint CommandAddress,
    bool CommandAddressIsAbsolute,
    uint? ContainingFunctionStart,
    uint? ContainingFunctionEnd,
    string? SectionName,
    string Reason,
    IReadOnlyList<uint> Arguments);

public sealed class KamekPatchPlan
{
    public required uint ModuleGuestBase { get; init; }
    public required IReadOnlyList<KamekPatchClassification> Classifications { get; init; }

    public IEnumerable<KamekPatchClassification> ModuleCommands =>
        Classifications.Where(c => c.Kind == KamekPatchClassificationKind.ModuleCommand);

    public IEnumerable<KamekPatchClassification> ExecutablePatches =>
        Classifications.Where(c => c.Kind == KamekPatchClassificationKind.BaseExecutablePatch);

    public IEnumerable<KamekPatchClassification> DataPatches =>
        Classifications.Where(c => c.Kind == KamekPatchClassificationKind.BaseDataPatch);

    public IEnumerable<KamekPatchClassification> DroppedReservedRegionWrites =>
        Classifications.Where(c => c.Kind == KamekPatchClassificationKind.DroppedReservedRegion);

    public IEnumerable<KamekPatchClassification> Unsupported =>
        Classifications.Where(c => c.Kind == KamekPatchClassificationKind.Unsupported);

    public string BuildTextReport(string title)
    {
        using var writer = new StringWriter();
        writer.WriteLine(title);
        writer.WriteLine($"Module base: 0x{ModuleGuestBase:X8}");
        writer.WriteLine();
        writer.WriteLine("Classification summary:");
        foreach (var group in Classifications.GroupBy(c => c.Kind).OrderBy(g => g.Key.ToString()))
        {
            writer.WriteLine($"  {group.Key,-22} {group.Count(),8:N0}");
        }
        writer.WriteLine();

        writer.WriteLine("Executable overlays:");
        foreach (var group in ExecutablePatches
            .Where(c => c.ContainingFunctionStart.HasValue && c.ContainingFunctionEnd.HasValue)
            .GroupBy(c => (Start: c.ContainingFunctionStart!.Value, End: c.ContainingFunctionEnd!.Value, c.SectionName))
            .OrderBy(g => g.Key.Start))
        {
            writer.WriteLine($"  0x{group.Key.Start:X8}-0x{group.Key.End:X8} {group.Key.SectionName}");
            foreach (var item in group.OrderBy(c => c.CommandAddress).Take(32))
            {
                writer.WriteLine($"    0x{item.CommandAddress:X8} {item.CommandId} {FormatArgs(item.Arguments)}");
            }
            if (group.Count() > 32)
            {
                writer.WriteLine($"    ... {group.Count() - 32:N0} more");
            }
        }
        if (!ExecutablePatches.Any())
        {
            writer.WriteLine("  none");
        }
        writer.WriteLine();

        writer.WriteLine("Data patches:");
        foreach (var group in DataPatches.GroupBy(c => c.SectionName ?? "<unknown>").OrderBy(g => g.Key))
        {
            writer.WriteLine($"  {group.Key}: {group.Count():N0}");
        }
        if (!DataPatches.Any())
        {
            writer.WriteLine("  none");
        }
        writer.WriteLine();

        writer.WriteLine("Unsupported:");
        foreach (var item in Unsupported.Take(128))
        {
            writer.WriteLine($"  0x{item.CommandAddress:X8} {item.CommandId}: {item.Reason}");
        }
        if (!Unsupported.Any())
        {
            writer.WriteLine("  none");
        }
        else if (Unsupported.Count() > 128)
        {
            writer.WriteLine($"  ... {Unsupported.Count() - 128:N0} more");
        }

        return writer.ToString();
    }

    private static string FormatArgs(IReadOnlyList<uint> args) =>
        args.Count == 0 ? string.Empty : string.Join(", ", args.Select(a => $"0x{a:X8}"));
}

public static class KamekPatchPlanner
{
    private const uint CodehandlerRegionStart = 0x80001920;
    private const uint CodehandlerRegionEnd = 0x80001924;

    public static KamekPatchPlan Build(KamekChunk chunk, BaseManifest baseManifest, uint moduleGuestBase)
    {
        var functionIndex = new BaseFunctionIndex(baseManifest.Functions);
        var classifications = new List<KamekPatchClassification>(chunk.Commands.Count);

        foreach (var command in chunk.Commands)
        {
            if (command.AddressIsRelative)
            {
                classifications.Add(new KamekPatchClassification(
                    KamekPatchClassificationKind.ModuleCommand,
                    command.Id,
                    moduleGuestBase + command.Address,
                    false,
                    null,
                    null,
                    "Kamek module",
                    "relative command applies to Kamek module image",
                    command.Arguments));
                continue;
            }

            var section = FindSection(baseManifest, command.Address);
            if (section is null)
            {
                classifications.Add(Unsupported(command, "absolute command address is outside known base sections"));
                continue;
            }

            if (!section.Executable &&
                command.Address >= CodehandlerRegionStart && command.Address < CodehandlerRegionEnd)
            {
                classifications.Add(new KamekPatchClassification(
                    KamekPatchClassificationKind.DroppedReservedRegion,
                    command.Id,
                    command.Address,
                    true,
                    null,
                    null,
                    section.Name,
                    "write to the WFC codehandler probe word is dropped; the recomp keeps 0x80001920 clean for the WFC anticheat probe",
                    command.Arguments));
                continue;
            }

            if (section.Executable)
            {
                var function = functionIndex.FindContaining(command.Address);
                if (function is null)
                {
                    classifications.Add(Unsupported(command, $"executable address is in {section.Name} but no containing function range was found"));
                    continue;
                }

                classifications.Add(new KamekPatchClassification(
                    KamekPatchClassificationKind.BaseExecutablePatch,
                    command.Id,
                    command.Address,
                    true,
                    function.Start,
                    function.End,
                    section.Name,
                    "absolute executable command requires patched base overlay",
                    command.Arguments));
                continue;
            }

            classifications.Add(new KamekPatchClassification(
                KamekPatchClassificationKind.BaseDataPatch,
                command.Id,
                command.Address,
                true,
                null,
                null,
                section.Name,
                "absolute non-executable command applies as data patch",
                command.Arguments));
        }

        return new KamekPatchPlan
        {
            ModuleGuestBase = moduleGuestBase,
            Classifications = classifications
        };
    }

    private static KamekPatchClassification Unsupported(KamekCommand command, string reason) =>
        new(
            KamekPatchClassificationKind.Unsupported,
            command.Id,
            command.Address,
            command.AddressIsAbsolute,
            null,
            null,
            null,
            reason,
            command.Arguments);

    private static BaseSectionMetadata? FindSection(BaseManifest manifest, uint address) =>
        manifest.Sections.FirstOrDefault(section => address >= section.GuestStart && address < section.GuestEnd);
}
