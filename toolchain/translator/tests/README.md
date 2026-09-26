# Translator tests

`Translator.Tests` is the default binary-free suite. It covers the decoder, IR/SSA, code
generation helpers, parsers with synthetic inputs, project-manifest loading, DOL-only image
construction, and generic entry-point translation.

The older asset-backed and host-C++-compiler-backed test sources are intentionally retained in
`Translator.Tests` but excluded in its project file. They mixed MKWii fixtures, several compiler
selection strategies, and private-method reflection checks, which made the normal suite both slow
and red on a clean Windows setup. They should become a separate opt-in integration project when
that work is useful again.

An external generic DOL can be included without adding it to the repository:

```powershell
$env:RECOMP_GENERIC_DOL = 'D:\path\to\main.dol'
dotnet test translator/Translator.sln -c Release
```

The test treats the file as a generic DOL: it reads the DOL entry point, builds a DOL-only image,
and translates that entry point without game-specific addresses or bindings.
