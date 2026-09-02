// See audio.h. Mirrors AudioManager/Sound from the Windows integration; the
// cached per-sound and listener state exists so the engine can be torn down
// and rebuilt after a device change without the Lua side noticing.
#include "audio.h"
#include "cai.h"
#include "miniaudio.h"
#include <atomic>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace {

// After a rebuild fails, how many more times to try, and how many frames
// (Update calls) to wait between tries: about two seconds at 60 frames per
// second, enough for an output device to finish appearing.
constexpr int kRebuildAttempts = 5;
constexpr int kRebuildRetryFrames = 120;

ma_attenuation_model ToMa(audio::AttenuationModel m) {
    switch (m) {
    case audio::AttenuationModel::None: return ma_attenuation_model_none;
    case audio::AttenuationModel::Inverse: return ma_attenuation_model_inverse;
    case audio::AttenuationModel::Linear: return ma_attenuation_model_linear;
    case audio::AttenuationModel::Exponential: return ma_attenuation_model_exponential;
    }
    return ma_attenuation_model_inverse;
}

class Sound {
public:
    explicit Sound(std::string path) : mPath(std::move(path)) {}
    ~Sound() { Shutdown(); }
    Sound(const Sound&) = delete;
    Sound& operator=(const Sound&) = delete;

    bool Initialize(ma_engine* engine) {
        ma_result r = ma_sound_init_from_file(engine, mPath.c_str(), 0, nullptr, nullptr, &mSound);
        mInitialized = r == MA_SUCCESS;
        if (!mInitialized) Log("audio: failed to load %s (%s)", mPath.c_str(), ma_result_description(r));
        RestoreState();
        return mInitialized;
    }
    void Shutdown() {
        if (!mInitialized) return;
        mWasPlaying = IsPlaying();
        ma_sound_uninit(&mSound);
        mInitialized = false;
    }
    bool Ready() const { return mInitialized; }

    void Play() { ma_result r = ma_sound_start(&mSound); LogDebug("audio: play %s -> %s", mPath.c_str(), ma_result_description(r)); }
    void Pause() { ma_sound_stop(&mSound); }
    void Stop() { ma_sound_stop(&mSound); ma_sound_seek_to_pcm_frame(&mSound, 0); }

    void SetLooping(bool b) { mLooping = b; ma_sound_set_looping(&mSound, b ? MA_TRUE : MA_FALSE); }
    bool IsLooping() const { return ma_sound_is_looping(&mSound) == MA_TRUE; }
    void SetVolume(float v) { mVolume = v; ma_sound_set_volume(&mSound, v); }
    float GetVolume() const { return ma_sound_get_volume(&mSound); }
    void SetPitch(float p) { mPitch = p; ma_sound_set_pitch(&mSound, p); }
    float GetPitch() const { return ma_sound_get_pitch(&mSound); }
    void SetPan(float p) { mPan = p; ma_sound_set_pan(&mSound, p); }
    float GetPan() const { return ma_sound_get_pan(&mSound); }
    void SetPosition(float x, float y, float z) { mPx = x; mPy = y; mPz = z; ma_sound_set_position(&mSound, x, y, z); }
    void GetPosition(float& x, float& y, float& z) const { ma_vec3f p = ma_sound_get_position(&mSound); x = p.x; y = p.y; z = p.z; }
    void SetDirection(float x, float y, float z) { mDx = x; mDy = y; mDz = z; ma_sound_set_direction(&mSound, x, y, z); }
    void SetVelocity(float x, float y, float z) { mVx = x; mVy = y; mVz = z; ma_sound_set_velocity(&mSound, x, y, z); }
    void SetSpatializationEnabled(bool b) { mSpatial = b; ma_sound_set_spatialization_enabled(&mSound, b ? MA_TRUE : MA_FALSE); }
    bool IsSpatializationEnabled() const { return ma_sound_is_spatialization_enabled(&mSound) == MA_TRUE; }
    void SetMinDistance(float d) { mMinDist = d; ma_sound_set_min_distance(&mSound, d); }
    void SetMaxDistance(float d) { mMaxDist = d; ma_sound_set_max_distance(&mSound, d); }
    void SetAttenuationModel(audio::AttenuationModel m) { mModel = m; ma_sound_set_attenuation_model(&mSound, ToMa(m)); }
    bool IsPlaying() const { return ma_sound_is_playing(&mSound) == MA_TRUE; }

private:
    void RestoreState() {
        if (!mInitialized) return;
        SetVolume(mVolume); SetPan(mPan); SetPitch(mPitch); SetLooping(mLooping);
        SetPosition(mPx, mPy, mPz); SetDirection(mDx, mDy, mDz); SetVelocity(mVx, mVy, mVz);
        SetMinDistance(mMinDist); SetMaxDistance(mMaxDist); SetAttenuationModel(mModel);
        SetSpatializationEnabled(mSpatial);
        if (mWasPlaying) Play();
    }

