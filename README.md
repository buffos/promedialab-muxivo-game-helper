# Muxivo Windows Game Capture helper

This repository contains the Windows helper that acquires frames from a selected
game for Muxivo. Muxivo owns the picker, live preview, Program composition and
operator controls. The bundled helper starts on demand and stops with Muxivo.
The game starts normally and is selected from the Muxivo picker.

| Graphics API | Game process architecture |
| --- | --- |
| Direct3D 9 | x64 and x86 on 64-bit Windows |
| Direct3D 11 | x64 |
| Direct3D 12 | x64 |
| OpenGL (WGL) | x64 and x86 on 64-bit Windows |
| Vulkan | x64 |

The graphics API, game, driver and presentation mode determine compatibility.
Exclusive fullscreen was verified with specific games and Windows
configurations. Muxivo also has a separate capture path for game windows.

## Build on Windows

Install Visual Studio 2022 with **Desktop development with C++**, the Windows
SDK, CMake 3.20 or newer, CTest and PowerShell. Put CMake and CTest on PATH.
Run from the checkout:

```powershell
.\scripts\build-adapters.ps1
.\scripts\build-adapters.ps1 -Test
```

The script verifies the public dependency downloads against pinned SHA-256
digests. It stages the x64 host, its capture DLLs, the Vulkan layer manifest
and the x86 compatibility components under `build/standalone/package`.
`-OutputRoot` selects a different build directory. Use an interactive Windows
desktop with graphics support for the CTest cases that create windows.
The Muxivo application and its source are unnecessary for this build.

## Capture lifecycle and data

Muxivo discovers eligible renderers and sends the selected process and window
identity to the local helper. For supported APIs, API-specific capture modules
read the selected renderer. Vulkan uses a Vulkan layer. The host command
channel has the version identifier `MXGC/3`. A separate local named-pipe stream
sends framed pixels and frame metadata to Muxivo. Shared Windows objects carry
capture-session configuration. Muxivo releases the session when the operator
stops capture or the game exits.

The helper handles pixels, frame dimensions and format, target and session
identifiers, and capture diagnostics. It has no network frame transport or
telemetry and does not write captured frames to disk. Muxivo handles recording
and broadcasting separately. This checkout includes diagnostic tests that use
synthetic targets.

## Release provenance

This is the MIT-licensed source repository. The source tag identifies the
sources for review. The Windows installer needs a separately built and
Authenticode-signed package. Its release record identifies the source tag
and commit, signing identity, and SHA-256 digests for the delivered files.

See [LICENSE](LICENSE) and [third-party notices](THIRD_PARTY_NOTICES.md).
