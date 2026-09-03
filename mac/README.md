# CAI on macOS

This folder is the native layer of the mod for the macOS build of Civilization VI (the Aspyr port sold on Steam). On Windows the mod's Lua talks to a replacement `LightFX.dll`, built from the `extern/civ6-accessibility-lua-integration` submodule, for speech, sounds, configuration, text input and the update check. On the Mac the same Lua talks to `libcai.dylib`, built from `native/`, which Steam loads into the game through a small launch wrapper. The mod's Lua is shared between the two platforms; the Mac-specific Lua paths are listed under "The Lua side" below.

Supported: Apple Silicon, macOS 13 or later, Civilization VI 1.4.6 build 376653.178. The dylib reads the bundle version at startup and stays idle on any other build, and also if any game export it relies on is missing, so a game update degrades to "no accessibility layer" rather than a crash. On an Intel Mac the wrapper starts the game without the mod.

## Layout

- `native/`: the dylib sources and the CMake build. `cai.h` declares what the units share; `entry.cpp` holds logging, the build guard and the capture of Lua errors; `handshake.cpp` captures the Lua state; `inject.cpp` creates `ExposedMembers.CAI`; `hks.h` and `hks.cpp` are the Havok Script binding; `luaapi.cpp` implements the `CAI.*` functions and the INI configuration; `speechRouter.cpp` chooses who speaks; `speech.mm` with `speech_samples.h` is the streamed system voice; `audio.cpp` with `miniaudio_impl.c` is the sound engine; `keyboard.mm` is the key monitor that stops speech on a key press and queues typed text; `platform.mm` is clipboard, window focus, paths and the release check; `speechtest.cpp` is a stand-alone test driver for the speech stream and `speech_samples_test.cpp` a host test for the sample helpers.
- `steam_launch.sh`, `install.sh`, `uninstall.sh`: shipped in the release package.
- `dev/`: `build.sh`, `deploy.sh`, `run.sh`, `kill.sh` for development and `package.sh` for the release asset.

## How the mod gets into the game

### Injection

The Windows DLL ships as `LightFX.dll` because the game loads that library for Alienware lighting. The Mac binary has no such loader, but its hardened runtime carries the `allow-dyld-environment-variables` and `disable-library-validation` entitlements, so the game accepts `DYLD_INSERT_LIBRARIES` and loads an ad-hoc signed dylib. The dylib's constructor runs before the game's `main` and nothing in the game bundle is modified.

Inline hooks are not possible: the runtime lacks `allow-unsigned-executable-memory`, so a patched code page kills the process with a code-signature `SIGKILL`. dyld interposing works only for calls that cross an image boundary, and the executable calls its own Lua functions directly, so the Lua state cannot be learned from a hook either.

### The handshake

`caiUtils.lua` runs `pcall(os.date, "ExposedMembers", 1234567890)` at the top of every context that includes it. Havok Script's `os.date` calls libc `gmtime` or `localtime`, cross-image calls the dylib interposes, and at that moment the `lua_State*` is still live in a callee-saved register of the `os.date` frame. A naked stub in `handshake.cpp` saves x0 to x30, and the C side checks that the time argument is the magic value, validates the register candidates against the state layout below, and injects `ExposedMembers.CAI` into the globals of that state. `ExposedMembers` is shared by every Lua context, so one injection serves the whole game; the handshake still runs per context because each context gets its own `print` replacement. The magic value must fit in 32 bits because `os.date` truncates its time argument. On Windows the call is harmless, since the DLL has injected the table long before.

### The Lua log

The Mac build writes no `Lua.log` and Lua `print` reaches nothing. The dylib provides the equivalent in `~/Library/Logs/CAI/cai_native.log`, with the previous run kept as `cai_native.log.1`. `print` is replaced per context in `inject.cpp`. Runtime errors are caught by interposing the libc `snprintf` family, through which the executable formats its "Runtime Error" text and traceback frames. Only libc functions are interposed, never a game export: an `__interpose` entry is an eager bind, and a game update that dropped the symbol would stop the dylib from loading instead of leaving it idle. The log lives under `~/Library/Logs` because the game empties its own `Logs` folder at startup. Setting `Debug=1` in the `[Logging]` section of the INI adds a line per native call.

