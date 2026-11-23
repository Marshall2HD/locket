# Build from source

## Requirements

Visual Studio 2026 (or 2022 17.14+) with the *Desktop development with C++* and
*WinUI application development* workloads, and the Windows 11 SDK.

## Build

```powershell
msbuild Locket.sln /t:Restore /p:Configuration=Release /p:Platform=x64
msbuild Locket.sln /p:Configuration=Release /p:Platform=x64
```

Or open `Locket.sln`, set the platform to x64 or ARM64 and press <kbd>F5</kbd>.

## Tests

Core logic has a test runner in `tests/Locket.Core.Tests`.

## Architecture

See [ARCHITECTURE.md](ARCHITECTURE.md) for how the code is organised.
