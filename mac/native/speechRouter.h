// Speech output routing for the CAI macOS dylib, modeled on SpeechManager in
// the Say the Spire 2 accessibility mod: a handler setting (auto, prism,
// systemvoice) picks who speaks, prism has its own backend setting, and the
// streamed system voice (speech.mm) has voice, rate and volume.
// All settings live in the [Speech] section of the INI under the same keys
// as the SettingIds in src/data/settings_CAI.sql.
//
// The default handler is the system voice, not auto: on a Mac without
// VoiceOver running, prism's best backend is AVSpeech, whose own queue leaves
// 120 to 250 ms of silence between lines (see speech.mm). Auto takes prism
// only when its best backend is VoiceOver, so VoiceOver users opt into auto
// or prism; a handler that fails to start falls back to the system voice.
//
// Every function takes the router's mutex and may be called from any thread;
// in the game they all arrive on the Lua thread.
#pragma once
#include <string>

namespace speech {
// (Re)activate the handler from the current INI values. The previous handler
// is released first and any current speech stops.
void Activate();
// Activate once. After an activation in which no handler could start, later
// calls do nothing until a setting changes, so a broken setup does not retry
// on every spoken line.
void EnsureActive();
// A key in the [Speech] section changed: re-activate when the handler or the
// prism backend changed, otherwise just reapply the system voice settings so
// a slider step does not interrupt speech or restart a backend.
void SettingChanged(const char* key);
bool IsActive();
void Say(const char* text, bool interrupt);
void Stop();
// A key went down in the game window (keyboard.mm): stop what the mod voices
// itself, as a screen reader stops speaking on a key press. A VoiceOver
// backend is left alone, since VoiceOver does this on its own. With wait
// false the call never blocks: it returns false, having done nothing, when
// another thread holds a lock the stop needs, so the NSEvent monitor on the
// main thread can retry from a worker instead of waiting on a game thread.
bool KeyPressed(bool wait);
bool IsSpeaking();
// "VoiceOver", "AVSpeech", "System voice" and so on; empty when nothing is active.
std::string ActiveName();
// Prism backends usable on this machine, one name per line.
std::string PrismBackendNames();
// Installed system voices as "identifier\tname\tlanguage" lines.
std::string SystemVoices();
// Identifier of the Spoken Content voice the system voice stream resolved.
std::string SystemDefaultVoice();
}
