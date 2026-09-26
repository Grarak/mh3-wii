using System.Runtime.CompilerServices;
using Translator.Core.Analysis;
using Translator.Core.Loading;

namespace Translator.Tests;

/// <summary>
/// Installs the MKWii PAL save/restore thunk address ranges once for the whole test assembly,
/// since several suites translate hand-assembled fragments that call these thunks by address.
/// </summary>
internal static class GuestSaveRestoreThunkTestDefaults
{
    [ModuleInitializer]
    internal static void Install()
    {
        GuestSaveRestoreThunks.Current = GuestSaveRestoreThunks.FromFunctionMap(FunctionMap.Parse(
            [
                "800214f8 _save_fpr_23",
                "8002150c _save_fpr_28",
                "80021544 _rest_fpr_23",
                "80021558 _rest_fpr_28",
                "8002156c _save_gpr_14",
                "800215a0 _save_gpr_27",
                "800215b8 _rest_gpr_14",
                "800215ec _rest_gpr_27"
            ],
            "mkwii-pal-test-defaults"));
    }
}
