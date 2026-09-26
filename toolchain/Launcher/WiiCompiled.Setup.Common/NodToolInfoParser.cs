using System.Text.RegularExpressions;

namespace WiiCompiled.Setup.Common;

/// <summary>Disc metadata parsed from `nodtool info`'s stdout.</summary>
public sealed record NodToolDiscInfo(string GameId, string Title, int Revision);

/// <summary>
/// Parses the plain-text stdout of `nodtool info &lt;iso&gt;`. nodtool has no JSON output mode, but
/// prints one unconditional disc-level Title/Game ID/Disc-Revision block (via its own
/// `print_header`) before any per-partition breakdown - Wii discs also have differently-scoped
/// "Title"/"Game ID" lines per update/channel partition further down, so the first match of each
/// pattern is always the disc-level one both installers want.
/// </summary>
public static partial class NodToolInfoParser
{
    public static NodToolDiscInfo Parse(string infoStdout)
    {
        var gameIdMatch = GameIdLine().Match(infoStdout);
        if (!gameIdMatch.Success)
            throw new InvalidOperationException("nodtool did not return disc metadata.");
        var titleMatch = TitleLine().Match(infoStdout);
        var revisionMatch = RevisionLine().Match(infoStdout);
        return new NodToolDiscInfo(
            GameId: gameIdMatch.Groups[1].Value,
            Title: titleMatch.Success ? titleMatch.Groups[1].Value : "",
            Revision: revisionMatch.Success ? int.Parse(revisionMatch.Groups[1].Value) : 0);
    }

    [GeneratedRegex(@"^Game ID: (\S+)", RegexOptions.Multiline)]
    private static partial Regex GameIdLine();

    [GeneratedRegex(@"^Title: (.+)$", RegexOptions.Multiline)]
    private static partial Regex TitleLine();

    [GeneratedRegex(@"^Disc \d+, Revision (\d+)", RegexOptions.Multiline)]
    private static partial Regex RevisionLine();
}
