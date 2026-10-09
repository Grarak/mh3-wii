using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;

namespace Translator.Core.Mods.Mkwii;

internal static class RetroWfcJson
{
    public static JsonElement? GetObject(JsonElement obj, string name) =>
        obj.ValueKind == JsonValueKind.Object &&
        obj.TryGetProperty(name, out var value) &&
        value.ValueKind == JsonValueKind.Object ? value : null;

    public static JsonElement? GetArray(JsonElement obj, string name) =>
        obj.ValueKind == JsonValueKind.Object &&
        obj.TryGetProperty(name, out var value) &&
        value.ValueKind == JsonValueKind.Array ? value : null;

    public static string? GetString(JsonElement obj, string name) =>
        obj.ValueKind == JsonValueKind.Object &&
        obj.TryGetProperty(name, out var value) &&
        value.ValueKind == JsonValueKind.String ? value.GetString() : null;

    public static bool? GetBool(JsonElement obj, string name) =>
        obj.ValueKind == JsonValueKind.Object &&
        obj.TryGetProperty(name, out var value) &&
        value.ValueKind is JsonValueKind.True or JsonValueKind.False ? value.GetBoolean() : null;

    public static int? GetInt(JsonElement obj, string name) =>
        obj.ValueKind == JsonValueKind.Object &&
        obj.TryGetProperty(name, out var value) &&
        value.ValueKind == JsonValueKind.Number && value.TryGetInt32(out var result) ? result : null;

    public static IReadOnlyList<string> ReadStringArray(JsonElement obj, string name)
    {
        if (GetArray(obj, name) is not { } array) return Array.Empty<string>();
        return array.EnumerateArray()
            .Where(static item => item.ValueKind == JsonValueKind.String && !string.IsNullOrWhiteSpace(item.GetString()))
            .Select(static item => item.GetString()!)
            .ToList();
    }

    public static bool TryReadAddress(JsonElement obj, string name, out uint value)
    {
        value = 0;
        return obj.ValueKind == JsonValueKind.Object &&
               obj.TryGetProperty(name, out var element) &&
               TryReadAddress(element, out value);
    }

    public static bool TryReadAddress(JsonElement element, out uint value)
    {
        value = 0;
        var text = element.ValueKind == JsonValueKind.String ? element.GetString() : null;
        return text is not null && GuestTargetParser.TryParseHexAddress(text, out value);
    }
}
