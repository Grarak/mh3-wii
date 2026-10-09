using System.Text.Json;

namespace WiiCompiled.Setup.Common;

/// <summary>Reads and atomically writes the small JSON state documents each installer keeps.</summary>
public static class JsonState
{
    private static readonly JsonSerializerOptions ReadOptions = new() { PropertyNameCaseInsensitive = true };
    private static readonly JsonSerializerOptions WriteOptions = new() { WriteIndented = true };

    public static T? TryRead<T>(string path) where T : class
    {
        try
        {
            return File.Exists(path) ? JsonSerializer.Deserialize<T>(File.ReadAllText(path), ReadOptions) : null;
        }
        catch
        {
            // A truncated or hand-edited state document must degrade into "unknown", which every
            // caller already treats as "assume stale and rebuild", not into a crash.
            return null;
        }
    }

    public static void Write<T>(string path, T value)
    {
        var directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
        var tempPath = path + ".tmp-" + Guid.NewGuid().ToString("N");
        File.WriteAllText(tempPath, JsonSerializer.Serialize(value, WriteOptions));
        File.Move(tempPath, path, overwrite: true);
    }
}
