using Translator.Cli.Configuration;
using Translator.Core.Loading;
using Translator.Core.Parsing.Dol;
using Translator.Core.Translation;
using Xunit;

namespace Translator.Tests;

public sealed class GenericProjectTests
{
    [Fact]
    public void FunctionMapIsPathOnlyWhileBinaryHashesRemainEnforced()
    {
        var temp = CreateTempDirectory();
        try
        {
            File.WriteAllBytes(Path.Combine(temp, "main.dol"), new byte[0x100]);
            var mapPath = Path.Combine(temp, "MAP.txt");
            File.WriteAllText(mapPath, "80001000 entry\n");
            var projectPath = Path.Combine(temp, "recomp.yml");
            const string manifest = """
                schema_version: 1
                workspace_root: .
                project:
                  id: map-config-test
                inputs:
                  dol:
                    path: main.dol
                """;

            File.WriteAllText(
                projectPath,
                manifest + Environment.NewLine + """
                translation:
                  function_map:
                    path: MAP.txt
                """);

            var project = TranslationProjectConfig.Load(projectPath);
            Assert.Equal(mapPath, project.Translation.FunctionMapPath);
            Assert.Equal(0x80001000u, Assert.Single(FunctionMap.Load(project.Translation.FunctionMapPath!).Addresses));

            File.WriteAllText(
                projectPath,
                manifest + Environment.NewLine + """
                translation:
                  function_map:
                    path: MAP.txt
                    sha256: deliberately-rejected-legacy-key
                """);

            // The function map binds by path alone; the legacy sha256 key is an
            // unknown property and rejects the whole manifest.
            var mapHashError = Assert.ThrowsAny<Exception>(() => TranslationProjectConfig.Load(projectPath));
            Assert.Contains("sha256", mapHashError.Message, StringComparison.Ordinal);

            File.WriteAllText(
                projectPath,
                manifest.Replace(
                    "    path: main.dol",
                    "    path: main.dol\n    sha256: deliberately-wrong",
                    StringComparison.Ordinal));

            var dolError = Assert.Throws<InvalidDataException>(() => TranslationProjectConfig.Load(projectPath));

            Assert.Contains("Configured DOL SHA-256 does not match", dolError.Message, StringComparison.Ordinal);

            File.WriteAllText(
                projectPath,
                manifest + Environment.NewLine + """
                translation:
                  function_map:
                    sha256: rejected-and-not-a-path
                """);

            var mapError = Assert.ThrowsAny<Exception>(() => TranslationProjectConfig.Load(projectPath));

            Assert.Contains("sha256", mapError.Message, StringComparison.Ordinal);
        }
        finally
        {
            Directory.Delete(temp, recursive: true);
        }
    }

    [Fact]
    public void DolOnlyProjectLoadsAndTranslatesItsConfiguredEntryPoint()
    {
        var temp = CreateTempDirectory();
        try
        {
            const uint entry = 0x80001000u;
            var dolPath = Path.Combine(temp, "main.dol");
            File.WriteAllBytes(
                dolPath,
                SyntheticDolFactory.CreateBytes(
                    entry,
                    sections: [SyntheticDolFactory.Text(0, entry, 0x38630001u, 0x4E800020u)]));
            var projectPath = Path.Combine(temp, "recomp.yml");
            File.WriteAllText(
                projectPath,
                """
                schema_version: 1
                project:
                  id: synthetic-dol
                  display_name: Synthetic DOL
                inputs:
                  dol:
                    path: main.dol
                translation:
                  entry_points: [0x80001000]
                  allow_unsupported_instructions: true
                output:
                  root: out
                """);

            var project = TranslationProjectConfig.Load(projectPath);
            Assert.Null(project.Inputs.Rel);
            Assert.Equal(entry, Assert.Single(project.Translation.EntryPoints));

            var dol = DolFile.Load(project.Inputs.Dol.Path);
            var image = new ProgramImageBuilder().Build(dol, ramBase: project.Memory.Base, ramSize: project.Memory.Size);
            var result = new FunctionTranslator(image).Translate(
                entry,
                new TranslationOptions(AllowUnsupportedInstructions: true));

            Assert.Equal(2, result.Metrics.PpcInstructionCount);
            Assert.Contains("func_80001000", result.CxxCode, StringComparison.Ordinal);
        }
        finally
        {
            Directory.Delete(temp, recursive: true);
        }
    }

    [Fact]
    public void ExternalGenericDolParsesAndTranslatesWhenConfigured()
    {
        var path = Environment.GetEnvironmentVariable("RECOMP_GENERIC_DOL");
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
        {
            return;
        }

        var dol = DolFile.Load(path);
        var image = new ProgramImageBuilder().Build(dol);
        var result = new FunctionTranslator(image).Translate(
            dol.EntryPoint,
            new TranslationOptions(
                MaxInstructions: 2048,
                MaxBytes: 0x10000,
                AllowUnsupportedInstructions: true));

        Assert.NotEmpty(dol.ExecutableSections);
        Assert.NotEmpty(result.CxxCode);
        Assert.True(result.Metrics.PpcInstructionCount > 0);
    }

    private static string CreateTempDirectory()
    {
        var path = Path.Combine(Path.GetTempPath(), "generic_recomp_project", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(path);
        return path;
    }
}
