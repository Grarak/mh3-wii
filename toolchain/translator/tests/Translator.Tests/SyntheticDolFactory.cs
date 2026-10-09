using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.IO;
using Translator.Core.Parsing.Dol;

namespace Translator.Tests;

internal static class SyntheticDolFactory
{
    private const int HeaderSize = 0x100;

    internal readonly record struct SectionSpec(bool IsText, int Slot, uint Address, byte[] Data);

    public static SectionSpec Text(int slot, uint address, params uint[] words) =>
        new(true, slot, address, WordsToBytes(words));

    public static SectionSpec Data(int slot, uint address, params byte[] data) =>
        new(false, slot, address, data);

    public static DolFile Create(uint entryPoint, uint bssAddress = 0, uint bssSize = 0, params SectionSpec[] sections)
        => DolFile.Load(new MemoryStream(CreateBytes(entryPoint, bssAddress, bssSize, sections), writable: false));

    public static byte[] CreateBytes(uint entryPoint, uint bssAddress = 0, uint bssSize = 0, params SectionSpec[] sections)
    {
        var textOffsets = new uint[7];
        var dataOffsets = new uint[11];
        var textAddresses = new uint[7];
        var dataAddresses = new uint[11];
        var textSizes = new uint[7];
        var dataSizes = new uint[11];
        var blobs = new List<(uint Offset, byte[] Data)>();

        var nextOffset = (uint)HeaderSize;
        foreach (var section in sections)
        {
            if (section.IsText)
            {
                textOffsets[section.Slot] = nextOffset;
                textAddresses[section.Slot] = section.Address;
                textSizes[section.Slot] = (uint)section.Data.Length;
            }
            else
            {
                dataOffsets[section.Slot] = nextOffset;
                dataAddresses[section.Slot] = section.Address;
                dataSizes[section.Slot] = (uint)section.Data.Length;
            }

            blobs.Add((nextOffset, section.Data));
            nextOffset += (uint)section.Data.Length;
        }

        var image = new byte[nextOffset];
        WriteArray(image, 0x00, textOffsets);
        WriteArray(image, 0x1C, dataOffsets);
        WriteArray(image, 0x48, textAddresses);
        WriteArray(image, 0x64, dataAddresses);
        WriteArray(image, 0x90, textSizes);
        WriteArray(image, 0xAC, dataSizes);
        WriteU32(image, 0xD8, bssAddress);
        WriteU32(image, 0xDC, bssSize);
        WriteU32(image, 0xE0, entryPoint);

        foreach (var (offset, data) in blobs)
        {
            data.CopyTo(image.AsSpan((int)offset));
        }

        return image;
    }

    private static byte[] WordsToBytes(params uint[] words)
    {
        var bytes = new byte[words.Length * 4];
        for (var i = 0; i < words.Length; i++)
        {
            BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(i * 4, 4), words[i]);
        }

        return bytes;
    }

    private static void WriteArray(byte[] image, int offset, IReadOnlyList<uint> values)
    {
        for (var i = 0; i < values.Count; i++)
        {
            WriteU32(image, offset + (i * 4), values[i]);
        }
    }

    private static void WriteU32(byte[] image, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(offset, 4), value);
}
