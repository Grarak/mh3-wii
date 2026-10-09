using System;
using System.Text;
using System.Text.Json;

namespace Translator.Core.IO;

/// <summary>
/// The one JSON artifact writer and the only serializer options the translator owns.
/// Publishes through <see cref="FileOutput.WriteBytesIfChanged"/>, so writes are content-gated and atomic.
/// </summary>
public static class JsonOutput
{
    /// <summary>
    /// Machine-read documents. Indentation only inflates files that already
    /// measure tens of megabytes and that no human opens.
    /// </summary>
    public static readonly JsonSerializerOptions Compact = new() { WriteIndented = false };

    /// <summary>Machine-read documents whose schema is spelled in camelCase.</summary>
    public static readonly JsonSerializerOptions CompactCamelCase = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = false
    };

    /// <summary>Documents small enough - and read by people often enough - to indent.</summary>
    public static readonly JsonSerializerOptions Indented = new() { WriteIndented = true };

    private static readonly byte[] TrailingNewLine = Encoding.UTF8.GetBytes(Environment.NewLine);

    public static bool WriteIfChanged<T>(string path, T value, JsonSerializerOptions options)
    {
        var json = JsonSerializer.SerializeToUtf8Bytes(value, options);
        var content = new byte[json.Length + TrailingNewLine.Length];
        json.CopyTo(content, 0);
        TrailingNewLine.CopyTo(content, json.Length);
        return FileOutput.WriteBytesIfChanged(path, content);
    }
}
