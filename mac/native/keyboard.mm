// Keyboard on macOS: one NSEvent local monitor on the main queue sees every
// key the game window receives. It stops speech on every key press, as a
// screen reader does, and queues the characters typed into text fields; Lua
// drains that queue once per frame through CAI.PollCharInput(), so the native
// side never calls into Lua. It also tracks input method composition (the
// marked text of a Japanese, Chinese or Korean input source) for
// CAI.IsImeComposing(), the Mac side of the WM_IME_STARTCOMPOSITION and
// WM_IME_ENDCOMPOSITION hooks of the Windows DLL.
#import <AppKit/AppKit.h>
#include "cai.h"
#include "speechRouter.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <string>

namespace {
// Typed characters not yet polled. A field that stops polling (the mod
// suspended mid-typing) must not accumulate input without bound, so the
// oldest character is dropped past this many.
constexpr size_t kMaxQueuedCharacters = 256;

std::mutex g_mutex;
std::deque<std::string> g_queue;
id g_monitor = nil;                            // main thread only
NSEventModifierFlags g_modifiers = 0;          // main thread only: the modifiers held at the last event
std::atomic<bool> g_installRequested{false};
std::atomic<bool> g_charInput{false};          // written by the game thread, read by the monitor on the main thread
std::atomic<bool> g_composing{false};          // sampled on the main thread around each key, read by the game thread

// Main thread. True while the first responder holds marked text, the
// uncommitted part of an input method composition. The game's view adopts
// NSTextInputClient (its binary references the protocol), so an input method
// composes through it; a responder without a text input context never
// composes and reads as false.
bool IsComposing() {
    id client = [NSTextInputContext currentInputContext].client;
    return client != nil && [client respondsToSelector:@selector(hasMarkedText)] && [client hasMarkedText];
}

// A key went down. Stop what the mod is voicing, as a screen reader stops
// speaking on a key press, before the game sees the key and Lua speaks for
// it. The monitor runs on the main thread, which must not wait on a game
// thread: that thread may be inside an AVSpeech or AVAudioEngine call under
// the same lock, and whether those ever wait on the main queue in turn is
// not documented. So the stop is tried without blocking, and when a lock is
// busy it is repeated from a worker thread, which still lands well before
// the game thread has handled the key.
void OnKeyPressed() {
    if (speech::KeyPressed(false)) return;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{ speech::KeyPressed(true); });
}

// Modifiers produce no key-down event, only a flags change. One whose bit
// was clear at the last event has just gone down; a release changes nothing.
void OnFlagsChanged(NSEvent* e) {
    NSEventModifierFlags mods = [e modifierFlags] & NSEventModifierFlagDeviceIndependentFlagsMask;
    bool pressed = (mods & ~g_modifiers) != 0;
    g_modifiers = mods;
    if (pressed) OnKeyPressed();
}

// The text a key-down typed, as UTF-8; empty when it typed none.
std::string TypedText(NSEvent* e) {
    NSEventModifierFlags mods = [e modifierFlags] & NSEventModifierFlagDeviceIndependentFlagsMask;
    // Characters typed with Command, Control, Option or Function held are
    // key bindings, not text (Option would produce dead-key characters).
    if (mods & (NSEventModifierFlagCommand | NSEventModifierFlagControl | NSEventModifierFlagOption | NSEventModifierFlagFunction)) return {};
    NSString* chars = [e characters];
    if (chars.length == 0) return {};
    std::string out;
    for (NSUInteger i = 0; i < chars.length; ++i) {
        unichar c = [chars characterAtIndex:i];
        if (c < 0x20 || c == 0x7f) continue;               // control characters: Enter, Tab, Escape, Backspace
        if (c >= 0xF700 && c <= 0xF8FF) continue;          // arrows, function keys, navigation keys
        // A surrogate pair is one character; a surrogate on its own is not
        // valid text and has no UTF-8 form, so it is dropped.
        if (CFStringIsSurrogateHighCharacter(c)) {
            if (i + 1 < chars.length && CFStringIsSurrogateLowCharacter([chars characterAtIndex:i + 1])) {
                if (const char* utf8 = [[chars substringWithRange:NSMakeRange(i, 2)] UTF8String]) out += utf8;
                ++i;
            }
            continue;
        }
        if (CFStringIsSurrogateLowCharacter(c)) continue;
        if (const char* utf8 = [[NSString stringWithCharacters:&c length:1] UTF8String]) out += utf8;
    }
    return out;
}

// A key-down: track composition and queue the text it typed, for
// CAI.PollCharInput(). The monitor sees the key before the window does, so
// the composition state is sampled twice: now, when marked text means the
// key edits or commits a composition and types nothing, and again after the
// window has handled it, when marked text means the key started one and its
// raw character belongs to the input method. The queueing therefore happens
// in that later block; it runs on the next turn of the main run loop, still
// long before Lua drains the queue for the key.
void OnKeyDown(NSEvent* e) {
    if (IsComposing()) {
        g_composing.store(true);
        dispatch_async(dispatch_get_main_queue(), ^{ g_composing.store(IsComposing()); });
        return;
    }
    std::string text = g_charInput.load(std::memory_order_relaxed) ? TypedText(e) : std::string();
    dispatch_async(dispatch_get_main_queue(), ^{
        bool composing = IsComposing();
        g_composing.store(composing);
        if (composing || text.empty()) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.push_back(text);
        if (g_queue.size() > kMaxQueuedCharacters) g_queue.pop_front();
    });
}

// Runs on the main thread. The monitor stays installed for the life of the
// process. A local monitor only sees events delivered to this application,
// and sees them before the window does.
void InstallMonitor() {
    if (g_monitor) return;
    g_modifiers = [NSEvent modifierFlags] & NSEventModifierFlagDeviceIndependentFlagsMask;
    g_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskFlagsChanged handler:^NSEvent*(NSEvent* e) {
        if ([e type] == NSEventTypeFlagsChanged) {
            OnFlagsChanged(e);
            return e;
        }
        OnKeyPressed();
        OnKeyDown(e);
        return e;
    }];
    Log("keyboard: NSEvent monitor %s", g_monitor ? "installed" : "FAILED to install");
}
}

void KeyboardInstall() {
    if (g_installRequested.exchange(true)) return;
    if ([NSThread isMainThread]) InstallMonitor();
    else dispatch_async(dispatch_get_main_queue(), ^{ InstallMonitor(); });
}

void CharInputEnable(bool enable) {
    g_charInput.store(enable);
    if (!enable) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_queue.clear();
    }
}

bool KeyboardIsComposing() {
    return g_composing.load();
}

bool CharInputPoll(std::string& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_queue.empty()) return false;
    out = std::move(g_queue.front());
    g_queue.pop_front();
    return true;
}
