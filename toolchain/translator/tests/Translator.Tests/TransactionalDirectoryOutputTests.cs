using Translator.Core.IO;
using Xunit;

namespace Translator.Tests;

public sealed class TransactionalDirectoryOutputTests
{
    [Fact]
    public void SuccessfulGenerationPreservesUnchangedTimestampsAndReconcilesMembership()
    {
        var root = CreateTempRoot();
        try
        {
            var output = Path.Combine(root, "generated-mod");
            Directory.CreateDirectory(Path.Combine(output, "cpp"));
            var unchangedPath = Path.Combine(output, "cpp", "unchanged.cpp");
            File.WriteAllText(unchangedPath, "same");
            File.WriteAllText(Path.Combine(output, "cpp", "changed.cpp"), "old");
            File.WriteAllText(Path.Combine(output, "cpp", "removed.cpp"), "stale");
            var originalTimestamp = new DateTime(2001, 2, 3, 4, 5, 6, DateTimeKind.Utc);
            File.SetLastWriteTimeUtc(unchangedPath, originalTimestamp);

            var outcome = TransactionalDirectoryOutput.GenerateAndPublish(
                output,
                staging =>
                {
                    Assert.Empty(Directory.EnumerateFileSystemEntries(staging));
                    Directory.CreateDirectory(Path.Combine(staging, "cpp"));
                    File.WriteAllText(Path.Combine(staging, "cpp", "unchanged.cpp"), "same");
                    File.WriteAllText(Path.Combine(staging, "cpp", "changed.cpp"), "new");
                    File.WriteAllText(Path.Combine(staging, "cpp", "added.cpp"), "added");
                    return 0;
                });

            Assert.Equal(0, outcome.ExitCode);
            Assert.Empty(outcome.Warnings);
            var result = Assert.IsType<TransactionalDirectoryOutput.PublishResult>(outcome.Publication);
            Assert.Equal(1, result.AddedFiles);
            Assert.Equal(1, result.UpdatedFiles);
            Assert.Equal(1, result.RemovedFiles);
            Assert.Equal(1, result.UnchangedFiles);
            Assert.Equal(originalTimestamp, File.GetLastWriteTimeUtc(Path.Combine(output, "cpp", "unchanged.cpp")));
            Assert.Equal("new", File.ReadAllText(Path.Combine(output, "cpp", "changed.cpp")));
            Assert.Equal("added", File.ReadAllText(Path.Combine(output, "cpp", "added.cpp")));
            Assert.False(File.Exists(Path.Combine(output, "cpp", "removed.cpp")));
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void FailedGenerationLeavesLastKnownGoodOutputUntouched()
    {
        var root = CreateTempRoot();
        try
        {
            var output = Path.Combine(root, "generated-mod");
            Directory.CreateDirectory(output);
            var livePath = Path.Combine(output, "mod_manifest.json");
            File.WriteAllText(livePath, "last-known-good");
            var originalTimestamp = new DateTime(2002, 3, 4, 5, 6, 7, DateTimeKind.Utc);
            File.SetLastWriteTimeUtc(livePath, originalTimestamp);

            var outcome = TransactionalDirectoryOutput.GenerateAndPublish(
                output,
                staging =>
                {
                    File.WriteAllText(Path.Combine(staging, "mod_manifest.json"), "incomplete");
                    File.WriteAllText(Path.Combine(staging, "stale-discovery.cpp"), "must not publish");
                    return 3;
                });

            Assert.Equal(3, outcome.ExitCode);
            Assert.Null(outcome.Publication);
            Assert.Equal("last-known-good", File.ReadAllText(livePath));
            Assert.Equal(originalTimestamp, File.GetLastWriteTimeUtc(livePath));
            Assert.False(File.Exists(Path.Combine(output, "stale-discovery.cpp")));
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void CleanupFailureDoesNotMaskProducerFailure()
    {
        var root = CreateTempRoot();
        FileStream? lockedStagingFile = null;
        try
        {
            var output = Path.Combine(root, "generated-mod");
            var error = Assert.Throws<InvalidOperationException>(() =>
                TransactionalDirectoryOutput.GenerateAndPublish(
                    output,
                    staging =>
                    {
                        lockedStagingFile = new FileStream(
                            Path.Combine(staging, "locked.cpp"),
                            FileMode.Create,
                            FileAccess.ReadWrite,
                            FileShare.None);
                        throw new InvalidOperationException("producer failure");
                    }));

            Assert.Equal("producer failure", error.Message);
            Assert.False(Directory.Exists(output));
        }
        finally
        {
            lockedStagingFile?.Dispose();
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void FailedGenerationExposesStagingDiagnosticsBeforeCleanup()
    {
        var root = CreateTempRoot();
        try
        {
            var output = Path.Combine(root, "generated-mod");
            string? capturedReport = null;
            var capturedExitCode = 0;

            var outcome = TransactionalDirectoryOutput.GenerateAndPublish(
                output,
                staging =>
                {
                    File.WriteAllText(Path.Combine(staging, "runtime_patch_slots.txt"), "two executable writes");
                    return 2;
                },
                (staging, producerExitCode) =>
                {
                    capturedExitCode = producerExitCode;
                    capturedReport = File.ReadAllText(Path.Combine(staging, "runtime_patch_slots.txt"));
                });

            Assert.Equal(2, outcome.ExitCode);
            Assert.Null(outcome.Publication);
            Assert.Equal(2, capturedExitCode);
            Assert.Equal("two executable writes", capturedReport);
            Assert.False(Directory.Exists(output));
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    private static string CreateTempRoot()
    {
        var root = Path.Combine(Path.GetTempPath(), $"translator-transactional-output-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        return root;
    }
}