    std::string mPath;
    bool mInitialized = false;
    ma_sound mSound{};
    float mVolume = 1.0f, mPitch = 1.0f, mPan = 0.0f;
    float mPx = 0, mPy = 0, mPz = 0;
    float mDx = 0, mDy = 0, mDz = 1.0f;
    float mVx = 0, mVy = 0, mVz = 0;
    float mMinDist = 1.0f, mMaxDist = std::numeric_limits<float>::max();
    bool mLooping = false, mWasPlaying = false, mSpatial = false;
    audio::AttenuationModel mModel = audio::AttenuationModel::Inverse;
};

struct Engine {
    ma_resource_manager resourceManager{};
    ma_engine engine{};
    bool initialized = false;      // the resource manager is up and Initialize() succeeded once
    bool engineReady = false;      // ma_engine is live; false between a lost device and a successful rebuild
    std::atomic_bool rebuilding{false};
    std::atomic_bool rebuildRequested{false};
    int rebuildFailures = 0;
    int retryCountdown = 0;        // frames until the next rebuild attempt, 0 when none is pending
    ma_device_state lastLoggedState = ma_device_state_uninitialized;
    audio::Handle nextHandle = 1;
    std::unordered_map<audio::Handle, std::unique_ptr<Sound>> sounds;
    // Listener and master state, reapplied after an engine rebuild.
    float volume = 1.0f;
    float px = 0, py = 0, pz = 0;
    float dx = 0, dy = 0, dz = -1.0f;
    float ux = 0, uy = 0, uz = 1.0f;
    float vx = 0, vy = 0, vz = 0;
};
Engine g_engine;
// Lua calls arrive from the game's UI thread and take this mutex. The device
// notification callback runs on miniaudio's thread while ma_engine_uninit may
// be waiting for it under the mutex, so the callback takes no lock and touches
// only the two atomics.
std::recursive_mutex g_mutex;
using Guard = std::lock_guard<std::recursive_mutex>;

void OnDeviceNotification(const ma_device_notification* n) {
    if (g_engine.rebuilding) return;
    switch (n->type) {
    case ma_device_notification_type_started: LogDebug("audio: device started"); break;
    case ma_device_notification_type_stopped: Log("audio: device stopped"); g_engine.rebuildRequested = true; break;
    // miniaudio's CoreAudio backend has already moved the audio unit to the
    // new default device when it posts this; if that restart failed the
    // device is in the stopped state and Update() handles it.
    case ma_device_notification_type_rerouted: Log("audio: device rerouted"); break;
    case ma_device_notification_type_interruption_began: Log("audio: interruption began"); break;
    case ma_device_notification_type_interruption_ended: Log("audio: interruption ended"); g_engine.rebuildRequested = true; break;
    case ma_device_notification_type_unlocked: LogDebug("audio: device unlocked"); break;
    default: LogDebug("audio: device notification %d", (int)n->type); break;
    }
}

void RestoreEngineState() {
    audio::SetMasterVolume(g_engine.volume);
    audio::SetListenerPosition(g_engine.px, g_engine.py, g_engine.pz);
    audio::SetListenerDirection(g_engine.dx, g_engine.dy, g_engine.dz);
    audio::SetListenerUp(g_engine.ux, g_engine.uy, g_engine.uz);
    audio::SetListenerVelocity(g_engine.vx, g_engine.vy, g_engine.vz);
}

bool InitializeEngine() {
    ma_engine_config cfg = ma_engine_config_init();
    cfg.pResourceManager = &g_engine.resourceManager;
    cfg.notificationCallback = OnDeviceNotification;
    ma_result r = ma_engine_init(&cfg, &g_engine.engine);
    if (r != MA_SUCCESS) { Log("audio: engine init failed (%s)", ma_result_description(r)); return false; }
    g_engine.engineReady = true;
    ma_device* dev = ma_engine_get_device(&g_engine.engine);
    Log("audio: engine ready, device \"%s\", %u Hz, %u channels", dev ? dev->playback.name : "?",
        ma_engine_get_sample_rate(&g_engine.engine), ma_engine_get_channels(&g_engine.engine));
    return true;
}

