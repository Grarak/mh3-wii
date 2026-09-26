using System.Text;

namespace WiiCompiled.Setup.Common;

public sealed record RuntimeConfigSnapshot(bool Existed, byte[] Contents);

/// <summary>
/// Reads and writes the runtime's <c>Config.toml</c>. Every entry point takes the file it operates on
/// since its location depends on the installation (portable <c>UserData</c> vs. per-user app data);
/// callers obtain it once via <see cref="ResolveConfigPath"/>. Shared by both installers - Linux
/// never creates a <see cref="PortableRoot.MarkerFileName"/> marker file, so
/// <see cref="ResolveConfigPath"/>/<see cref="FormatPathValue"/>'s portable-root lookups always miss
/// there and this degrades to the same plain per-user-app-data, always-absolute-path behavior a
/// non-portable Windows install already gets.
/// </summary>
public static class RuntimeConfiguration
{
    public const string ConfigFileName = "Config.toml";

    /// <summary>
    /// Ordinal, matching the runtime parser/writer (<c>runtime_config.h</c>) exactly. Case-insensitive
    /// matching here would let setup rewrite a <c>[Paths]</c>/<c>Dvd_Root</c> spelling the runtime ignores.
    /// </summary>
    private const StringComparison KeyComparison = StringComparison.Ordinal;

