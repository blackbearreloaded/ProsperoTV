# PS5 dependency snapshot

This directory contains the PS5 headers, static archives, and import stubs used
by the validated PSRadio build. They are checked in so a repository clone uses
the same application-facing dependency set.

- SDL2 headers and `libSDL2.a` provide the PS5 window, software renderer, and
  synchronization primitives. The SDL license is retained in
  `sdl/include/SDL2/SDL_copying.h`.
- RmlUi 6.2 headers and `librmlui.a` provide document parsing, layout, and
  rendering. Lua bindings are disabled. Its MIT license is retained in
  `rmlui/LICENSE.txt`.
- FreeType 2.13.2 is retained as a static link dependency of the RmlUi build;
  its license is in `freetype/LICENSE.txt`.
- C++ runtime archives, unwind support, and public libc/kernel import stubs come
  from the open-source PS5 Payload SDK toolchain.

The compiler and PS5 target support still come from the SDK installed at
`/opt/ps5-payload-sdk` inside WSL. Do not add proprietary Sony SDK files,
firmware modules, extracted game libraries, keys, or credentials here.
