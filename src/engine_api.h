// Cast engine API, shared by the Winamp DSP part and the AIMP plugin part of aimp_cast.dll.
// Exported too, so tools/dsptest.exe can drive the engine without AIMP.
#pragma once
#include <windows.h>

struct AimpCastDevice { char id[160]; char name[128]; char model[64]; };

extern "C" {
__declspec(dllexport) int AimpCast_ApiVersion();
__declspec(dllexport) void AimpCast_Attach();   // engine is reference counted: Attach/Detach and DSP Init/Quit
__declspec(dllexport) void AimpCast_Detach();
__declspec(dllexport) void AimpCast_Rescan();
__declspec(dllexport) void AimpCast_StopCast();
__declspec(dllexport) int AimpCast_CastTo(const char* id);
__declspec(dllexport) int AimpCast_GetDevices(AimpCastDevice* out, int max);
__declspec(dllexport) int AimpCast_GetState(char* curId, int n);  // bit 0: casting, bit 1: DSP receives audio
__declspec(dllexport) void AimpCast_SetVolume(float level);       // AIMP volume 0..1, mirrored on the speaker
__declspec(dllexport) float AimpCast_GetSpeakerVolume();          // -1 when not casting or unknown
__declspec(dllexport) void* AimpCast_DspHeader(HWND);             // winampDSPHeader*, for aimp_cast_dsp.dll
__declspec(dllexport) int AimpCast_MuteLocal(int set);             // set: 0/1, or -1 to only read; returns current
}