## Havok Script on this binary

The binding uses only symbols the game executable exports, plus the state layout below. Used: `hks_pushnamedcclosure`, `lua_createtable`, `luaL_checklstring`, `luaL_checknumber`, `luaL_checkinteger`, `hks_obj_getfield`, `hks_obj_settable`, `hks_obj_setmetatable`, `hks_obj_tolstring`, `hks_obj_newlstringhashed` and `hks::CallStack::growApiStack`. Their mangled names are resolved with `dlsym` into a function-pointer table (`hks::Bind` in `hks.cpp`) at startup, so the dylib links against nothing in the game; if one is missing the dylib stays idle. Not exported: `lua_gettop`, `lua_settop`, `lua_getfield`, `lua_setfield`, `lua_type`, `lua_toboolean`, the `lua_push*` family, `luaL_ref`, `luaL_unref`, `lua_pcall` and `luaL_loadbuffer`. The stack functions are slot reads and writes once the layout is known, and `lua_pcall` is not needed: the only native-to-Lua call the Windows DLL makes is the character input handler, which on the Mac is a queue that Lua polls.

Layout of `lua_State`, offsets from the state pointer, arm64, build 1.4.6:

- `+0x10`: `global_State*`.
- `+0x18` to `+0x40`: the call stack (records, last record, current record, current Lua pc, hook return address, hook level).
- `+0x48`: `HksObject* top`; `+0x50`: `base`; `+0x58`: `alloc_top`; `+0x60`: `bottom`. This is the API stack. The arguments of a C function are `base[0..n)` with `n = top - base`. Pushing is a slot write and `top += 1`; popping is `top -= 1`.
- `+0x70`: `HksObject globals`, the globals table of this thread.
- `+0x80`: `cEnv`; `+0x90`: call sites; `+0x98`: number of C calls; `+0xa8`: name; `+0xb0`: `lua_State* nextState`, a linked list of all states.

`HksObject` is 16 bytes: a `uint32` tag, masked with `0xf`, padding, then an 8 byte value. It is passed and returned in x0:x1, so `hks_obj_getfield` returns its result in registers. Tags: nil 0, boolean 1, lightuserdata 2, number 3, string 4, table 5, function 6, userdata 7, thread 8, ifunction 9, cfunction 10. An interned string points at a `TString` whose length sits at `+0x08` with two flag bits in the top of the word (mask `0x3fffffffffffffff`), the `uint32` hash at `+0x10` and the NUL terminated characters at `+0x14`.

`hks_obj_newlstringhashed` needs the caller to supply the hash: Bob Jenkins' lookup3 `hashlittle` with `a = b = c = 0x6b6f7265 + length`, the length capped at 31 bytes for both the seed and the loop, full 12 byte blocks read as little-endian words and the tail bytes packed big-endian. `hks::Hash` implements it; the empty string hashes to `0x6b6f7265` and `"CAI"` to `0xbd5e268d`. The executable's exported `String::Hash` and `Platform::memhash` are different functions and must not be used for Lua strings.

The dylib constructor runs under dyld's lock before any static initializer of the game, so speech, audio and the release check start from the first Lua call that needs them, never in the constructor.

## Speech

### The streamed system voice

`AVSpeechSynthesizer` can speak an utterance itself, but its own queue starts and stops the audio output around every utterance, which leaves 120 to 250 ms of silence between lines. `speech.mm` therefore never lets AVSpeech play. Each line is rendered offline with `writeUtterance:toBufferCallback:`, in the voice's native format (Eloquence 16 kHz, compact voices 22.05 kHz). The silence AVSpeech leaves around the line is trimmed, the samples are brought to one loudness under a peak limiter (`speech_samples.h`), and the result plus 50 ms of silence is scheduled on an `AVAudioPlayerNode` on an `AVAudioEngine` of our own. Scheduled buffers play back to back, so lines are gapless, and a stop discards the scheduled buffers, which is the interrupt. A render cannot be aborted, so an interrupted render is parked with its synthesizer until AVSpeech delivers both of its end markers, and a fresh synthesizer takes the next line at once; the parked synthesizer is then released on the main queue, because freeing it from another thread while the framework was still in the write's completion crashed the game inside TextToSpeech. The design is a port of `MacSpeechStream.cs` from the Oxygen Not Included accessibility mod, reworked so nothing depends on a per-frame update or on the main thread: Lua runs on a game thread while AVSpeech's callbacks arrive on the main dispatch queue. `IsSpeaking` counts the pending lines, the render in flight and the buffers still scheduled on the player. A key press stops the stream through the key monitor described under "Keyboard".