// Give the engine up for good: the sounds stay in the map so their handles
// remain valid for Lua, but every call on them is a no-op from here on.
void GiveUp() {
    for (auto& [h, s] : g_engine.sounds) s->Shutdown();
    if (g_engine.engineReady) { ma_engine_uninit(&g_engine.engine); g_engine.engineReady = false; }
    ma_resource_manager_uninit(&g_engine.resourceManager);
    g_engine.initialized = false;
    g_engine.retryCountdown = 0;
}

// A device problem that a rebuild may fix: schedule one, or give up after
// kRebuildAttempts in a row.
void ScheduleRebuild(const char* why) {
    if (++g_engine.rebuildFailures >= kRebuildAttempts) {
        Log("audio: %s; giving up after %d attempts, sounds are off", why, g_engine.rebuildFailures);
        GiveUp();
        return;
    }
    Log("audio: %s; rebuilding the engine in about two seconds (attempt %d of %d)", why, g_engine.rebuildFailures + 1, kRebuildAttempts);
    g_engine.retryCountdown = kRebuildRetryFrames;
}

// Tear the engine down and build it again with the same sounds. The sounds
// are shut down first, so nothing touches the old engine while it goes away.
void RebuildEngine() {
    g_engine.rebuilding = true;
    for (auto& [h, s] : g_engine.sounds) s->Shutdown();
    if (g_engine.engineReady) { ma_engine_uninit(&g_engine.engine); g_engine.engineReady = false; }
    bool ok = InitializeEngine();
    g_engine.rebuilding = false;
    if (!ok) { ScheduleRebuild("engine rebuild failed"); return; }
    for (auto& [h, s] : g_engine.sounds) if (!s->Initialize(&g_engine.engine)) Log("audio: failed to recreate sound %u", h);
    RestoreEngineState();
    g_engine.rebuildFailures = 0;
    Log("audio: engine rebuilt with %zu sounds", g_engine.sounds.size());
}

Sound* Find(audio::Handle h) {
    auto it = g_engine.sounds.find(h);
    return it == g_engine.sounds.end() ? nullptr : it->second.get();
}

// Run f on the sound behind h if it exists and its engine is live.
template <class F>
void WithSound(audio::Handle h, F f) {
    Guard lock(g_mutex);
    if (Sound* s = Find(h); s && s->Ready()) f(*s);
}
template <class T, class F>
T SoundValue(audio::Handle h, T fallback, F f) {
    Guard lock(g_mutex);
    if (Sound* s = Find(h); s && s->Ready()) return f(*s);
    return fallback;
}
} // namespace

