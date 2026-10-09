using Translator.Core;
using Translator.Core.CodeGen;
using Xunit;

namespace Translator.Tests;

public sealed class RuntimeNativeIndexTests
{
    [Fact]
    public void BuildCapturesEveryRuntimeRegistrationKind()
    {
        var directory = Path.Combine(Path.GetTempPath(), $"mkw-native-kinds-{Guid.NewGuid():N}");
        Directory.CreateDirectory(directory);
        try
        {
            File.WriteAllText(Path.Combine(directory, "registrations.cpp"), """
                REGISTER_NATIVE_FUNCTION(0x80000010, Direct);
                REGISTER_NATIVE_FUNCTION_AS(0x80000020, Aliased, "alias");
                REGISTER_TRANSLATED_FUNCTION(0x80000030, Translated);
                PPC_NATIVE_OVERRIDE_VOID(80000040, Stub, (void), ());
                GX_FATAL_STUB(80000050, "Fatal")
                // REGISTER_NATIVE_FUNCTION(0x80000060, CommentedOut);
                """);

            var registrations = RuntimeNativeIndexBuilder.Build(directory).Registrations;
            Assert.Equal(5, registrations.Length);
            Assert.Contains(registrations, static entry =>
                entry.Address == 0x80000010u && entry.ExcludesBaseTranslation);
            Assert.Contains(registrations, static entry =>
                entry.Address == 0x80000020u && !entry.ExcludesBaseTranslation);
            Assert.Contains(registrations, static entry =>
                entry.Address == 0x80000030u && entry.IsTranslatedOverride);
            Assert.Contains(registrations, static entry =>
                entry.Address == 0x80000040u && entry.Symbol == "Stub");
            Assert.Contains(registrations, static entry =>
                entry.Address == 0x80000050u && entry.Symbol == "GX_FATAL_STUB_80000050");
            Assert.DoesNotContain(registrations, static entry => entry.Address == 0x80000060u);
        }
        finally
        {
            if (Directory.Exists(directory))
                Directory.Delete(directory, recursive: true);
        }
    }

    [Fact]
    public void BuildSharesTypedAbiAndEffectDataWithoutCreatingSidecarFiles()
    {
        var directory = Path.Combine(Path.GetTempPath(), $"mkw-native-index-{Guid.NewGuid():N}");
        Directory.CreateDirectory(directory);
        var sourcePath = Path.Combine(directory, "fixture.cpp");
        try
        {
            File.WriteAllText(sourcePath, """
                extern "C" void Typed(float value, uint32_t count) { (void)value; (void)count; }
                PPC_NATIVE_OVERRIDE_VOID(80001234, Typed, (float value, uint32_t count), (value, count));
                """);

            var index = RuntimeNativeIndexBuilder.Build(directory);
            Assert.Single(index.Registrations);
            Assert.Single(index.VoidStubAbis);
            Assert.Single(index.Effects);
            Assert.True(index.ToGuestEffectSet().Contracts.ContainsKey(0x80001234u));

            var provider = RuntimeNativeFunctionAbiProvider.FromIndex(
                index, new HashSet<uint> { 0x80001234u });
            Assert.True(provider.TryGetGuestFunctionAbi("func_80001234", out var abi));
            Assert.Contains("f1", abi.ArgumentRegisters);
            Assert.Contains("r3", abi.ArgumentRegisters);
            Assert.Contains("f1", abi.ScalarFloatArgumentRegisters);

            var outOfScope = RuntimeNativeFunctionAbiProvider.FromIndex(index, new HashSet<uint>());
            Assert.False(outOfScope.TryGetGuestFunctionAbi("func_80001234", out _));

            Assert.Equal([sourcePath], Directory.GetFiles(directory));
        }
        finally
        {
            if (Directory.Exists(directory))
                Directory.Delete(directory, recursive: true);
        }
    }
}