### Who speaks

`speechRouter.cpp` follows the `SpeechManager` of the Say the Spire 2 accessibility mod. The `SpeechHandler` setting picks the handler:

- `systemvoice`, the default: the streamed system voice above, with the `SpeechVoice`, `SpeechRate` and `SpeechVolume` settings. An empty voice means the Spoken Content voice from System Settings, and the rate is seeded from Spoken Content on first use.
- `prism`: prism (https://github.com/ethindp/prism), a library over screen reader and speech APIs, with the `PrismBackend` setting naming a backend or `auto` for its best available. On a Mac, prism offers a VoiceOver backend ("VoiceOver (macOS)" in its registry) and an AVSpeech backend.
- `auto`: prism only when its best backend is VoiceOver, otherwise the system voice.

The default is the system voice rather than auto because on a Mac without VoiceOver running prism's best backend is AVSpeech, with the gaps described above. VoiceOver users opt into auto or prism. A handler that fails to start falls back to the system voice. Prism 0.18.2 is linked statically from its release archive with `-force_load`, because its backends register through static initializers, and needs IOKit.

Settings live in the `[Speech]` section of the INI under the same keys as the SettingIds in `src/data/settings_CAI.sql`. A change made through `CAI.SetConfigValue` to that section takes effect at once: a new handler or prism backend re-activates the handler, while voice, rate and volume are applied to the running system voice without interrupting it.

`AVSpeechSynthesizer` ignores the requested voice when the process runs in the Background launchd session, as it does when started from an SSH shell. Steam launches are in the GUI session and unaffected; `dev/run.sh` bootstraps the game as a launchd job into `gui/<uid>` for the same reason.

## Sound

`audio.cpp` ports the Windows `AudioManager` and `Sound` classes onto miniaudio (CoreAudio backend, WAV decoder only, `MA_NO_RUNTIME_LINKING`, compiled once as C in `miniaudio_impl.c`). The per-sound and listener state is cached so the engine can be torn down and rebuilt without the Lua side noticing when the device stops or fails to restart; a rebuild that fails is retried a few times, seconds apart, before sounds are given up. A change of the default output device is handled by miniaudio itself, which moves its audio unit before it reports the reroute.

## Text input

The Windows DLL hooks the window procedure for `WM_CHAR` and calls the registered Lua handler directly. On the Mac, the `NSEvent` monitor in `keyboard.mm` (see "Keyboard" below) queues the characters typed into the game window, and `UIScreenManager:OnUpdate` drains the queue once per frame through `CAI.PollCharInput()`, so the native side never calls into Lua. Characters typed with Command, Control, Option or Fn held are dropped, since those are key bindings and Option produces dead-key characters, and so are control and navigation characters. The clipboard comes from `NSPasteboard`, window focus from `NSRunningApplication`, and the release check is an `NSURLSession` fetch of the GitHub releases endpoint parsed as on Windows.

## Keyboard

- Every key press stops speech, as a screen reader does. On Windows the screen reader itself does this; on the Mac nothing would, so `keyboard.mm` installs an `NSEvent` local monitor for key-down and modifier-change events at inject time and calls `speech::KeyPressed` on each. A local monitor sees an event before the window does, so the stop always lands before the game thread handles the key and Lua speaks for it. The stop reaches the system voice stream or prism's AVSpeech backend, never a VoiceOver backend, since VoiceOver stops itself. The monitor runs on the main thread, which must not wait on the game thread: that thread may be inside an AVSpeech or AVAudioEngine call under the stream's lock, and whether those wait on the main queue in turn is undocumented. So the stop first tries its locks without blocking (`cai_speech_try_stop`) and, when one is busy, repeats from a worker thread. An idle stream is left untouched, so the per-key cost is two uncontended locks.
- Alt is the Option key and Control is the Control key. The Aspyr build reports Option as Alt, so every existing binding works unchanged. Alt is not mapped to Command because the mod's Alt gestures include Alt+Q, Alt+H, Alt+M and Alt+Space, which would land on Cmd+Q (quit), Cmd+H (hide), Cmd+M (minimize) and Cmd+Space (Spotlight).
- The Command key reaches Lua as a key of its own, `Keys.VK_LWIN` (118), with normal down and up events, and sets no modifier flag. A key pressed while Command is held arrives as the plain key, and Command stacks with Shift. `InputStruct:GetFlags()` knows Shift 4, Control 8 and Alt 16 only. The dylib exposes the live state as `CAI.IsCommandDown()` through `CGEventSourceFlagsState`, which is safe from the game thread and cannot get stuck when the window loses focus; `caiUtils.lua` tracks `VK_LWIN` itself as the fallback when the native layer is absent.
- The one remapping: a Control binding on one of the four arrow keys is Command on the Mac, Shift stacking, because all four Ctrl+arrow combinations are Mission Control shortcuts that macOS keeps by default and never reach the game. The affected bindings are the column jumps in tables and grids and word movement and selection in edit boxes. Control is not swapped for Command wholesale because Cmd+Space, Cmd+Tab, Cmd+Q, Cmd+W, Cmd+M and the screenshot keys belong to macOS, and because the engine's hotkey gesture strings can only express Ctrl, Alt and Shift. The rule is implemented in three places and nowhere else: `IsMacCommandKey` in `caiUtils.lua`, the modifier check in `UIWidget:OnHandleInput`, and `FormatBinding` in the input help, which speaks those bindings as Command. Spoken key help also says Option instead of Alt on the Mac.
- Combinations macOS or the app menu take first remain unusable: Cmd+Tab, Cmd+Space, Cmd+Q, Cmd+H, Cmd+M, Cmd+W, Cmd+backtick and the screenshot keys. Clashes that depend on the user's settings are documented in the README's macOS section rather than remapped: Ctrl+Space (input source switching), Ctrl+1 to Ctrl+9 (switch to desktop), F11 (Show Desktop), the "Use F1, F2, etc. keys as standard function keys" setting, and VoiceOver's Keyboard Commander taking Right Option.

## Steam launch

The launch option is `"<path>/steam_launch.sh" %command%`. Each line of the wrapper exists because a launch fails without it:

- Steam on macOS does not run launch options through a shell, so `DYLD_INSERT_LIBRARIES=... %command%` makes Steam try to spawn a program of that name. A wrapper script is the only way to set the variable.
- `%command%` expands to the app bundle path, not to an executable. The wrapper resolves the executable itself.
- The bundle's own executable, `Civ6_Exe`, is Aspyr's launcher, which spawns the real game, `Civ6_Exe_Child`. Started from a wrapper the launcher idles in its event loop and never spawns the game, so the wrapper starts the child directly, with the working directory set to `Contents/MacOS` and the `SteamAppId` and `SteamGameId` Steam already put in the environment. Steam tracks the process; the Steam overlay is not loaded this way.
- Steam is an x86_64 process under Rosetta, and children of a translated process start translated when the binary is universal. The wrapper uses `arch -arm64` to start the native slice, which the arm64-only dylib needs.
- `arch` and `sh` are protected system binaries, and dyld strips every `DYLD_` variable from their environment when they start. So the wrapper runs `arch -arm64 /bin/sh -c 'export DYLD_INSERT_LIBRARIES=...; exec game'`: the arm64 shell sets the variable itself right before it starts the game, which is entitled to receive it.
- Steam has no macOS privacy grant for Documents, Desktop or Downloads, so a wrapper placed there never starts and the launch hangs at "CreatingProcess". The native files therefore live in `~/Library/Application Support/Sid Meier's Civilization VI/CAI/`.

Whatever goes wrong, the wrapper starts the game, without the mod when the dylib is missing or the Mac is not Apple Silicon. It logs each launch to `~/Library/Logs/CAI/steam_launch.log`, keeping the previous launch as `steam_launch.log.1`.

## Vanilla parity

Aspyr patched nine vanilla Lua files, marked with `ASPYR MOD BEGIN` or `ASL_BEGIN` comments and calls to `UI.GetAspyrAppVersion()`. The mod's partial replacements include the vanilla file and inherit those edits. Four of the mod's full-copy replacements collide with patched files and carry a `local m_isAspyrMacBuild = (UI.GetAspyrAppVersion ~= nil)` flag that branches where the Aspyr edit is behavioral:

- `src/UI/frontEnd/MainMenu.lua`: the version label shows the Aspyr version, the crossplay multiplayer entry is absent, and the offline Internet tooltip is re-evaluated live for COPPA age restriction.
- `src/UI/shared/Options.lua`: no borderless window mode, and six controls that the Mac `Options.xml` omits (tuner, multi-GPU, leader motion blur, touch input, RGB lighting, mouse capture) are guarded with `Controls.X ~= nil`.
- `src/UI/shared/PopupDialog.lua`: `Close()` clears the countdown end callback so the Options screen's `RevertGraphicsChanges` is not called after the dialog has closed.
- `src/UI/FiraxisLive/My2K.lua`: the unlinked account tooltip is only shown for age-restricted accounts.

The Windows path of each file is unchanged. After a game update, diff the mod's full copies against the Mac originals under `Civ6.app/Contents/Assets/Base/Assets/UI/` and keep the lines only vanilla has.

## The Lua side

Everything Mac-specific in `src/`:

- `src/UI/shared/caiUtils.lua`: the handshake, `IsMacBuild()`, the Command key helpers `IsMacCommandKey`, `TrackCommandKey` and `IsCommandDown`, and the clock `GetMonotonicTime()`. `Automation.GetTime()` advances only once per second on the Aspyr build (the native log showed every delayed cursor sound starting at the same fraction of a second, up to a second late), so every CAI timer and the audio manager's delayed playback read `CAI.GetTime()`, a `steady_clock` reading from the dylib, and `Automation.GetTime()` only on Windows.
- `src/UI/uiManager/CAIUIScreenManager.lua`: `PollCharInput` from `OnUpdate`, and `TrackCommandKey` at the top of `HandleInput`.
- The queue is drained in two places. Per frame from the contexts that own an update hook (`IntroScreen.lua`, `MainMenu.lua`, `WorldInput_CAI.lua`; those front-end hooks precede the accessibility section's `local mgr`, so they resolve `ExposedMembers.CAI_UIManager` at call time). And on every key-up at the top of `UIScreenManager:HandleInput`, because a context's update callback stops while a popup such as game setup or the leader picker sits above it, while key events still reach the manager; by the key-up the key-down has been handled and the character queued, which keeps the Windows order of key-down then character.
- `src/UI/uiManager/CAIWidget_Base.lua`: the Command check in `UIWidget:OnHandleInput`.
- `src/UI/uiManager/helpers/CAIWidgetHelpers_InputHelp.lua`: `GetAltKeyName` and the Command case in `FormatBinding`.
- `src/UI/shared/CAISettings.lua`: `GetDefinitions` filters on the `Platform` column, `GetOptions` dispatches to `CAISettings.OptionProviders`, and the two providers `SpeechVoices` and `PrismBackends` build their dropdowns from the native layer. Provider rows carry `IsLiteral`, which the settings helper honors by showing the label text as is.
- `src/data/settings_CAI.sql`: the `Platform` and `OptionsProvider` columns of `CAI_Settings` and the Speech section rows.
- The native API has six functions the Windows DLL lacks: `PollCharInput`, `IsCommandDown`, `GetTime`, `GetSpeechVoices`, `GetSpeechSystemVoice` and `GetPrismBackends`. Lua checks for them with `CAI.X ~= nil`. `src/ideHelpers.lua` annotates all of them.

## Files on the Mac

- Game: `~/Library/Application Support/Steam/steamapps/common/Sid Meier's Civilization VI/Civ6.app`, Steam app id 289070. Assets under `Contents/Assets/Base/Assets/UI/` and `Contents/Assets/DLC/`, the same layout as Windows.
- Mods: `~/Library/Application Support/Sid Meier's Civilization VI/Sid Meier's Civilization VI/Mods/`. The game follows a symlink there, and APFS is case-insensitive by default, like Windows.
- Game options and logs: `~/Library/Application Support/Sid Meier's Civilization VI/Firaxis Games/Sid Meier's Civilization VI/`, with `AppOptions.txt`, `UserOptions.txt`, `Mods.sqlite` and `Logs/`.
- Native layer: `~/Library/Application Support/Sid Meier's Civilization VI/CAI/` holding `libcai.dylib` and `steam_launch.sh`.
- Configuration: `~/Library/Application Support/Sid Meier's Civilization VI/civ6-accessibility-integration.ini`.
- Log: `~/Library/Logs/CAI/cai_native.log`, previous run in `cai_native.log.1`, wrapper log in `steam_launch.log`.

## Building and developing

Requirements: Xcode command line tools (clang with C++17), CMake 3.24 or later and Ninja, both from Homebrew. `dev/build.sh` configures with the `default` preset (Ninja, RelWithDebInfo, build tree in `native/build/`, which is gitignored), builds `libcai.dylib`, which the linker ad-hoc signs, and runs `dev/deploy.sh` to copy the dylib and the wrapper into the CAI folder. The first configure downloads about 17 MB: prism 0.18.2, miniaudio 0.11.25 and SimpleIni 4.26, each pinned by URL and SHA-256 in `CMakeLists.txt`. No binary or vendored header is committed, and the build does not use the Windows submodule.

For development, symlink the repo's `src/` into the Mods folder as `CivVi-Accessibility-Integration`, as on Windows. If the repo lives under `~/Documents`, Steam needs the Documents privacy grant in System Settings, because a Steam-launched game is attributed to Steam for privacy checks; without it mod discovery blocks on a permission prompt.

- `dev/run.sh` launches the game with the built dylib as a launchd job in the GUI session, with Steam running. Its plist and `run.log` land next to it and are gitignored. `dev/kill.sh` stops it.
- `speechtest` is a CMake target outside the default build: `cmake --build --preset default --target speechtest`, run from `native/build/` with `--runloop` (the test's main thread must drain the main queue as the game's does). It lists the voices, speaks queued lines and measures the timing of an interrupt; see the options at the top of `speechtest.cpp`. Building with `-DCAI_SPEECH_INITIAL_RATE=16000` forces the player reconnect path that a voice with another sample rate takes.
- `samplestest` is the host test for `speech_samples.h`: `cmake --build --preset default --target samplestest && build/samplestest`. It exits non-zero on a failed check.

## Releasing

`dev/package.sh` builds `release/Civ-VI-Accessibility-Integration-<version>-macos.zip`, the version taken from the modinfo, containing the mod folder as the Windows release packages it (from the committed tree, without `ideHelpers.lua` and `todo.md`), `libcai.dylib`, `steam_launch.sh`, `install.sh`, `uninstall.sh`, and a `licenses` folder with prism's NOTICE and LICENSES plus the miniaudio and SimpleIni license texts extracted from their headers. The Windows release workflow rewrites the modinfo version from the release tag, so run the script on the release commit after that bump; an optional version argument is checked against the modinfo. The script also refuses to package when the supported game build in `install.sh` and `entry.cpp` differ. With `CAI_SIGN_IDENTITY` set to a Developer ID the dylib is signed with the hardened runtime and a timestamp, and with `CAI_NOTARY_PROFILE` set it is notarized as well (a dylib cannot be stapled; the ticket is checked online). Without them the dylib keeps its ad-hoc signature, which loads because the game disables library validation; `install.sh` clears the quarantine flag on the downloaded files.

`install.sh` copies the mod into the Mods folder (leaving a symlink there alone), the dylib and the wrapper into the CAI folder, and puts the Steam launch option on the clipboard (`--no-clipboard` skips that). `uninstall.sh` removes them and the log folder, asks first unless given `--yes`, and keeps the INI unless given `--purge`.

## Third-party code

- prism by Ethin Probst, MPL-2.0, statically linked.
- miniaudio by David Reid, MIT-0 or public domain, compiled in.
- SimpleIni by Brodie Thiesfield, MIT, header only. The same library the Windows integration uses for its INI file.
