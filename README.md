# AIMP Cast

Play [AIMP](https://www.aimp.ru/) on Google Cast speakers (Chromecast, Nest and other Cast-enabled speakers) on your home network.

AIMP Cast streams exactly what AIMP plays, **after** AIMP's DSP effects, equalizer and crossfade. It is a live stream, so FLAC, MP3 and every other format AIMP can play work the same way.

- A **Play on** submenu in AIMP's main menu, the tray menu and the Stop-button menu: pick a speaker or **This PC (local)**
- AIMP's volume slider and the speaker's volume follow each other in both directions
- Optionally mutes the PC while casting
- Finds speakers automatically (mDNS), with no cloud service and no account
- One plugin folder with no dependencies, x64

Tested with AIMP 5.40 (x64) on Windows 11.

## Install

1. Close AIMP.
2. Copy both folders to `C:\Program Files\AIMP\Plugins\`, so you get `Plugins\aimp_cast\aimp_cast.dll` and `Plugins\aimp_cast_dsp\aimp_cast_dsp.dll`.
3. Start AIMP. If it asks, enable **AIMP Cast** in the plugin manager.
4. Open AIMP's DSP manager, go to the Winamp DSP plugins, and select **AIMP Cast - stream to Google Cast** (`aimp_cast_dsp.dll`). This is the part that receives the audio.
5. Open the main menu (or the tray menu, or right-click Stop), choose **Play on** and pick your speaker.

The menu shows a hint if the DSP part is not active yet.

## How it works

```
AIMP ──► Winamp DSP (aimp_cast_dsp.dll) ──► HTTP server on the PC ──► Cast speaker
             16-bit PCM                     endless WAV, port 45710     Default Media Receiver
             (after AIMP's effects)
```

`aimp_cast.dll` is the AIMP plugin (menu, volume sync) and holds the engine. `aimp_cast_dsp.dll` is a small Winamp DSP entry point that forwards to `Plugins\aimp_cast\aimp_cast.dll`. AIMP does not start the plugin part of a DLL that also exports the Winamp DSP entry point, so the two have to be separate files. Both end up in one module instance, so there is a single engine. The engine:

- finds speakers with mDNS (`_googlecast._tcp`)
- talks to the speaker over the Cast v2 protocol (TLS on port 8009, protobuf) to launch the Default Media Receiver and load the stream
- serves the PCM as an endless WAV over HTTP, and sends a few seconds of buffered audio first so playback starts without re-buffering

Because the speaker buffers, it plays a few seconds behind AIMP.

## Settings

`%APPDATA%\AIMP\aimp_cast\aimp_cast.ini`, section `[Cast]`:

| Key | Default | Meaning |
| --- | --- | --- |
| `MuteLocal` | `1` | Mute the PC while casting (also in the menu) |
| `StartBufferMs` | `4000` | Audio sent at once when the speaker connects |
| `LastDevice` | | Last speaker used |

A log is written to `%APPDATA%\AIMP\aimp_cast\aimp_cast.log`.

Windows Firewall must allow AIMP to accept connections on the private network (port 45710 or the next free port). Windows asks the first time.

## Build

Requirements: Visual Studio 2022 or later with the C++ x64 build tools.

1. Download the AIMP SDK from <https://www.aimp.ru/?do=download&os=windows&cat=sdk> and copy its C++ headers (`Sources\Cpp\*.h`) to `third_party\aimp_sdk\`. The SDK is not included in this repository.
2. Run `build.cmd`. Output: `build\aimp_cast.dll` and `build\aimp_cast_dsp.dll`, plus two test tools.

Test tools (in `tools/`):

- `castctl discover` lists Cast devices. `castctl play <ip> <port> <url>` plays a URL on a device.
- `dsptest <dll> <seconds>` loads the DLL like AIMP's DSP manager would and feeds it a quiet 440 Hz test tone. `dsptest <dll> api` exercises the engine API without audio.

## License

MIT, see [LICENSE](LICENSE). Third-party parts and trademarks are listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). AIMP and the AIMP SDK are © Artem Izmaylov. Google Cast is a trademark of Google LLC. This project is not affiliated with either.