namespace audio {

bool Initialize() {
    Guard lock(g_mutex);
    if (g_engine.initialized) return true;
    ma_resource_manager_config rm = ma_resource_manager_config_init();
    ma_result r = ma_resource_manager_init(&rm, &g_engine.resourceManager);
    if (r != MA_SUCCESS) { Log("audio: resource manager init failed (%s)", ma_result_description(r)); return false; }
    if (!InitializeEngine()) { ma_resource_manager_uninit(&g_engine.resourceManager); return false; }
    g_engine.initialized = true;
    g_engine.rebuildRequested = false;
    g_engine.rebuildFailures = 0;
    RestoreEngineState();
    return true;
}

std::optional<Handle> LoadSound(const std::string& path) {
    Guard lock(g_mutex);
    if (!g_engine.engineReady) return std::nullopt;
    auto s = std::make_unique<Sound>(path);
    if (!s->Initialize(&g_engine.engine)) return std::nullopt;
    Handle h = g_engine.nextHandle++;
    g_engine.sounds.emplace(h, std::move(s));
    LogDebug("audio: loaded %s as handle %u", path.c_str(), h);
    return h;
}

bool DestroySound(Handle h) {
    Guard lock(g_mutex);
    LogDebug("audio: destroy handle %u", h);
    return g_engine.sounds.erase(h) > 0;
}

void Play(Handle h) { WithSound(h, [](Sound& s) { s.Play(); }); }
void Pause(Handle h) { WithSound(h, [](Sound& s) { s.Pause(); }); }
void Stop(Handle h) { WithSound(h, [](Sound& s) { s.Stop(); }); }
void SetVolume(Handle h, float v) { WithSound(h, [=](Sound& s) { s.SetVolume(v); }); }
float GetVolume(Handle h) { return SoundValue(h, 0.0f, [](Sound& s) { return s.GetVolume(); }); }
void SetLooping(Handle h, bool b) { WithSound(h, [=](Sound& s) { s.SetLooping(b); }); }
bool IsLooping(Handle h) { return SoundValue(h, false, [](Sound& s) { return s.IsLooping(); }); }
void SetPitch(Handle h, float p) { WithSound(h, [=](Sound& s) { s.SetPitch(p); }); }
float GetPitch(Handle h) { return SoundValue(h, 0.0f, [](Sound& s) { return s.GetPitch(); }); }
void SetPan(Handle h, float p) { WithSound(h, [=](Sound& s) { s.SetPan(p); }); }
float GetPan(Handle h) { return SoundValue(h, 0.0f, [](Sound& s) { return s.GetPan(); }); }
void SetPosition(Handle h, float x, float y, float z) { WithSound(h, [=](Sound& s) { s.SetPosition(x, y, z); }); }
void GetPosition(Handle h, float& x, float& y, float& z) { x = y = z = 0; WithSound(h, [&](Sound& s) { s.GetPosition(x, y, z); }); }
void SetDirection(Handle h, float x, float y, float z) { WithSound(h, [=](Sound& s) { s.SetDirection(x, y, z); }); }
void SetVelocity(Handle h, float x, float y, float z) { WithSound(h, [=](Sound& s) { s.SetVelocity(x, y, z); }); }
void SetSpatializationEnabled(Handle h, bool b) { WithSound(h, [=](Sound& s) { s.SetSpatializationEnabled(b); }); }
bool IsSpatializationEnabled(Handle h) { return SoundValue(h, false, [](Sound& s) { return s.IsSpatializationEnabled(); }); }
void SetMinDistance(Handle h, float d) { WithSound(h, [=](Sound& s) { s.SetMinDistance(d); }); }
void SetMaxDistance(Handle h, float d) { WithSound(h, [=](Sound& s) { s.SetMaxDistance(d); }); }
void SetAttenuationModel(Handle h, AttenuationModel m) { WithSound(h, [=](Sound& s) { s.SetAttenuationModel(m); }); }
bool IsPlaying(Handle h) { return SoundValue(h, false, [](Sound& s) { return s.IsPlaying(); }); }

void SetMasterVolume(float v) { Guard lock(g_mutex); g_engine.volume = v; if (g_engine.engineReady) ma_engine_set_volume(&g_engine.engine, v); }
float GetMasterVolume() { Guard lock(g_mutex); return g_engine.engineReady ? ma_engine_get_volume(&g_engine.engine) : 0.0f; }
void SetListenerPosition(float x, float y, float z) { Guard lock(g_mutex); g_engine.px = x; g_engine.py = y; g_engine.pz = z; if (g_engine.engineReady) ma_engine_listener_set_position(&g_engine.engine, 0, x, y, z); }
void SetListenerDirection(float x, float y, float z) { Guard lock(g_mutex); g_engine.dx = x; g_engine.dy = y; g_engine.dz = z; if (g_engine.engineReady) ma_engine_listener_set_direction(&g_engine.engine, 0, x, y, z); }
void SetListenerUp(float x, float y, float z) { Guard lock(g_mutex); g_engine.ux = x; g_engine.uy = y; g_engine.uz = z; if (g_engine.engineReady) ma_engine_listener_set_world_up(&g_engine.engine, 0, x, y, z); }
void SetListenerVelocity(float x, float y, float z) { Guard lock(g_mutex); g_engine.vx = x; g_engine.vy = y; g_engine.vz = z; if (g_engine.engineReady) ma_engine_listener_set_velocity(&g_engine.engine, 0, x, y, z); }

void Update() {
    Guard lock(g_mutex);
    if (!g_engine.initialized) return;
    if (g_engine.retryCountdown > 0 && --g_engine.retryCountdown == 0) g_engine.rebuildRequested = true;
    if (g_engine.rebuildRequested.exchange(false)) { RebuildEngine(); return; }
    if (!g_engine.engineReady) return;
    ma_device* dev = ma_engine_get_device(&g_engine.engine);
    ma_device_state state = ma_device_get_state(dev);
    if (state != g_engine.lastLoggedState) { LogDebug("audio: device state %d", (int)state); g_engine.lastLoggedState = state; }
    if (state == ma_device_state_stopped) {
        ma_result r = ma_device_start(dev);
        if (r == MA_SUCCESS) Log("audio: restarted the stopped device");
        else ScheduleRebuild(ma_result_description(r));
    }
}
}
