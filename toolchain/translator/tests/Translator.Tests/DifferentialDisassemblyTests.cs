using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text.RegularExpressions;
using Translator.Core.Disassembly;
using Xunit;

namespace Translator.Tests;

public class DifferentialDisassemblyTests
{
    [Theory]
    [InlineData(0x7C032800u, 0x80000000u, "cmpw")]
    [InlineData(0x41820010u, 0x80000000u, "beq")]
    [InlineData(0x3C608038u, 0x80000000u, "lis")]
    [InlineData(0x6063F780u, 0x80000004u, "ori")]
    [InlineData(0x7C0903A6u, 0x80000000u, "mtctr")]
    public void DecoderMnemonicMatchesDolphinOracle(uint opcode, uint address, string expectedMnemonic)
    {
        if (!LlvmDisassemblerOracle.IsAvailable())
        {
            return;
        }

        var oracle = LlvmDisassemblerOracle.Disassemble(opcode, address);
        var decoded = PpcDecoder.Decode(address, opcode);

        Assert.Equal(expectedMnemonic, decoded.Mnemonic);
        Assert.Equal(expectedMnemonic, NormalizeMnemonic(oracle));
    }

    private static string NormalizeMnemonic(string disassembly)
    {
        foreach (var line in disassembly.Split(new[] { '\r', '\n' }, StringSplitOptions.RemoveEmptyEntries))
        {
            var match = Regex.Match(line.ToLowerInvariant(), @"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2}\s+){4}([a-z0-9_.]+)");
            if (match.Success)
            {
                return match.Groups[1].Value;
            }
        }

        throw new Xunit.Sdk.XunitException($"Failed to parse oracle disassembly '{disassembly}'.");
    }

    private static class LlvmDisassemblerOracle
    {
        public static bool IsAvailable()
        {
            try
            {
                if (OperatingSystem.IsWindows())
                {
                    var output = Run("wsl.exe", "bash -lc 'command -v llvm-mc-18 >/dev/null && command -v llvm-objdump-18 >/dev/null && echo ok'");
                    return output.Contains("ok", StringComparison.OrdinalIgnoreCase);
                }

                return !string.IsNullOrWhiteSpace(Run("bash", "-lc 'command -v llvm-mc-18 && command -v llvm-objdump-18'"));
            }
            catch
            {
                return false;
            }
        }

        public static string Disassemble(uint opcode, uint address)
        {
            var tempDir = Path.Combine(Path.GetTempPath(), "mkw_llvm_disasm_oracle");
            Directory.CreateDirectory(tempDir);
            var asmPath = Path.Combine(tempDir, "oracle.s");
            var objPath = Path.Combine(tempDir, "oracle.o");
            File.WriteAllText(
                asmPath,
                $".text{Environment.NewLine}.long 0x{opcode.ToString("X8", CultureInfo.InvariantCulture)}{Environment.NewLine}");

            if (OperatingSystem.IsWindows())
            {
                var asmWsl = ToWslPath(asmPath);
                var objWsl = ToWslPath(objPath);
                RunWsl($"/usr/bin/llvm-mc-18 -triple=powerpc-unknown-unknown -filetype=obj -o '{objWsl}' '{asmWsl}'");
                return RunWsl(
                    $"/usr/bin/llvm-objdump-18 --triple=powerpc-unknown-unknown --adjust-vma=0x{address.ToString("X", CultureInfo.InvariantCulture)} -d '{objWsl}'");
            }

            Run("llvm-mc-18", $"-triple=powerpc-unknown-unknown -filetype=obj -o \"{objPath}\" \"{asmPath}\"");
            return Run(
                "llvm-objdump-18",
                $"--triple=powerpc-unknown-unknown --adjust-vma=0x{address.ToString("X", CultureInfo.InvariantCulture)} -d \"{objPath}\"");
        }

        private static string Run(string fileName, string arguments)
        {
            var psi = new ProcessStartInfo
            {
                FileName = fileName,
                Arguments = arguments,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false
            };

            using var process = Process.Start(psi)!;
            process.WaitForExit(10000);
            var stdout = process.StandardOutput.ReadToEnd();
            var stderr = process.StandardError.ReadToEnd();
            Assert.True(process.ExitCode == 0, $"{fileName} {arguments}\nstdout:\n{stdout}\nstderr:\n{stderr}");
            return stdout + stderr;
        }

        private static string RunWsl(string command)
        {
            var escaped = command.Replace("'", "'\"'\"'");
            return Run("wsl.exe", $"bash -lc '{escaped}'");
        }

        private static string ToWslPath(string windowsPath)
        {
            var psi = new ProcessStartInfo
            {
                FileName = "wsl.exe",
                Arguments = $"wslpath -a \"{windowsPath}\"",
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false
            };

            using var process = Process.Start(psi)!;
            process.WaitForExit(10000);
            var stdout = process.StandardOutput.ReadToEnd().Trim();
            var stderr = process.StandardError.ReadToEnd();
            Assert.True(process.ExitCode == 0, $"wslpath failed for '{windowsPath}': {stderr}");
            Assert.False(string.IsNullOrWhiteSpace(stdout), $"wslpath returned empty output for '{windowsPath}'.");
            return stdout;
        }
    }
}