    /// <summary>The per-user configuration an ordinary (non-portable) installation shares.</summary>
    public static string ApplicationDataConfigPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "WiiCompiled", ConfigFileName);

    /// <summary>
    /// The configuration file that governs <paramref name="installDirectory"/>. This mirrors
    /// <c>RuntimeConfigFile::ResolveConfigPath</c> in the runtime: portable roots win, everything
    /// else is per-user application data.
    /// </summary>
    public static string ResolveConfigPath(string? installDirectory)
    {
        if (!string.IsNullOrWhiteSpace(installDirectory) &&
            PortableRoot.TryFind(installDirectory) is { } portableRoot)
            return Path.Combine(PortableRoot.UserDataDirectory(portableRoot), ConfigFileName);
        return ApplicationDataConfigPath;
    }

    public static RuntimeConfigSnapshot Capture(string configPath) =>
        File.Exists(configPath)
            ? new RuntimeConfigSnapshot(true, File.ReadAllBytes(configPath))
            : new RuntimeConfigSnapshot(false, []);

    public static void Restore(string configPath, RuntimeConfigSnapshot snapshot)
    {
        if (snapshot.Existed)
            FileSystemUtilities.WriteAtomic(configPath, snapshot.Contents);
        else
            File.Delete(configPath);
    }

    public static void SetDvdRoot(string configPath, string dvdRoot) =>
        SetPath(configPath, "dvd_root", dvdRoot);

    /// <summary>
    /// Records the one canonical Retro Rewind installation Wheel Wizard owns. The runtime builds its
    /// asset overlay by scanning this directory live at launch, so an asset-only Retro Rewind update
    /// needs no backend work: the next launch simply reads the new files.
    /// </summary>
    public static void SetRetroRewindRoot(string configPath, string retroRewindRoot) =>
        SetPath(configPath, "retro_rewind_root", retroRewindRoot);

    /// <summary>The canonical Retro Rewind root, or null when no installation has recorded one.</summary>
    public static string? GetRetroRewindRoot(string configPath) =>
        GetResolvedPath(configPath, "retro_rewind_root");

    public static void RemoveRetroRewindRootIfOwned(string configPath, string retroRewindRoot) =>
        RemovePathIfOwned(configPath, "retro_rewind_root", retroRewindRoot);

    public static void RemoveDvdRootIfOwned(string configPath, string dvdRoot) =>
        RemovePathIfOwned(configPath, "dvd_root", dvdRoot);

    /// <summary>The raw stored text of a <c>[paths]</c> key, exactly as the file holds it.</summary>
    public static string? GetPath(string configPath, string key) =>
        TryUnquoteToml(GetRawValue(configPath, "paths", key) ?? "", out var value) ? value : null;

    /// <summary>
    /// A <c>[paths]</c> value as the runtime will actually use it. The runtime resolves every relative
    /// <c>[paths]</c> value against the directory holding <c>Config.toml</c> (never the working
    /// directory), so the host must resolve it the same way before comparing or reading it.
    /// </summary>
    public static string? GetResolvedPath(string configPath, string key)
    {
        var stored = GetPath(configPath, key);
        return string.IsNullOrWhiteSpace(stored) ? null : ResolveAgainstConfig(configPath, stored);
    }

    public static string ConfigDirectory(string configPath) =>
        Path.GetDirectoryName(Path.GetFullPath(configPath))
        ?? throw new InvalidOperationException($"{configPath} has no containing directory.");

    public static string ResolveAgainstConfig(string configPath, string value) =>
        Path.GetFullPath(value, ConfigDirectory(configPath));

    private static void SetPath(string configPath, string key, string value)
    {
        var lines = ReadLinesOrDefault(configPath);
        SetSectionValue(lines, "paths", key, QuoteToml(FormatPathValue(configPath, value)));
        WriteLines(configPath, lines);
    }

    /// <summary>
    /// Paths inside the portable root are stored relative to the config directory (forward slashes,
    /// needing no TOML escaping) so the tree keeps working if the user moves or renames the root.
    /// Everything else, including all non-portable paths, is stored absolute.
    /// </summary>
    private static string FormatPathValue(string configPath, string value)
    {
        var full = Path.GetFullPath(value);
        var configDirectory = ConfigDirectory(configPath);
        if (PortableRoot.TryFind(configDirectory) is not { } root || !PortableRoot.Contains(root, full))
            return full;
        var relative = Path.GetRelativePath(configDirectory, full);
        return Path.IsPathRooted(relative)
            ? full
            : relative.Replace(Path.DirectorySeparatorChar, '/');
    }

    private static void RemovePathIfOwned(string configPath, string key, string expectedValue)
    {
        if (!File.Exists(configPath)) return;
        var lines = File.ReadAllLines(configPath).ToList();
        var inPaths = false;
        var changed = false;
        for (var index = 0; index < lines.Count; index++)
        {
            var trimmed = RemoveComment(lines[index]).Trim();
            if (IsSection(trimmed))
            {
                inPaths = trimmed.Equals("[paths]", KeyComparison);
                continue;
            }
            if (!inPaths || trimmed.Length == 0) continue;
            var equals = trimmed.IndexOf('=');
            if (equals < 0 || !trimmed[..equals].Trim().Equals(key, KeyComparison))
                continue;
            if (!TryUnquoteToml(trimmed[(equals + 1)..].Trim(), out var configured)) return;
            // A portable installation stores this relative, so ownership is decided on the path the
            // runtime would actually resolve, not on the stored text.
            if (!ResolveAgainstConfig(configPath, configured)
                    .Equals(Path.GetFullPath(expectedValue), StringComparison.OrdinalIgnoreCase))
                return;
            lines.RemoveAt(index);
            changed = true;
            break;
        }
        if (changed) WriteLines(configPath, lines);
    }

    /// <summary>The raw TOML literal stored for a key, or null when the section or key is absent.</summary>
    public static string? GetRawValue(string configPath, string section, string key)
    {
        if (!File.Exists(configPath)) return null;
        var header = $"[{section}]";
        var inSection = false;
        foreach (var line in File.ReadLines(configPath))
        {
            var trimmed = RemoveComment(line).Trim();
            if (IsSection(trimmed))
            {
                inSection = trimmed.Equals(header, KeyComparison);
                continue;
            }
            if (!inSection || trimmed.Length == 0) continue;
            var equals = trimmed.IndexOf('=');
            if (equals < 0 || !trimmed[..equals].Trim().Equals(key, KeyComparison)) continue;
            return trimmed[(equals + 1)..].Trim();
        }
        return null;
    }

    /// <summary>
    /// Replaces one key in one section, preserving comments, ordering, and every unrelated setting -
    /// this file belongs to the user and to the in-game settings bar as much as to setup. A
    /// commented-out key is deliberately not matched, matching the runtime's own writer.
    /// </summary>
    private static void SetSectionValue(List<string> lines, string section, string key, string value)
    {
        var header = $"[{section}]";
        var sectionStart = -1;
        var sectionEnd = lines.Count;
        for (var index = 0; index < lines.Count; index++)
        {
            var trimmed = RemoveComment(lines[index]).Trim();
            if (!IsSection(trimmed)) continue;
            if (sectionStart >= 0)
            {
                sectionEnd = index;
                break;
            }
            if (trimmed.Equals(header, KeyComparison)) sectionStart = index;
        }
        if (sectionStart < 0)
        {
            if (lines.Count > 0 && lines[^1].Length != 0) lines.Add("");
            lines.Add(header);
            lines.Add($"{key} = {value}");
            return;
        }

        for (var index = sectionStart + 1; index < sectionEnd; index++)
        {
            var trimmed = RemoveComment(lines[index]).Trim();
            if (trimmed.Length == 0) continue;
            var equals = trimmed.IndexOf('=');
            if (equals >= 0 && trimmed[..equals].Trim().Equals(key, KeyComparison))
            {
                lines[index] = $"{key} = {value}";
                return;
            }
        }

        // Append after the section's last real line, not after the blank line that separates it from
        // the next header, so a generated file stays readable for the user who also edits it by hand.
        var insertAt = sectionEnd;
        while (insertAt > sectionStart + 1 && lines[insertAt - 1].Trim().Length == 0) insertAt--;
        lines.Insert(insertAt, $"{key} = {value}");
    }

    private static List<string> ReadLinesOrDefault(string configPath) =>
        File.Exists(configPath) ? File.ReadAllLines(configPath).ToList() : DefaultConfigLines();

    private static void WriteLines(string configPath, List<string> lines) =>
        FileSystemUtilities.WriteAtomic(configPath,
            Encoding.UTF8.GetBytes(string.Join(Environment.NewLine, lines) + Environment.NewLine));

    private static List<string> DefaultConfigLines() =>
    [
        "# WiiCompiled user configuration",
        "",
        "[paths]"
    ];

    private static bool IsSection(string value) =>
        value.Length >= 2 && value[0] == '[' && value[^1] == ']';

    /// <summary>
    /// Line-for-line port of <c>RemoveComment</c> in <c>runtime_config.h</c>, including its backslash
    /// escaping inside basic strings: without it a Windows path ending in <c>\\"</c> reads as
    /// re-opening the quote, and the two implementations disagree about which <c>#</c> starts a comment.
    /// </summary>
    private static string RemoveComment(string line)
    {
        var inSingle = false;
        var inDouble = false;
        var escaped = false;
        for (var index = 0; index < line.Length; index++)
        {
            var character = line[index];
            if (inDouble && character == '\\' && !escaped)
            {
                escaped = true;
                continue;
            }
            if (character == '\'' && !inDouble) inSingle = !inSingle;
            else if (character == '"' && !inSingle && !escaped) inDouble = !inDouble;
            else if (character == '#' && !inSingle && !inDouble) return line[..index];
            escaped = false;
        }
        return line;
    }

    private static string QuoteToml(string value) =>
        "\"" + value.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";

    public static bool TryUnquoteToml(string value, out string result)
    {
        result = "";
        if (value.Length < 2) return false;
        // Literal strings carry no escapes; the runtime's parser treats them the same way.
        if (value[0] == '\'' && value[^1] == '\'')
        {
            result = value[1..^1];
            return true;
        }
        if (value[0] != '"' || value[^1] != '"') return false;
        var builder = new StringBuilder(value.Length - 2);
        for (var index = 1; index < value.Length - 1; index++)
        {
            if (value[index] == '\\' && index + 1 < value.Length - 1)
            {
                var escaped = value[++index];
                builder.Append(escaped switch { '\\' => '\\', '"' => '"', _ => escaped });
            }
            else
            {
                builder.Append(value[index]);
            }
        }
        result = builder.ToString();
        return true;
    }
}
