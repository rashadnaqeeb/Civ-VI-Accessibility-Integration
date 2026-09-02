// CAI macOS speech: a gapless system-voice queue for a game-injected dylib.
//
// Each line is rendered offline through AVSpeechSynthesizer
// (writeUtterance:toBufferCallback:) and played back to back on an
// AVAudioPlayerNode of our own, because AVSpeech's own playback queue leaves
// 120 to 250 ms of silence between utterances. See speech.mm for the design.
//
// Every function is safe to call from any thread. None of them blocks on
// audio or on the synthesizer.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the audio engine, read the installed voices, and pick the voice and
// rate from System Settings > Accessibility > Spoken Content. False if the
// engine cannot start (nothing else fails at init). Idempotent.
bool cai_speech_init(void);

// Stop playback, abandon any render in flight, and release the engine.
void cai_speech_shutdown(void);

// Queue a line. With interrupt, everything queued or playing is dropped first,
// which also happens for empty or whitespace-only text; such text is then
// not queued, so an interrupt with an empty line is a plain stop.
void cai_speech_say(const char* utf8, bool interrupt);

// Drop everything queued or playing.
void cai_speech_stop(void);

// cai_speech_stop for a caller that must not wait, the key monitor on the
// main thread: when another thread holds one of the stream's locks, nothing
// is done and false is returned.
bool cai_speech_try_stop(void);

// True while a line is queued, being rendered, or still playing (including
// the short gap after it).
bool cai_speech_is_speaking(void);

// Speaking rate on AVSpeech's [0, 1] scale (0.5 is AVSpeech's default).
// Applies to lines queued after the call.
void cai_speech_set_rate(float unit0to1);

// Playback volume, 0 to 1. Applies at once, to what is already playing too.
void cai_speech_set_volume(float unit0to1);

// Switch later lines to the voice with this AVSpeech identifier. NULL or an
// empty string returns to AVSpeech's own default voice. False, with the voice
// unchanged, if AVSpeech has no voice with that identifier.
bool cai_speech_set_voice(const char* identifier);

// The installed voices, as read at init. The strings stay valid until shutdown.
int cai_speech_voice_count(void);
const char* cai_speech_voice_id(int i);
const char* cai_speech_voice_name(int i);
const char* cai_speech_voice_language(int i);

// The rate lines are queued with now, on AVSpeech's [0, 1] scale, and the
// identifier of the voice they use (empty for AVSpeech's default), copied
// into the caller's buffer and truncated to fit. After init these are the
// Spoken Content settings.
float cai_speech_get_rate(void);
void cai_speech_get_voice(char* buffer, size_t size);

// Optional: route log lines somewhere other than stderr. Pass NULL for stderr.
// The line is a NUL-terminated UTF-8 string without a trailing newline or
// timestamp; it is only valid for the duration of the call. Called from any
// thread. WARNING lines report a failure or a dropped line; INFO lines trace
// the normal flow of a render.
typedef enum { CAI_SPEECH_LOG_INFO = 0, CAI_SPEECH_LOG_WARNING = 1 } cai_speech_log_level;
typedef void (*cai_speech_log_fn)(cai_speech_log_level level, const char* line);
void cai_speech_set_log(cai_speech_log_fn fn);

#ifdef __cplusplus
}
#endif
