# Third-party notices

AIMP Cast's own code is under the MIT license (see [LICENSE](LICENSE)). It contains no code copied from other projects. This file lists what it builds against, talks to or refers to.

## Not included, needed to build

### AIMP SDK

- Copyright © 2006-2025 Artem Izmaylov, <https://www.aimp.ru/>
- Used: the C++ plugin headers (`apiPlugin.h`, `apiCore.h`, `apiMenu.h`, `apiMessages.h`, `apiActions.h` and the headers they include)
- The SDK headers carry a copyright notice but no license, so they are **not** in this repository. You download them from <https://www.aimp.ru/?do=download&os=windows&cat=sdk>, see [third_party/aimp_sdk/README.md](third_party/aimp_sdk/README.md).
- The AIMP author provides the SDK so that people can write AIMP plugins. Built plugins contain only declarations from these headers (interface IDs and constants), not SDK code.

## Part of the build toolchain or Windows

### Microsoft C/C++ runtime

- The DLLs are built with Visual Studio's `/MT` option, so the C++ standard library and C runtime are linked statically.
- Visual Studio's license terms allow this code to be distributed as part of your own program.

### Windows system libraries

- Used: `ws2_32` (sockets), `secur32` (Schannel TLS), `user32`, `shell32`, `ole32`
- These are part of Windows and are only linked against, not shipped.

## Interfaces reimplemented

### Winamp DSP plugin interface

- AIMP loads Winamp DSP plugins. `src/engine.cpp` declares the `winampDSPHeader` / `winampDSPModule` structures (version `0x20`) so that it is binary compatible.
- The declarations are written for this project. No Winamp SDK file is used or included.
- Winamp is a trademark of its owner.

### Google Cast (Cast v2 protocol)

- `src/cast.cpp` implements the parts of the Cast v2 protocol that are needed: TLS on port 8009, length-prefixed protobuf `CastMessage` frames with a minimal hand-written encoder/decoder, the JSON namespaces for connection, heartbeat, receiver and media, and mDNS discovery of `_googlecast._tcp`.
- No Google SDK or library is used. The stream plays in Google's public Default Media Receiver (app ID `CC1AD845`).
- Google Cast and Chromecast are trademarks of Google LLC. This project is not made, endorsed or certified by Google.

## Trademarks

AIMP is the property of Artem Izmaylov. Google Cast, Chromecast and Nest are trademarks of Google LLC. They are named only to describe what this software works with.
