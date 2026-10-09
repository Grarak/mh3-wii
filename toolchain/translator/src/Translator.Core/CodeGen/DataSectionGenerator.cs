using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using Translator.Core.IO;
using Translator.Core.Loading;
using Translator.Core.Parsing.Dol;
using Translator.Core.Parsing.Rel;

namespace Translator.Core.CodeGen;

/// <summary>
/// Generates C++ code to initialize DOL and REL data sections directly in the binary.
/// This allows running without loading a RAM dump at runtime.
/// </summary>
public static class DataSectionGenerator
{
    /// <summary>
    /// Generates a C++ header/source pair that initializes all data sections.
    /// </summary>
    public static void Generate(
        DolFile dol,
        RelImage? rel,
        string outputPath,
        string projectName = "PowerPC DOL",
        string relName = "rel_module",
        string? blobReferenceDirectory = null)
    {
        var sections = new List<DataSectionEntry>();
        
        // Add DOL data sections (skip .bss - it's zero-initialized)
        foreach (var section in dol.Sections)
        {
            if (section.Kind == SectionKind.Bss || !section.HasData || section.Size == 0)
            {
                continue;
            }
            
            // Include both code and data sections - they all need to be in memory
            sections.Add(new DataSectionEntry(
                Name: SanitizeName(section.Name),
                Address: section.VirtualAddress,
                Data: section.Data,
                Source: "DOL"));
        }
        
        // Add REL data if provided
        if (rel != null && rel.Data.Length > 0)
        {
            sections.Add(new DataSectionEntry(
                Name: SanitizeName(relName),
                Address: rel.BaseAddress,
                Data: rel.Data,
                Source: "REL"));
        }
        
        var blobDirectory = Path.Combine(
            Path.GetDirectoryName(outputPath) ?? ".",
            Path.GetFileNameWithoutExtension(outputPath) + "_blobs");
        var blobAssemblyPath = Path.Combine(
            Path.GetDirectoryName(outputPath) ?? ".",
            Path.GetFileNameWithoutExtension(outputPath) + "_blobs.S");

        WriteBlobFiles(
            sections,
            blobDirectory,
            blobReferenceDirectory ?? blobDirectory,
            blobAssemblyPath);
        WriteSourceFile(sections, outputPath, dol.BssAddress, dol.BssSize, projectName);
    }
    
    private static string SanitizeName(string name)
    {
        var sb = new StringBuilder();
        foreach (var c in name)
        {
            if (char.IsLetterOrDigit(c))
            {
                sb.Append(c);
            }
            else
            {
                sb.Append('_');
            }
        }
        return sb.ToString();
    }

    private static void WriteBlobFiles(
        List<DataSectionEntry> sections,
        string blobDirectory,
        string blobReferenceDirectory,
        string assemblyPath)
    {
        var blobs = sections.Select(section => new AssemblyBlob(
            $"{section.Name}.bin",
            $"kData_{section.Name}",
            section.Data,
            $"{section.Source}: {section.Name} @ 0x{section.Address:X8} ({section.Data.Length} bytes)"))
            .ToList();
        AssemblyBlobWriter.Write(
            assemblyPath,
            blobDirectory,
            blobReferenceDirectory,
            blobs,
            "// AUTO-GENERATED - DO NOT EDIT",
            "// Binary DOL/REL section payloads for data_sections_init.cpp.");
    }
    
    private static void WriteSourceFile(
        List<DataSectionEntry> sections,
        string path,
        uint bssAddress,
        uint bssSize,
        string projectName)
    {
        var sb = new StringBuilder(capacity: 8 * 1024 * 1024);
        using var writer = new StringWriter(sb);
        
        writer.WriteLine("// AUTO-GENERATED - DO NOT EDIT");
        writer.WriteLine($"// Data section initializer for {projectName}");
        writer.WriteLine("// This file embeds the DOL and REL data sections directly in the binary.");
        writer.WriteLine("#include \"memory.h\"");
        writer.WriteLine("#include <cstring>");
        writer.WriteLine("#include <cstddef>");
        writer.WriteLine("#include <cstdint>");
        writer.WriteLine("#include <iostream>");
        writer.WriteLine();

        writer.WriteLine("extern \"C\" {");
        foreach (var section in sections)
        {
            writer.WriteLine($"extern const uint8_t kData_{section.Name}[];");
        }
        writer.WriteLine("} // extern \"C\"");
        
        // Write the initialization function with C linkage (for cross-compilation-unit linking)
        writer.WriteLine();
        writer.WriteLine("namespace {");
        writer.WriteLine("bool g_dataInitialized = false;");
        writer.WriteLine("} // namespace");
        writer.WriteLine();
        writer.WriteLine("extern \"C\" void InitializeDataSections() {");
        writer.WriteLine("    if (g_dataInitialized) return;");
        writer.WriteLine("    g_dataInitialized = true;");
        writer.WriteLine();
        writer.WriteLine("    std::cout << \"[runtime] Initializing embedded data sections...\" << std::endl;");
        writer.WriteLine();
        
        foreach (var section in sections)
        {
            writer.WriteLine($"    // {section.Source} section: {section.Name} @ 0x{section.Address:X8}");
            writer.WriteLine($"    constexpr size_t kSize_{section.Name} = {section.Data.Length}u;");
            writer.WriteLine($"    if (Memory::Contains(0x{section.Address:X8}u, kSize_{section.Name})) {{");
            writer.WriteLine($"        std::memcpy(Memory::GetPointer(0x{section.Address:X8}u, kSize_{section.Name}),");
            writer.WriteLine($"                    kData_{section.Name}, kSize_{section.Name});");
            writer.WriteLine($"        std::cout << \"[runtime]   Loaded {section.Name} ({section.Data.Length} bytes) @ 0x{section.Address:X8}\" << std::endl;");
            writer.WriteLine("    }");
            writer.WriteLine();
        }
        
        // BSS is zero-initialized (memory starts at zero so we don't need to do anything)
        if (bssSize > 0)
        {
            writer.WriteLine($"    // BSS section @ 0x{bssAddress:X8} ({bssSize} bytes) - memory already zero-initialized");
        }
        
        writer.WriteLine("}");
        writer.WriteLine();
        
        // Also provide a check function with C linkage
        writer.WriteLine("extern \"C\" bool IsDataSectionsInitialized() {");
        writer.WriteLine("    return g_dataInitialized;");
        writer.WriteLine("}");

        FileOutput.WriteTextIfChanged(path, sb.ToString());
    }
    
    private record DataSectionEntry(string Name, uint Address, ReadOnlyMemory<byte> Data, string Source);
}
