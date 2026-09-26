namespace WiiCompiled.Setup.Common;

/// <summary>
/// Resolves the one canonical Retro Rewind install. Wheel Wizard owns and passes it as
/// <c>--retro-dir</c>; each installer only resolves, reads, and records it, never packages or
/// copies it.
/// </summary>
public static class RetroRewindSource
{
    /// <summary>
    /// Resolves the <c>RetroRewind6</c> folder from a selection that may be the folder itself or a
    /// parent containing exactly one <c>RetroRewind6/Binaries/Code.pul</c>.
    /// </summary>
    public static string ResolveRetroRewind6(string selected)
    {
        if (string.IsNullOrWhiteSpace(selected))
            throw new InvalidDataException("Choose the canonical Retro Rewind folder.");
        var root = Path.GetFullPath(selected);
        if (!Directory.Exists(root))
            throw new DirectoryNotFoundException($"The Retro Rewind folder does not exist: {root}");

        var candidates = new List<string> { root, Path.Combine(root, "RetroRewind6") };
        // A folder produced by unpacking the published distribution keeps an extra wrapper directory.
        foreach (var child in Directory.EnumerateDirectories(root))
            candidates.Add(Path.Combine(child, "RetroRewind6"));

        var matches = candidates
            .Where(candidate => File.Exists(Path.Combine(candidate, "Binaries", "Code.pul")))
            .Select(Path.GetFullPath)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToList();

        return matches.Count switch
        {
            1 => ResolveFinalDirectory(matches[0]),
            0 => throw new InvalidDataException(
                $"{root} does not contain RetroRewind6\\Binaries\\Code.pul. " +
                "Select the canonical Retro Rewind folder, or the folder that contains it."),
            _ => throw new InvalidDataException(
                $"{root} contains more than one RetroRewind6\\Binaries\\Code.pul. " +
                "Select the exact Retro Rewind folder to use.")
        };
    }

    /// <summary>
    /// Must resolve a junction/symlink RetroRewind6 to its real directory before fingerprinting: the
    /// compile-input identity records on-disk kind, and a snapshot copy is always a real directory, so an
    /// unresolved link can never match its own copy and installs fail forever with a misleading error.
    /// </summary>
    private static string ResolveFinalDirectory(string path)
    {
        var directory = new DirectoryInfo(path);
        if (directory.LinkTarget is null)
            return path;
        var target = directory.ResolveLinkTarget(returnFinalTarget: true)?.FullName;
        if (target is null || !Directory.Exists(target))
            throw new InvalidDataException($"The Retro Rewind folder is a link to a missing target: {path}");
        return target;
    }
}
