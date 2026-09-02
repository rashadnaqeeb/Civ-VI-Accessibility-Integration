// Sound engine for the CAI macOS dylib: a port of audioManager.h/sound.h from
// the Windows integration (extern/civ6-accessibility-lua-integration) onto
// miniaudio's CoreAudio backend. The handle, volume, pitch, pan, position,
// looping, spatialization and listener API is exactly what the Lua audio
// manager (src/UI/shared/audioManager_CAI.lua) expects. Every function is
// safe to call before Initialize() and after the engine has been lost.
#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace audio {
using Handle = uint32_t;

// The values Lua passes to SetSoundAttenuationModel, in this order.
enum class AttenuationModel { None, Inverse, Linear, Exponential };

// Start the engine. True when it runs, also when it was already running.
bool Initialize();

std::optional<Handle> LoadSound(const std::string& filePath);
bool DestroySound(Handle h);

void Play(Handle h);
void Pause(Handle h);
void Stop(Handle h);

void SetVolume(Handle h, float volume);
float GetVolume(Handle h);
void SetLooping(Handle h, bool looping);
bool IsLooping(Handle h);
void SetPitch(Handle h, float pitch);
float GetPitch(Handle h);
void SetPan(Handle h, float pan);
float GetPan(Handle h);
void SetPosition(Handle h, float x, float y, float z);
void GetPosition(Handle h, float& x, float& y, float& z);
void SetDirection(Handle h, float x, float y, float z);
void SetVelocity(Handle h, float x, float y, float z);
void SetSpatializationEnabled(Handle h, bool enabled);
bool IsSpatializationEnabled(Handle h);
void SetMinDistance(Handle h, float d);
void SetMaxDistance(Handle h, float d);
void SetAttenuationModel(Handle h, AttenuationModel m);
bool IsPlaying(Handle h);

void SetMasterVolume(float volume);
float GetMasterVolume();
void SetListenerPosition(float x, float y, float z);
void SetListenerDirection(float x, float y, float z);
void SetListenerUp(float x, float y, float z);
void SetListenerVelocity(float x, float y, float z);

// Called once per frame from Lua (CAI.AudioUpdate): restarts a stopped device
// and rebuilds the engine when the device went away.
void Update();
}
