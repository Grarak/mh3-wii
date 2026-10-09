namespace WiiCompiled.Setup.Common;

/// <summary>
/// A portable installation is a self-contained directory tree the user can move or carry on removable media:
/// <code>
/// &lt;root&gt;\portable.txt   marker; its contents are irrelevant
/// &lt;root&gt;\Install\       the installation directory
/// &lt;root&gt;\UserData\      runtime user state (Config.toml, NAND, Cache, Logs)
/// </code>
/// The runtime finds the same root independently (<c>runtime/include/runtime_config.h</c>,
/// <c>PortableRootDirectory</c>); this class must keep the same marker name, layout, and depth bound.
/// Shared by both installers - Linux's CLI has no <c>--portable</c> flag, so it only ever calls
/// <see cref="TryFind"/>/<see cref="UserDataDirectory"/>/<see cref="Contains"/> (always missing,
/// since it never creates a marker file), not <see cref="Create"/>.
/// </summary>
public static class PortableRoot
{
    public const string MarkerFileName = "portable.txt";
    public const string UserDataDirectoryName = "UserData";
    public const string InstallDirectoryName = "Install";

    /// <summary>
    /// Parents searched above the start directory. Kept small (installs sit 2 levels below root,
    /// <c>&lt;root&gt;\Install\Base\game.exe</c>) so an unrelated marker far up a drive can't capture it.
    /// </summary>
    public const int MaximumSearchDepth = 4;

    /// <summary>
    /// The portable root <paramref name="startDirectory"/> belongs to, or null otherwise. Callers pass the
    /// installation directory (not the running executable's location), since a downloaded setup runs from Downloads.
    /// </summary>
    public static string? TryFind(string startDirectory)
    {
        if (string.IsNullOrWhiteSpace(startDirectory)) return null;
        string current;
        try
        {
            // Normalize before trimming: "C:\" trimmed to "C:" is the current directory on that
            // drive, not the drive root.
            current = Normalize(startDirectory);
        }
        catch (Exception ex) when (ex is ArgumentException or NotSupportedException or PathTooLongException)
        {
            return null;
        }

        for (var level = 0; level <= MaximumSearchDepth; level++)
        {
            if (File.Exists(Path.Combine(current, MarkerFileName))) return current;
            var parent = Path.GetDirectoryName(current);
            if (string.IsNullOrEmpty(parent) || parent.Equals(current, StringComparison.Ordinal)) break;
            current = parent;
        }
        return null;
    }

    public static string UserDataDirectory(string root) => Path.Combine(root, UserDataDirectoryName);

    /// <summary>Whether <paramref name="candidate"/> is <paramref name="root"/> or lives inside it.</summary>
    public static bool Contains(string root, string candidate) =>
        FileSystemUtilities.PathContains(root, candidate);

    /// <summary>
    /// Establishes the root an install is about to publish into: the marker and the user-state
    /// directory must exist before anything resolves a configuration path, because that resolution is
    /// what decides whether the installation writes portable or machine-wide settings.
    /// </summary>
    public static string Create(string root)
    {
        var full = Normalize(root);
        EnsureUsableRoot(full);
        Directory.CreateDirectory(full);
        Directory.CreateDirectory(UserDataDirectory(full));
        var marker = Path.Combine(full, MarkerFileName);
        if (!File.Exists(marker))
        {
            File.WriteAllText(marker,
                "WiiCompiled portable installation." + Environment.NewLine +
                "This marker makes the runtime keep Config.toml, NAND, Cache, and Logs in UserData\\ " +
                "beside it instead of in %LOCALAPPDATA%." + Environment.NewLine +
                "Delete it to make this installation use per-user application data again." +
                Environment.NewLine);
        }
        return full;
    }

    /// <summary>
    /// A portable root is deleted wholesale by the user, so like an install directory it may never be a
    /// drive root or well-known system location; both share <see cref="FileSystemUtilities.EnsureUsableLocation"/>.
    /// </summary>
    public static void EnsureUsableRoot(string root) =>
        FileSystemUtilities.EnsureUsableLocation(root, "A portable installation root");

    private static string Normalize(string path) => FileSystemUtilities.NormalizePath(path);
}
