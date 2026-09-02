// miniaudio implementation unit for the CAI macOS dylib. Compiled as C.
// Only the CoreAudio backend and the WAV decoder are needed: all 27 shipped
// sounds are WAV, so no vorbis/opus backends as on Windows.
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_COREAUDIO
#define MA_NO_RUNTIME_LINKING
#define MA_NO_MP3
#define MA_NO_FLAC
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_IMPLEMENTATION
#include "miniaudio.h"
