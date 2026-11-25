# Third-party notices

- [Microsoft Detours](https://github.com/microsoft/Detours/tree/1dbd4ea281d73d478aa8d09a8b83fbd265f715f7)
  is built from the pinned commit under the MIT License. The build package
  includes the original Detours license.
- The pinned [sdl3-src 3.4.20 crate](https://crates.io/crates/sdl3-src/3.4.20)
  supplies the Khronos headers from the SDL source distribution. SDL is
  distributed under the zlib license. The Vulkan and Vulkan Video headers
  carry their original Khronos copyright and SPDX notices, including
  Apache-2.0. The build compiles against these headers and links no SDL library.
- The Windows SDK and Visual Studio are build prerequisites from Microsoft.
  Their SDK files are not redistributed in this repository or build package.
