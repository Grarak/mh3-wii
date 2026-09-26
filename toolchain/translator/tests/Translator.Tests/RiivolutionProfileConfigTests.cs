using Translator.Cli.Configuration;
using Xunit;

namespace Translator.Tests;

public sealed class RiivolutionProfileConfigTests
{
    [Fact]
    public void ParsesRiivolutionSectionAndNormalizesPackRelativeXmlPath()
    {
        var root = CreateTempRoot();
        try
        {
            var projectPath = WriteProject(
                root,
                "    riivolution:",
                "      xml: .\\xml\\RetroRewind6.xml",
                "      options:",
                "        - section: Retro Rewind",
                "          option: Pack",
                "          choice: 1",
                "        - option: Extra");

            var profile = TranslationProjectConfig.Load(projectPath).Profiles["retro-rewind"];

            Assert.NotNull(profile.Riivolution);
            Assert.Equal("xml/RetroRewind6.xml", profile.Riivolution!.Xml);
            Assert.Collection(
                profile.Riivolution.Options,
                option => Assert.Equal(new ProjectRiivolutionOption("Retro Rewind", "Pack", 1u), option),
                option => Assert.Equal(new ProjectRiivolutionOption(string.Empty, "Extra", 0u), option));
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void ProfileWithoutRiivolutionLeavesItUnset()
    {
        var root = CreateTempRoot();
        try
        {
            var projectPath = WriteProject(root);
            Assert.Null(TranslationProjectConfig.Load(projectPath).Profiles["retro-rewind"].Riivolution);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void RiivolutionWithoutXmlIsRejected()
    {
        var root = CreateTempRoot();
        try
        {
            var projectPath = WriteProject(
                root,
                "    riivolution:",
                "      options:",
                "        - option: Pack",
                "          choice: 1");

            var error = Assert.Throws<InvalidDataException>(() => TranslationProjectConfig.Load(projectPath));
            Assert.Contains("profiles.retro-rewind.riivolution.xml", error.Message, StringComparison.Ordinal);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void RiivolutionOptionWithoutOptionNameIsRejected()
    {
        var root = CreateTempRoot();
        try
        {
            var projectPath = WriteProject(
                root,
                "    riivolution:",
                "      xml: xml/RetroRewind6.xml",
                "      options:",
                "        - section: Retro Rewind",
                "          choice: 1");

            var error = Assert.Throws<InvalidDataException>(() => TranslationProjectConfig.Load(projectPath));
            Assert.Contains("riivolution.options[0].option", error.Message, StringComparison.Ordinal);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    private static string WriteProject(string root, params string[] profileLines)
    {
        File.WriteAllBytes(Path.Combine(root, "main.dol"), new byte[0x100]);
        var lines = new List<string>
        {
            "schema_version: 1",
            "workspace_root: .",
            "",
            "project:",
            "  id: riivolution-test",
            "",
            "inputs:",
            "  dol:",
            "    path: main.dol",
            "",
            "translation:",
            "  entry_points:",
            "    - 0x80001000",
            "",
            "profiles:",
            "  retro-rewind:",
            "    enabled: true",
        };
        lines.AddRange(profileLines);

        var path = Path.Combine(root, "recomp.yml");
        File.WriteAllLines(path, lines);
        return path;
    }

    private static string CreateTempRoot()
    {
        var root = Path.Combine(Path.GetTempPath(), $"translator-riivolution-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        return root;
    }
}
