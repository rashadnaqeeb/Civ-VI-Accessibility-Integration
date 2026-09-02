// macOS platform helpers: clipboard, window focus, user data path, bundle
// version, and the release check against GitHub.
#import <AppKit/AppKit.h>
#include "cai.h"
#include <mutex>
#include <string>

namespace {
// The release check, the same endpoint and request the Windows integration's
// UpdateManager uses.
constexpr const char* kLatestReleaseUrl = "https://api.github.com/repos/flat-arther/Civ-VI-Accessibility-Integration/releases/latest";
constexpr const char* kUserAgent = "Civ6 Accessibility";
}

// Every function here is called from the game's Lua thread, not the main
// thread. NSPasteboard reads are documented as usable from any thread.
std::string PlatformClipboardText() {
    @autoreleasepool {
        NSString* s = [[NSPasteboard generalPasteboard] stringForType:NSPasteboardTypeString];
        return s ? std::string([s UTF8String]) : std::string();
    }
}

// Live Command key state. The game reports Command to Lua as a plain key
// (VK_LWIN) with no modifier flag, so CAI's binding matcher asks here.
// CGEventSourceFlagsState is a plain C call, safe from the game thread.
bool PlatformIsCommandDown() {
    return (CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState) & kCGEventFlagMaskCommand) != 0;
}

// NSRunningApplication is the thread-safe way to ask; NSApp.isActive is
// AppKit state that belongs to the main thread.
bool PlatformIsGameWindowFocused() {
    @autoreleasepool {
        return [[NSRunningApplication currentApplication] isActive];
    }
}

std::string PlatformUserDataDir() {
    @autoreleasepool {
        NSString* home = NSHomeDirectory();
        return std::string([home UTF8String]) + "/Library/Application Support/Sid Meier's Civilization VI";
    }
}

// CFBundleShortVersionString and CFBundleVersion of the app the dylib was loaded into.
void PlatformBundleVersion(std::string& shortVersion, std::string& build) {
    @autoreleasepool {
        NSDictionary* info = [[NSBundle mainBundle] infoDictionary];
        NSString* s = info[@"CFBundleShortVersionString"];
        NSString* b = info[@"CFBundleVersion"];
        shortVersion = s ? [s UTF8String] : "";
        build = b ? [b UTF8String] : "";
    }
}

// --- Release check ------------------------------------------------------------
// The result is empty until the request completes or if it fails; Lua's
// version comparison treats an empty string as "unknown" and stays quiet.
namespace {
std::mutex g_versionMutex;
std::string g_latestVersion;

// The tag_name field of the release JSON, scanned the way the Windows
// integration does it rather than through NSJSONSerialization, so both
// platforms accept and reject the same responses.
std::string TagName(const std::string& json) {
    size_t key = json.find("\"tag_name\"");
    if (key == std::string::npos) return {};
    size_t colon = json.find(':', key);
    if (colon == std::string::npos) return {};
    size_t q1 = json.find('"', colon);
    if (q1 == std::string::npos) return {};
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return json.substr(q1 + 1, q2 - q1 - 1);
}
}

void PlatformStartUpdateCheck() {
    @autoreleasepool {
        NSURL* url = [NSURL URLWithString:@(kLatestReleaseUrl)];
        NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:url];
        req.timeoutInterval = 20;
        [req setValue:@(kUserAgent) forHTTPHeaderField:@"User-Agent"];
        [req setValue:@"application/vnd.github+json" forHTTPHeaderField:@"Accept"];
        NSURLSessionDataTask* task = [[NSURLSession sharedSession] dataTaskWithRequest:req
            completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                NSInteger status = [response isKindOfClass:[NSHTTPURLResponse class]] ? [(NSHTTPURLResponse*)response statusCode] : 0;
                if (error || status != 200 || !data) {
                    Log("update check failed: HTTP %ld %s", (long)status, error ? [[error localizedDescription] UTF8String] : "");
                    return;
                }
                std::string json((const char*)[data bytes], [data length]);
                std::string version = TagName(json);
                if (!version.empty() && version.front() == 'v') version.erase(0, 1);
                {
                    std::lock_guard<std::mutex> lock(g_versionMutex);
                    g_latestVersion = version;
                }
                Log("update check: latest release is %s", version.empty() ? "(unknown)" : version.c_str());
            }];
        [task resume];
    }
}

std::string PlatformLatestVersion() {
    std::lock_guard<std::mutex> lock(g_versionMutex);
    return g_latestVersion;
}
