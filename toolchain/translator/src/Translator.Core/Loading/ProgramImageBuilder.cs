using System;
using System.Linq;
using Translator.Core.Parsing.Dol;
using Translator.Core.Parsing.Rel;

namespace Translator.Core.Loading;

public sealed class ProgramImageBuilder
{
    public ProgramImage Build(string dolPath, string relPath, uint relBaseAddress)
    {
        var dol = DolFile.Load(dolPath);
        var rel = RelFile.Load(relPath).BuildImage(relBaseAddress);
        return Build(dol, rel);
    }

    public ProgramImage Build(
        DolFile dol,
        RelImage? rel = null,
        uint ramBase = MemoryLayout.RamBase,
        int ramSize = MemoryLayout.RamSize)
    {
        if (ramSize <= 0)
        {
            throw new ArgumentOutOfRangeException(nameof(ramSize));
        }

        var ram = new byte[ramSize];

        // Some DOL headers report a coarse BSS span that overlaps initialized small-data sections.
        // Clear first so the bytes that exist in the file win when ranges overlap.
        foreach (var bss in dol.Sections.Where(s => s.Kind == SectionKind.Bss))
        {
            var offset = checked((int)(bss.VirtualAddress - ramBase));
            var end = offset + (int)bss.Size;
            if (offset < 0 || end > ram.Length)
            {
                throw new InvalidOperationException($"BSS section spills outside RAM window (offset {offset}, end {end})");
            }

            ram.AsSpan(offset, (int)bss.Size).Clear();
        }

        foreach (var section in dol.Sections)
        {
            if (section.Size == 0 || section.Kind == SectionKind.Bss)
            {
                continue;
            }

            var offset = checked((int)(section.VirtualAddress - ramBase));
            var end = offset + (int)section.Size;
            if (offset < 0 || end > ram.Length)
            {
                throw new InvalidOperationException($"DOL section {section.Name} spills outside RAM window (offset {offset}, end {end})");
            }

            section.Data.Span.CopyTo(ram.AsSpan(offset, (int)section.Size));
        }

        var relRange = new AddressRange(0, 0);
        if (rel is not null)
        {
            var relOffset = checked((int)(rel.BaseAddress - ramBase));
            var relEnd = relOffset + rel.Data.Length;
            if (relOffset < 0 || relEnd > ram.Length)
            {
                throw new InvalidOperationException($"REL image spills outside RAM window (offset {relOffset}, end {relEnd})");
            }

            rel.Data.CopyTo(ram, relOffset);
            relRange = rel.Range;
        }

        var usedRange = rel is null ? dol.MemoryRange : AddressRange.Union(dol.MemoryRange, rel.Range);

        var sha = ChecksumUtilities.Sha256Hex(ram);
        return new ProgramImage(ram, usedRange, dol.MemoryRange, relRange, sha, ramBase);
    }
}
