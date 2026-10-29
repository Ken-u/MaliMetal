// SPDX-License-Identifier: MIT
// Metal plugin for the Mali-G610 (Mali + PanVK), reached through the original discovery:
//   IOServiceMatching("IOAcceleratorES") -> property MetalPluginName -> /System/Library/Extensions/<name>.bundle -> principal class
//   (NSPrincipalClass in Info.plist) -> [[cls alloc] initWithAcceleratorPort:port].
// The render server (backboardd) is the one process that gets a device: the native Metal frontend (NativeBridgeMetalFrontend.mm, the
// metal2vulkan runtime over the bridge) opened on PanVK, which the original registration (_MTLAddDevice) adds to the system's devices;
// QuartzCore then renders the UI through it. Every other process returns nil and keeps the original software renderer.
// Diagnostics reach the kernel's console through MaliService (MALI_LOG), the one console a sandboxed process can write.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#import <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <pwd.h>
#include <vector>
#include <utility>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalBridge.h"
#include "mali_uapi.h"

@interface _MTLDevice : NSObject
@end
@interface NativeMaliMetalDevice : _MTLDevice
- (id)initWithAcceleratorPort:(unsigned)port;
@end

namespace {

typedef unsigned (*MatchingFn)(const char *);
typedef unsigned (*GetServiceFn)(unsigned, void *);
typedef int (*OpenFn)(unsigned, unsigned, unsigned, unsigned *);
typedef int (*CallFn)(unsigned, unsigned, const void *, size_t, void *, size_t *);
typedef int (*EntryIDFn)(unsigned, uint64_t *);
typedef int (*RetainFn)(unsigned);
typedef int (*MapFn)(unsigned, unsigned, unsigned, uint64_t *, uint64_t *, unsigned);

struct IOKit {
    void *handle;
    void *(*matching)(const char *);
    GetServiceFn getService;
    OpenFn open;
    CallFn call;
    EntryIDFn entryID;
    RetainFn retain;
    MapFn map;
};

IOKit &iokit() {
    static IOKit k;
    static bool ready;
    if (!ready) {
        ready = true;
        k.handle = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW | RTLD_LOCAL);
        if (k.handle) {
            k.matching = (void *(*)(const char *))dlsym(k.handle, "IOServiceMatching");
            k.getService = (GetServiceFn)dlsym(k.handle, "IOServiceGetMatchingService");
            k.open = (OpenFn)dlsym(k.handle, "IOServiceOpen");
            k.call = (CallFn)dlsym(k.handle, "IOConnectCallStructMethod");
            k.entryID = (EntryIDFn)dlsym(k.handle, "IORegistryEntryGetRegistryEntryID");
            k.retain = (RetainFn)dlsym(k.handle, "IOObjectRetain");
            k.map = (MapFn)dlsym(k.handle, "IOConnectMapMemory64");
        }
    }
    return k;
}

// One line on the kernel's console ("mali: user: ..."), through a connection of this process to the Mali service.
void mlog(const char *format, ...) __attribute__((format(printf, 1, 2)));
void mlog(const char *format, ...) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static unsigned connection;
    static bool tried;
    IOKit &k = iokit();
    pthread_mutex_lock(&lock);
    if (!tried && k.matching && k.getService && k.open) {
        tried = true;
        const unsigned service = k.getService(0, k.matching(MALI_SERVICE));
        if (service) {
            unsigned c = 0;
            if (k.open(service, mach_task_self(), 0, &c) == 0)
                connection = c;
        }
    }
    if (connection && k.call) {
        mali_log_in line = {};
        va_list ap;
        va_start(ap, format);
        // The render server's lines are plain; another process's say whose they are.
        const bool server = strcmp(getprogname(), "backboardd") == 0;
        const int prefix = server ? 0 : snprintf(line.text, sizeof line.text, "[%s] ", getprogname());
        vsnprintf(line.text + prefix, sizeof line.text - prefix, format, ap);
        va_end(ap);
        k.call(connection, MALI_LOG, &line, sizeof line, nullptr, nullptr);
    }
    pthread_mutex_unlock(&lock);
}

// The directory this plugin's bundle is in (its executable is <dir>/<name>.bundle/<name>).
bool bundleDirectory(char *path, size_t size) {
    Dl_info info = {};
    if (!dladdr(reinterpret_cast<const void *>(&bundleDirectory), &info) || !info.dli_fname)
        return false;
    snprintf(path, size, "%s", info.dli_fname);
    char *slash = strrchr(path, '/');
    if (!slash)
        return false;
    *slash = 0;
    return true;
}

void missing(const char *klass, const char *selector) {
    mlog("metal: unimplemented %s %s", klass, selector);
}

// What the render server's framebuffer choice rests on (iPad7,1 17.7.11 QuartzCore, addresses of that dyld cache; xnu-probe/symbolize): the
// byte behind CADeviceUseCIF10 (the "deep" 10-bit framebuffer: MobileGestalt's answer, then the CAEnableDeepFramebuffer preference), the compression
// bytes, and the first display's flags (IOMFBDisplay::framebuffer_pixel_format answers 'b3a8' when bit 3 is set). Read only, for the console.
bool readTask(uintptr_t address, void *out, size_t size) {
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), address, size, (vm_address_t)out, &got) == KERN_SUCCESS && got == size;
}

void logFramebufferChoice(const char *when) {
    void *start = dlsym(RTLD_DEFAULT, "CARenderServerStart");
    unsigned prefs = 0;
    if (CFTypeRef value = CFPreferencesCopyAppValue(CFSTR("CAEnableDeepFramebuffer"), kCFPreferencesCurrentApplication)) {
        prefs |= 1;
        if (CFGetTypeID(value) == CFBooleanGetTypeID() && !CFBooleanGetValue((CFBooleanRef)value))
            prefs |= 2;
        CFRelease(value);
    }
    if (auto copyAnswer = (CFTypeRef(*)(CFStringRef))dlsym(RTLD_DEFAULT, "MGCopyAnswer")) {
        if (CFTypeRef deep = copyAnswer(CFSTR("Aixt/MEN2O2B7f+8m4TxUA"))) {
            prefs |= 8;
            if (CFGetTypeID(deep) == CFBooleanGetTypeID() && CFBooleanGetValue((CFBooleanRef)deep))
                prefs |= 4;
            CFRelease(deep);
        }
    }
    if (!start) {
        mlog("qc %s: no CARenderServerStart, pref bits %#x (1 set, 2 false, 4 MG deep, 8 MG answer), uid %d", when, prefs, getuid());
        return;
    }
    const intptr_t slide = (intptr_t)start - 0x189651efc;
    uint8_t v[4] = {};
    readTask(0x1f491ae10 + slide, &v[0], 1);   // CADeviceUseCIF10
    readTask(0x1ed7abf10 + slide, &v[1], 1);   // framebuffer compression in effect
    readTask(0x1f4919dc0 + slide, &v[2], 1);   // its default
    readTask(0x1f4919c20 + slide, &v[3], 1);   // universal buffer compression
    uint32_t flags = 0xffffffff;
    uint16_t limits = 0;
    unsigned displays = 0;
    @try {
        Class serverClass = NSClassFromString(@"CAWindowServer");
        id server = serverClass ? ((id(*)(id, SEL))objc_msgSend)(serverClass, NSSelectorFromString(@"serverIfRunning")) : nil;
        NSArray *list = server ? ((id(*)(id, SEL))objc_msgSend)(server, NSSelectorFromString(@"displays")) : nil;
        displays = (unsigned)list.count;
        id first = displays ? list[0] : nil;
        uintptr_t a = first ? (uintptr_t) * (void **)((char *)(__bridge void *)first + 8) : 0, b = 0, d = 0;
        if (a && readTask(a + 0x40, &b, 8) && b && readTask(b + 0x60, &d, 8) && d) {
            readTask(d + 0x238, &flags, 4);
            readTask(d + 0x258, &limits, 2);
        }
    } @catch (NSException *) {
    }
    mlog("qc %s: slide %#lx, deep %u comp %u default %u universal %u, display flags %#x limits %#x (%u display(s)), pref bits %#x (1 set, 2 false, 4 MG deep, 8 MG answer)",
         when, (unsigned long)slide, v[0], v[1], v[2], v[3], flags, limits, displays, prefs);
}

// The preference does not always reach the render server (the domain, or the sandbox's say on which domains it may read), and the display
// object has taken the byte's value by then: clear both in this process (what UniXNU's Adreno plugin did as an experiment), so the framebuffer
// formats it asks for next are the plain BGRA the display driver takes. Writes only when a flag is set.
void clearDeepFramebuffer() {
    void *start = dlsym(RTLD_DEFAULT, "CARenderServerStart");
    if (!start)
        return;
    const intptr_t slide = (intptr_t)start - 0x189651efc;
    uint8_t deep = 0, zero = 0;
    uint32_t flags = 0;
    unsigned cleared = 0;
    if (readTask(0x1f491ae10 + slide, &deep, 1) && deep && vm_write(mach_task_self(), 0x1f491ae10 + slide, (vm_offset_t)&zero, 1) == KERN_SUCCESS)
        cleared |= 1;
    @try {
        Class serverClass = NSClassFromString(@"CAWindowServer");
        id server = serverClass ? ((id(*)(id, SEL))objc_msgSend)(serverClass, NSSelectorFromString(@"serverIfRunning")) : nil;
        NSArray *list = server ? ((id(*)(id, SEL))objc_msgSend)(server, NSSelectorFromString(@"displays")) : nil;
        for (id display in list) {
            uintptr_t a = (uintptr_t) * (void **)((char *)(__bridge void *)display + 8), b = 0, d = 0;
            if (a && readTask(a + 0x40, &b, 8) && b && readTask(b + 0x60, &d, 8) && d && readTask(d + 0x238, &flags, 4) && (flags & 8)) {
                flags &= ~8u;
                if (vm_write(mach_task_self(), d + 0x238, (vm_offset_t)&flags, 4) == KERN_SUCCESS)
                    cleared |= 2;
            }
        }
    } @catch (NSException *) {
    }
    if (cleared)
        mlog("qc: cleared the deep framebuffer flags (%#x: 1 global byte, 2 a display's flag)", cleared);
}

// The bundle is in a process: which ones the discovery loads it into (the others never get this far: the sandbox, or no accelerator match).
// An Objective-C exception nobody catches ends in abort(), and the process's stderr goes to logd, not to the console (AegirPoster, gpu33/34: SIGABRT
// out of a main-queue block, with the throw's own stack already gone). The handler says what was thrown and where: the throw-time return addresses.
NSUncaughtExceptionHandler *gPreviousHandler;
void uncaughtException(NSException *exception) {
    mlog("exc: %.60s: %.130s", exception.name.UTF8String ?: "?", exception.reason.UTF8String ?: "?");
    NSArray<NSNumber *> *stack = exception.callStackReturnAddresses;
    char text[200];
    for (NSUInteger i = 0; i < stack.count && i < 24; i += 8) {
        int n = snprintf(text, sizeof text, "exc stack %lu:", (unsigned long)i);
        for (NSUInteger k = i; k < stack.count && k < i + 8; ++k)
            n += snprintf(text + n, sizeof text - n, " %#llx", stack[k].unsignedLongLongValue);
        mlog("%s", text);
    }
    if (gPreviousHandler)
        gPreviousHandler(exception);
}

// Outside the render server a CAMetalLayer takes the 8-bit twin of an extended-range 10-bit format (the frontend narrows pipelines and textures the same
// way): the device withholds XR10, CAMetalLayer raises CAMetalLayerInvalid for it (ClockKitUI's CLKUIMetalQuadView, AegirPoster), and metal2vulkan cannot
// blend into the sRGB ones.
void (*gOriginalSetPixelFormat)(id, SEL, NSUInteger);
void narrowedSetPixelFormat(id self, SEL cmd, NSUInteger format) {
    NSUInteger narrowed = format;
    switch (format) {
    case 552: case 554: narrowed = 80; break;
    case 553: case 555: narrowed = 81; break;
    default: break;
    }
    if (narrowed != format)
        mlog("metal: CAMetalLayer pixel format %lu -> %lu", (unsigned long)format, (unsigned long)narrowed);
    gOriginalSetPixelFormat(self, cmd, narrowed);
}

void narrowMetalLayerFormats() {
    if (strcmp(getprogname(), "backboardd") == 0)
        return;
    Method method = class_getInstanceMethod(objc_getClass("CAMetalLayer"), sel_registerName("setPixelFormat:"));
    if (!method)
        return;
    gOriginalSetPixelFormat = reinterpret_cast<void (*)(id, SEL, NSUInteger)>(method_getImplementation(method));
    method_setImplementation(method, reinterpret_cast<IMP>(&narrowedSetPixelFormat));
}

// A fault in a process whose stderr nobody reads (the poster extensions: SIGSEGV in a memmove called from the Vulkan driver, gpu37): the fault address, the
// registers and the frame-pointer chain, each address as image offset + nearest symbol, on the console. Then the default action, so the kernel's crash line
// still comes.
void describeAddress(char *out, size_t size, uint64_t address) {
    Dl_info info = {};
    if (dladdr(reinterpret_cast<const void *>(address), &info) && info.dli_fname) {
        const char *image = strrchr(info.dli_fname, '/');
        image = image ? image + 1 : info.dli_fname;
        snprintf(out, size, "%s+%#llx %s", image, (unsigned long long)(address - reinterpret_cast<uint64_t>(info.dli_fbase)), info.dli_sname ? info.dli_sname : "");
    } else {
        snprintf(out, size, "%#llx", (unsigned long long)address);
    }
}

void faultHandler(int sig, siginfo_t *info, void *context) {
    static volatile int once;
    if (__sync_lock_test_and_set(&once, 1)) {
        signal(sig, SIG_DFL);
        return;
    }
    ucontext_t *uc = static_cast<ucontext_t *>(context);
    const uint64_t mask = 0x0000007fffffffffull;
    const uint64_t pc = uc->uc_mcontext->__ss.__pc & mask, lr = uc->uc_mcontext->__ss.__lr & mask;
    uint64_t fp = uc->uc_mcontext->__ss.__fp & mask;
    char text[160];
    mlog("fault: signal %d at %#llx, pc %#llx lr %#llx x0 %#llx x1 %#llx x2 %#llx", sig, (unsigned long long)reinterpret_cast<uintptr_t>(info->si_addr), (unsigned long long)pc,
         (unsigned long long)lr, (unsigned long long)uc->uc_mcontext->__ss.__x[0], (unsigned long long)uc->uc_mcontext->__ss.__x[1], (unsigned long long)uc->uc_mcontext->__ss.__x[2]);
    describeAddress(text, sizeof text, lr);
    mlog("fault: lr = %s", text);
    for (unsigned i = 0; i < 12 && fp && !(fp & 7); ++i) {
        const uint64_t *frame = reinterpret_cast<const uint64_t *>(fp);
        vm_size_t got = 0;
        uint64_t pair[2];
        if (vm_read_overwrite(mach_task_self(), fp, sizeof pair, reinterpret_cast<vm_address_t>(pair), &got) != KERN_SUCCESS || got != sizeof pair)
            break;
        (void)frame;
        describeAddress(text, sizeof text, pair[1] & mask);
        mlog("fault: #%u %s", i, text);
        if ((pair[0] & mask) <= fp)
            break;
        fp = pair[0] & mask;
    }
    signal(sig, SIG_DFL);
}

void installFaultHandler() {
    if (strcmp(getprogname(), "backboardd") == 0)
        return;
    struct sigaction action = {};
    action.sa_sigaction = faultHandler;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    sigaction(SIGSEGV, &action, nullptr);
    sigaction(SIGBUS, &action, nullptr);
}

bool stacksWanted();
void *stackThread(void *);

__attribute__((constructor)) void bundleLoaded() {
    mlog("metal: bundle loaded in %s (pid %d)", getprogname(), getpid());
    narrowMetalLayerFormats();
    installFaultHandler();
    if (stacksWanted()) {
        pthread_t thread;
        if (pthread_create(&thread, nullptr, stackThread, nullptr) == 0)
            pthread_detach(thread);
    }
    gPreviousHandler = NSGetUncaughtExceptionHandler();
    NSSetUncaughtExceptionHandler(&uncaughtException);
}

// What the runtime told through its message sink: every distinct message (digits masked in the key), its text the first time it is heard, in
// 150-character rows (a shadow image's reason names the object and why no image aliases it; a degraded pass says what it costs), and its count
// again each time it has doubled. The telemetry's rows keep only the last warning.
void printMessageKinds() {
    enum { kinds = 96 };
    static uint64_t told[kinds];
    char text[900];
    for (size_t i = 0; i < kinds; ++i) {
        uint64_t count = 0;
        size_t n = native_bridge_metal_message_kind(i, &count, text, sizeof text);
        if (!n)
            break;
        if (count == told[i])
            continue;
        if (!told[i]) {
            if (n >= sizeof text)
                n = sizeof text - 1;
            for (size_t at = 0; at < n; at += 150)
                mlog("msg %zu: %.150s", i, text + at);
        } else if (count < told[i] * 2) {
            continue;
        }
        mlog("msg %zu: x%llu", i, (unsigned long long)count);
        told[i] = count;
    }
}

// Every ten seconds, one or two lines on the kernel's console: what the render server asked of the device (native_bridge_metal_status: commits
// made/failed, completed, pipelines, problems, render passes, draws, full-screen passes, ...) and how the Vulkan pipelines went. It is the
// proof, from the board, that QuartzCore's frames run on the Mali.
void *statusThread(void *) {
    for (;;) {
        sleep(10);
        printMessageKinds();
        char status[512] = {};
        native_bridge_metal_status(status, sizeof status);
        mlog("status: %.180s", status);
        static unsigned tick;
        if (tick++ < 3) {
            logFramebufferChoice("tick");
            clearDeepFramebuffer();
        }
        // Where a frame's time goes on the CPU side of the bridge (totals since start; the difference of two lines is the interval): the
        // GPU's commit-to-completion, the render server's waits, the surface cache maintenance around every commit.
        {
            static char table[4096];
            native_bridge_metal_telemetry(table, sizeof table);
            unsigned commits = 0;
            for (char *at = table; *at;) {
                char *end = at;
                while (*end && *end != '\n')
                    ++end;
                const bool last = !*end;
                *end = 0;
                // G and J: the totals; K: the newest commits (passes, draws, kilopixels, GPU time from commit to callback)
                if (*at == 'G' || *at == 'J' || (*at == 'K' && commits++ < 10))
                    mlog("status: %.100s", at);
                at = last ? end : end + 1;
            }
        }
        uint64_t work[10] = {};
        if (native_bridge_metal_work_stats(work))
            mlog("status: vkpipe n=%llu failed=%llu total=%llums max=%llums, messages info %llu warning %llu, cache hit %llu miss %llu",
                 (unsigned long long)work[0], (unsigned long long)work[4], (unsigned long long)(work[1] / 1000), (unsigned long long)(work[2] / 1000),
                 (unsigned long long)work[5], (unsigned long long)work[6], (unsigned long long)work[7], (unsigned long long)work[8]);
        // Where textures over buffers and surfaces cost copies (totals since start; the difference of two lines is the interval): linear images at
        // Metal's row pitch made / refused / refused again, textures given a shadow image, the shadow copies planned, the texels copied back, passes
        // over linear images in place / through copies of them.
        uint64_t tc[8] = {};
        if (native_bridge_metal_texture_counts(tc))
            mlog("status: L lin %llu/%llu/%llu sh %llu cp %llu bk %llu pass %llu/%llu", (unsigned long long)tc[0], (unsigned long long)tc[1],
                 (unsigned long long)tc[2], (unsigned long long)tc[3], (unsigned long long)tc[4], (unsigned long long)tc[5], (unsigned long long)tc[6],
                 (unsigned long long)tc[7]);
    }
    return nullptr;
}

// mali-timeline=1: every two seconds, a "tl sync" line (the uptime clock in microseconds at that moment: the host stamps the line when it arrives, which
// ties the two clocks together) and then one "tl" line per up to five command buffers completed since: "t age start gpu ctc rp.dr.kpx rep.sub.sch.ca.cb" each (see
// native_bridge_metal_timeline), times in units of 100 us (t: the commit's time on the uptime clock). The kernel's "display gaps" lines carry the frames' times on the same clock, so the command buffers
// of a hitch can be told from the frames the panel got.
void *timelineThread(void *) {
    uint64_t cursor = 0;
    for (;;) {
        usleep(2000000);
        mlog("tl sync %llu", (unsigned long long)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000));
        char text[240];
        while (native_bridge_metal_timeline(&cursor, text, sizeof text))
            mlog("tl %s", text);
    }
    return nullptr;
}

// The kernel's boot-args hold the diagnostics switches (mali-profile=1 starts the display thread's sampling profiler and the CPU-per-thread
// report; they cost a few percent of the display thread, so frame-rate measurements run without).
bool bootArgHas(const char *needle) {
    char args[2048] = {};
    size_t size = sizeof args - 1;
    if (sysctlbyname("kern.bootargs", args, &size, nullptr, 0) != 0)
        return false;
    return strstr(args, needle) != nullptr;
}

// The value of "name=value" in the boot-args (up to the next blank), false when the name is not there.
bool bootArgValue(const char *name, char *out, size_t outSize) {
    char args[2048] = {};
    size_t size = sizeof args - 1;
    if (sysctlbyname("kern.bootargs", args, &size, nullptr, 0) != 0)
        return false;
    const size_t length = strlen(name);
    for (const char *at = args; (at = strstr(at, name)); at += length) {
        if ((at != args && at[-1] != ' ') || at[length] != '=')
            continue;
        const char *value = at + length + 1;
        size_t n = strcspn(value, " ");
        if (n >= outSize)
            n = outSize - 1;
        memcpy(out, value, n);
        out[n] = 0;
        return true;
    }
    return false;
}

// Boot-args mali-dump=FIRST:COUNT (and mali-dump-draws=A:B, mali-dump-image=WHERE,..., mali-dump-match=TEXT): the frontend's own dump of
// what the render server's command buffers FIRST.. rendered (NATIVE_METAL_DUMP_CB: every pass's targets with a brightness grid, the first draws
// of each pass with their pipelines and state). It writes to standard error, which reaches no console in backboardd: a pipe takes over file
// descriptor 2 and a thread forwards its lines to the kernel's console (long lines in pieces).
// A stream for standard error whose writes go to the kernel's console line by line (a pipe or a file for descriptor 2 does not do: the
// render server's sandbox kills the process that makes the pipe or the file -- reason 25/0xdd).
int consoleWrite(void *, const char *data, int length) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static char line[181];
    static size_t have;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < length; ++i) {
        if (data[i] != '\n')
            line[have++] = data[i];
        if ((data[i] == '\n' && have) || have == 180) {
            line[have] = 0;
            mlog("%s", line);
            have = 0;
        }
    }
    pthread_mutex_unlock(&lock);
    return length;
}
// What Mesa's u_trace prints (one line per begin/end of a command buffer, render pass, barrier, cache flush, sync wait, with the GPU's timestamp in
// nanoseconds) goes through here instead of straight to the console: printing every line took longer than the GPU's frames (the trace's buffers ran
// out, the driver's NULL dereference killed backboardd every ten seconds). The lines are paired up in memory and only what costs time comes out:
//   gt cb DURus: R passes/TOTALus (longest LONGUS WxH) wait Nus flush Nus barrier Nus   one per command buffer (as its subqueue saw it)
//   gt R WxH rtN FORMAT tileN DURus                                                     every render pass of 300 us or more
//   gt wait DURus / gt flush DURus / gt barrier DURus                                   a wait, a cache flush or a barrier of 1 ms or more
int traceWrite(void *, const char *data, int length) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static char line[1536];
    static size_t have;
    static uint64_t beginRender, beginCb, beginWait, beginFlush, beginBarrier, beginMeta, beginDispatch;
    static struct { unsigned passes; uint64_t total, longest; unsigned lw, lh; uint64_t wait, flush, barrier; } cb;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < length; ++i) {
        if (data[i] != '\n') {
            if (have < sizeof line - 1)
                line[have++] = data[i];
            continue;
        }
        line[have] = 0;
        have = 0;
        const char *p = line;
        uint64_t ts = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            ts = ts * 10 + static_cast<uint64_t>(*p++ - '0');
            ++digits;
        }
        if (digits != 16)
            continue;
        while (*p == ' ')
            ++p;
        if (*p == '+' || *p == '-')
            ++p;
        while (*p >= '0' && *p <= '9')
            ++p;
        if (p[0] != ':' || p[1] != ' ')
            continue;
        p += 2;
        const char *args = strstr(p, ": ");
        char name[32];
        size_t n = args ? static_cast<size_t>(args - p) : strlen(p);
        if (n >= sizeof name)
            continue;
        memcpy(name, p, n);
        name[n] = 0;
        if (args)
            args += 2;
        if (!strcmp(name, "begin_cmdbuf")) {
            beginCb = ts;
            memset(&cb, 0, sizeof cb);
        } else if (!strcmp(name, "end_cmdbuf") && beginCb) {
            if ((ts - beginCb) >= 2000000 || cb.total >= 1000000)
            mlog("gt cb %lluus: %u passes/%lluus (longest %lluus %ux%u) wait %lluus flush %lluus barrier %lluus",
                 static_cast<unsigned long long>((ts - beginCb) / 1000), cb.passes, static_cast<unsigned long long>(cb.total / 1000),
                 static_cast<unsigned long long>(cb.longest / 1000), cb.lw, cb.lh, static_cast<unsigned long long>(cb.wait / 1000),
                 static_cast<unsigned long long>(cb.flush / 1000), static_cast<unsigned long long>(cb.barrier / 1000));
            beginCb = 0;
        } else if (!strcmp(name, "begin_render")) {
            beginRender = ts;
        } else if (!strcmp(name, "end_render") && beginRender && args) {
            const uint64_t d = ts - beginRender;
            unsigned w = 0, h = 0, rt = 0, tile = 0;
            char format[40] = "?";
            const char *a;
            if ((a = strstr(args, "width=")))
                w = static_cast<unsigned>(atoi(a + 6));
            if ((a = strstr(args, "height=")))
                h = static_cast<unsigned>(atoi(a + 7));
            if ((a = strstr(args, "rt_count=")))
                rt = static_cast<unsigned>(atoi(a + 9));
            if ((a = strstr(args, "tile_size=")))
                tile = static_cast<unsigned>(atoi(a + 10));
            if ((a = strstr(args, "rt0_format=PIPE_FORMAT_"))) {
                a += 23;
                size_t f = 0;
                while (a[f] && a[f] != ',' && f < sizeof format - 1) {
                    format[f] = a[f];
                    ++f;
                }
                format[f] = 0;
            }
            ++cb.passes;
            cb.total += d;
            if (d > cb.longest) {
                cb.longest = d;
                cb.lw = w;
                cb.lh = h;
            }
            if (d >= 300000)
                mlog("gt R %ux%u rt%u %s tile%u %lluus", w, h, rt, format, tile, static_cast<unsigned long long>(d / 1000));
            beginRender = 0;
        } else if (!strcmp(name, "begin_sync64_wait") || !strcmp(name, "begin_sync32_wait")) {
            beginWait = ts;
        } else if ((!strcmp(name, "end_sync64_wait") || !strcmp(name, "end_sync32_wait")) && beginWait) {
            const uint64_t d = ts - beginWait;
            cb.wait += d;
            if (d >= 1000000)
                mlog("gt wait %lluus", static_cast<unsigned long long>(d / 1000));
            beginWait = 0;
        } else if (!strcmp(name, "begin_flush_cache")) {
            beginFlush = ts;
        } else if (!strcmp(name, "end_flush_cache") && beginFlush) {
            const uint64_t d = ts - beginFlush;
            cb.flush += d;
            if (d >= 1000000)
                mlog("gt flush %lluus", static_cast<unsigned long long>(d / 1000));
            beginFlush = 0;
        } else if (!strcmp(name, "begin_meta")) {
            beginMeta = ts;
        } else if (!strcmp(name, "end_meta") && beginMeta) {
            const uint64_t d = ts - beginMeta;
            if (d >= 300000)
                mlog("gt meta %lluus", static_cast<unsigned long long>(d / 1000));
            beginMeta = 0;
        } else if (!strcmp(name, "begin_dispatch")) {
            beginDispatch = ts;
        } else if (!strcmp(name, "end_dispatch") && beginDispatch) {
            const uint64_t d = ts - beginDispatch;
            if (d >= 300000)
                mlog("gt dispatch %lluus", static_cast<unsigned long long>(d / 1000));
            beginDispatch = 0;
        } else if (!strcmp(name, "begin_barrier")) {
            beginBarrier = ts;
        } else if (!strcmp(name, "end_barrier") && beginBarrier) {
            const uint64_t d = ts - beginBarrier;
            cb.barrier += d;
            if (d >= 1000000)
                mlog("gt barrier %lluus", static_cast<unsigned long long>(d / 1000));
            beginBarrier = 0;
        }
    }
    pthread_mutex_unlock(&lock);
    return length;
}
void startDumpFromBootArgs() {
    char value[256];
    if (bootArgValue("mali-skip-draws", value, sizeof value)) {
        for (char *c = value; *c; ++c)
            if (*c == ',')
                *c = ':';   // PASS:FIRST:LAST: the draws of every command buffer's render pass PASS left out
        setenv("NATIVE_METAL_SKIP_DRAWS", value, 1);
    }
    // mali-panvk-debug=noafbc,linear,...: PANVK_DEBUG for the render server's driver (comma list; noafbc / linear change the images' layout, startup
    // prints the flags). Not force_blackhole: that one maps sparse buffers and images to a black-hole page, it does not skip any shading.
    if (bootArgValue("mali-panvk-debug", value, sizeof value))
        setenv("PANVK_DEBUG", value, 1);
    // mali-panvk-fpk=bound: PANVK_FPK=bound, the driver's old rule for forward pixel kill (a draw kills the fragments of earlier draws only if it writes every
    // bound color attachment; the default lets a draw that leaves one unwritten kill too, CoreAnimation's passes bind a write-masked memoryless second one).
    // An A/B switch for the frame-rate measurements.
    if (bootArgValue("mali-panvk-fpk", value, sizeof value))
        setenv("PANVK_FPK", value, 1);
    // mali-bifrost-debug=shaderdb or statsfull: BIFROST_MESA_DEBUG for the render server's shader compiler. Every shader the driver compiles prints
    // its statistics to standard error (the console below): shaderdb one line (instructions, cycles of the arithmetic, load/store, varying and
    // texture pipes, work registers, threads, spills), statsfull a block with the pipe each shader is bound by.
    if (bootArgValue("mali-bifrost-debug", value, sizeof value))
        setenv("BIFROST_MESA_DEBUG", value, 1);
    // mali-skip-format=N: every draw that has a fragment texture of Metal pixel format N bound (500 is the two-plane YCbCr of the wallpaper) is left
    // out (NATIVE_METAL_SKIP_FORMAT): the frame's GPU time without what samples such textures, wherever in a pass those draws lie.
    if (bootArgValue("mali-skip-format", value, sizeof value))
        setenv("NATIVE_METAL_SKIP_FORMAT", value, 1);
    // mali-skip-pipeline=PATTERN/PATTERN...: every draw made with a render pipeline whose label (the part after the last dot, e.g. PBGRAXm_A2Xhf, the
    // plain fills of CoreAnimation) matches a pattern is left out (NATIVE_METAL_SKIP_PIPELINE; a dot in a pattern stands for any text: ".Tds4." is every
    // label with Tds4 in it): the frame's GPU time without that kind of draw. mali-list-pipelines=1 prints each distinct label as it is made.
    if (bootArgValue("mali-skip-pipeline", value, sizeof value))
        setenv("NATIVE_METAL_SKIP_PIPELINE", value, 1);
    if (bootArgValue("mali-list-pipelines", value, sizeof value))
        setenv("NATIVE_METAL_LIST_PIPELINES", "1", 1);
    // mali-trace=print: Mesa's u_trace (MESA_GPU_TRACES) in the render server's driver: a line at the start and the end of every command buffer,
    // render pass (size, formats, tile size), barrier, cache flush and sync wait, each with the GPU's own timestamps. It prints to stdout, which
    // gets the same console stream as standard error below.
    const bool trace = bootArgValue("mali-trace", value, sizeof value);
    if (trace)
        setenv("MESA_GPU_TRACES", value, 1);
    if (trace) {
        setenv("PANVK_UTRACE_CLONE_MEM_SIZE", "33554432", 0);   // the trace's timestamp buffers (default 10 MiB ran out)
        FILE *traceOut = funopen(nullptr, nullptr, traceWrite, nullptr, nullptr);
        if (traceOut) {
            setvbuf(traceOut, nullptr, _IOLBF, 0);
            stdout = traceOut;
            mlog("gpu trace: MESA_GPU_TRACES=%s, the driver's trace lines are summed up here as 'gt' lines", getenv("MESA_GPU_TRACES"));
        }
    }
    // (the shader compiler's statistics, mali-bifrost-debug, go to standard error as well: the console stream is not only for the trace)
    if ((trace || bootArgValue("mali-bifrost-debug", value, sizeof value)) && !bootArgValue("mali-dump", value, sizeof value)) {
        FILE *console = funopen(nullptr, nullptr, consoleWrite, nullptr, nullptr);
        if (console) {
            setvbuf(console, nullptr, _IOLBF, 0);
            stderr = console;
        }
        return;
    }
    if (!bootArgValue("mali-dump", value, sizeof value))
        return;
    // (the boot-args cannot hold a colon: FIRST,COUNT stands for FIRST:COUNT)
    for (char *c = value; *c; ++c)
        if (*c == ',')
            *c = ':';
    setenv("NATIVE_METAL_DUMP_CB", value, 1);
    if (bootArgValue("mali-dump-draws", value, sizeof value)) {
        for (char *c = value; *c; ++c)
            if (*c == ',')
                *c = ':';
        setenv("NATIVE_METAL_DUMP_DRAWS", value, 1);
    }
    if (bootArgValue("mali-dump-image", value, sizeof value)) {
        for (char *c = value; *c; ++c)
            if (*c == '_')
                *c = ' ';   // ("pass_2_color_0": a place has blanks)
        setenv("NATIVE_METAL_DUMP_IMAGE", value, 1);
    }

    if (bootArgValue("mali-dump-match", value, sizeof value))
        setenv("NATIVE_METAL_DUMP_DRAWS_MATCH", value, 1);
    if (bootArgValue("mali-dump-words", value, sizeof value))
        setenv("NATIVE_METAL_DUMP_WORDS", value, 1);
    FILE *console = funopen(nullptr, nullptr, consoleWrite, nullptr, nullptr);
    if (!console) {
        mlog("dump: no stream (errno %d)", errno);
        return;
    }
    setvbuf(console, nullptr, _IOLBF, 0);
    stderr = console;
    mlog("dump: command buffers %s of the render server; what its frontend writes to standard error comes to this console", getenv("NATIVE_METAL_DUMP_CB"));
}

// Where the render server's CPU time goes: every two seconds the threads that used the most CPU in the interval (milliseconds, user+system),
// with their names. The GPU is busy a quarter of the time during frames, so the frame rate is set by what runs on the CPU.
void *cpuThread(void *) {
    struct Seen {
        uint64_t id;
        uint64_t us;
    };
    static Seen seen[256];
    static unsigned seenCount;
    for (;;) {
        sleep(2);
        thread_act_array_t threads = nullptr;
        mach_msg_type_number_t count = 0;
        if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
            continue;
        struct Row {
            uint64_t ms;
            uint64_t id;
            char name[40];
        } top[6] = {};
        uint64_t total = 0;
        for (mach_msg_type_number_t i = 0; i < count; ++i) {
            thread_identifier_info_data_t ident = {};
            mach_msg_type_number_t identCount = THREAD_IDENTIFIER_INFO_COUNT;
            thread_basic_info_data_t basic = {};
            mach_msg_type_number_t basicCount = THREAD_BASIC_INFO_COUNT;
            if (thread_info(threads[i], THREAD_IDENTIFIER_INFO, reinterpret_cast<thread_info_t>(&ident), &identCount) == KERN_SUCCESS &&
                thread_info(threads[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&basic), &basicCount) == KERN_SUCCESS) {
                const uint64_t us = (uint64_t)basic.user_time.seconds * 1000000 + basic.user_time.microseconds +
                                    (uint64_t)basic.system_time.seconds * 1000000 + basic.system_time.microseconds;
                unsigned at = 0;
                while (at < seenCount && seen[at].id != ident.thread_id)
                    ++at;
                uint64_t before = 0;
                if (at < seenCount)
                    before = seen[at].us;
                else if (seenCount < 256)
                    seen[seenCount++] = Seen{ident.thread_id, 0};
                else
                    at = 255;
                seen[at].us = us;
                const uint64_t ms = us > before ? (us - before) / 1000 : 0;
                total += ms;
                unsigned worst = 0;
                for (unsigned k = 1; k < 6; ++k)
                    if (top[k].ms < top[worst].ms)
                        worst = k;
                if (ms > top[worst].ms) {
                    top[worst].ms = ms;
                    top[worst].id = ident.thread_id;
                    top[worst].name[0] = 0;
                    pthread_t p = pthread_from_mach_thread_np(threads[i]);
                    if (p)
                        pthread_getname_np(p, top[worst].name, sizeof top[worst].name);
                }
            }
            mach_port_deallocate(mach_task_self(), threads[i]);
        }
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_t));
        if (total < 40)
            continue;
        for (unsigned a = 0; a < 6; ++a)
            for (unsigned b = a + 1; b < 6; ++b)
                if (top[b].ms > top[a].ms)
                    std::swap(top[a], top[b]);
        mlog("cpu 2s: all %llums | %s %llu | %s %llu | %s %llu | %s %llu | %s %llu", (unsigned long long)total, top[0].name[0] ? top[0].name : "?", (unsigned long long)top[0].ms,
             top[1].name[0] ? top[1].name : "?", (unsigned long long)top[1].ms, top[2].name[0] ? top[2].name : "?", (unsigned long long)top[2].ms,
             top[3].name[0] ? top[3].name : "?", (unsigned long long)top[3].ms, top[4].name[0] ? top[4].name : "?", (unsigned long long)top[4].ms);
    }
    return nullptr;
}

// A sampling profiler for the render server's display thread (the CPU-bound one): 250 times a second it stops the thread, reads its program
// counter and walks the frame pointers, and every six seconds says where the samples fell: by image (self and inclusive of callers) and by
// function. It is the answer to "QuartzCore, our plugin, the Vulkan driver or the C library?".
std::string imageOf(const void *address, std::string *symbol, uintptr_t *offset) {
    Dl_info info = {};
    if (!dladdr(address, &info) || !info.dli_fname) {
        *symbol = "?";
        *offset = 0;
        return "?";
    }
    const char *slash = strrchr(info.dli_fname, '/');
    std::string image = slash ? slash + 1 : info.dli_fname;
    char text[96];
    if (info.dli_sname && info.dli_sname[0]) {
        snprintf(text, sizeof text, "%.80s", info.dli_sname);
    } else {
        snprintf(text, sizeof text, "+%#lx", (unsigned long)((reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(info.dli_fbase)) >> 8 << 8));
    }
    *symbol = text;
    *offset = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(info.dli_fbase);
    return image;
}

// Rust symbols come mangled (v0: _RNv..., legacy: _ZN...); this keeps the last three path segments, enough to tell the functions apart.
std::string shortName(const std::string &name) {
    std::vector<std::string> parts;
    if (name.compare(0, 2, "_R") == 0) {
        size_t i = 2;
        while (i < name.size()) {
            const char c = name[i];
            if (c == 'C' && i + 1 < name.size() && name[i + 1] == 's') {   // crate disambiguator: Cs<base62>_
                while (i < name.size() && name[i] != '_')
                    ++i;
                ++i;
            } else if (c == 'B') {   // back reference: B<base62>_
                while (i < name.size() && name[i] != '_')
                    ++i;
                ++i;
            } else if (c >= '0' && c <= '9') {
                size_t n = 0;
                while (i < name.size() && name[i] >= '0' && name[i] <= '9')
                    n = n * 10 + (size_t)(name[i++] - '0');
                if (i < name.size() && name[i] == '_')
                    ++i;
                if (n && i + n <= name.size())
                    parts.push_back(name.substr(i, n));
                i += n;
            } else {
                ++i;
            }
        }
    } else if (name.compare(0, 3, "_ZN") == 0) {
        size_t i = 3;
        while (i < name.size() && name[i] >= '0' && name[i] <= '9') {
            size_t n = 0;
            while (i < name.size() && name[i] >= '0' && name[i] <= '9')
                n = n * 10 + (size_t)(name[i++] - '0');
            if (i + n > name.size())
                break;
            const std::string piece = name.substr(i, n);
            if (!(piece.size() == 17 && piece[0] == 'h'))
                parts.push_back(piece);
            i += n;
        }
    }
    if (parts.empty())
        return name;
    std::string out;
    for (size_t k = parts.size() > 3 ? parts.size() - 3 : 0; k < parts.size(); ++k)
        out += (out.empty() ? "" : "::") + parts[k];
    return out;
}

void profileReport(const char *what, std::map<std::string, unsigned> &counts, unsigned total, unsigned entries, unsigned perLine = 3, unsigned nameLength = 46) {
    std::vector<std::pair<unsigned, std::string>> sorted;
    for (auto &entry : counts)
        sorted.push_back({entry.second, entry.first});
    std::sort(sorted.begin(), sorted.end(), [](const std::pair<unsigned, std::string> &a, const std::pair<unsigned, std::string> &b) { return a.first > b.first; });
    char line[400];
    size_t used = 0;
    unsigned shown = 0, inLine = 0;
    for (auto &entry : sorted) {
        if (shown >= entries)
            break;
        const int n = snprintf(line + used, sizeof line - used, "%s%.*s %u%%", inLine ? " | " : "", (int)nameLength, entry.second.c_str(), entry.first * 100 / (total ? total : 1));
        if (n <= 0 || used + (size_t)n >= 170)
            break;
        used += (size_t)n;
        ++shown;
        if (++inLine == perLine) {
            mlog("prof %s: %s", what, line);
            used = 0;
            inLine = 0;
            line[0] = 0;
        }
    }
    if (inLine)
        mlog("prof %s: %s", what, line);
}

void *profileThread(void *) {
    thread_t target = MACH_PORT_NULL;
    pthread_t targetPthread = nullptr;
    std::map<std::string, unsigned> leafImage, inclusive, leafSymbol, plugin, pluginInclusive;
    unsigned running = 0, samples = 0;
    uint64_t lastReport = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), lastLookup = 0;
    for (;;) {
        usleep(4000);
        const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (target == MACH_PORT_NULL || now - lastLookup > 10000000000ull) {
            lastLookup = now;
            thread_act_array_t threads = nullptr;
            mach_msg_type_number_t count = 0;
            if (task_threads(mach_task_self(), &threads, &count) == KERN_SUCCESS) {
                thread_t found = MACH_PORT_NULL;
                for (mach_msg_type_number_t i = 0; i < count; ++i) {
                    char name[64] = {};
                    pthread_t p = pthread_from_mach_thread_np(threads[i]);
                    if (p && !found && pthread_getname_np(p, name, sizeof name) == 0 && strcmp(name, "com.apple.coreanimation.display.primary") == 0) {
                        found = threads[i];
                        targetPthread = p;
                        continue;
                    }
                    mach_port_deallocate(mach_task_self(), threads[i]);
                }
                vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_t));
                if (found) {
                    if (target != MACH_PORT_NULL)
                        mach_port_deallocate(mach_task_self(), target);
                    target = found;
                }
            }
        }
        if (target != MACH_PORT_NULL) {
            thread_basic_info_data_t basic = {};
            mach_msg_type_number_t basicCount = THREAD_BASIC_INFO_COUNT;
            ++samples;
            if (thread_info(target, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&basic), &basicCount) == KERN_SUCCESS && basic.run_state == TH_STATE_RUNNING &&
                thread_suspend(target) == KERN_SUCCESS) {
                arm_thread_state64_t state = {};
                mach_msg_type_number_t stateCount = ARM_THREAD_STATE64_COUNT;
                const kern_return_t got = thread_get_state(target, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &stateCount);
                uintptr_t pc = 0, fp = 0, lr = 0;
                if (got == KERN_SUCCESS) {
                    pc = (uintptr_t)(state.__pc & 0x7fffffffffull);
                    fp = (uintptr_t)(state.__fp & 0x7fffffffffull);
                    lr = (uintptr_t)(state.__lr & 0x7fffffffffull);
                }
                const uintptr_t top = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(targetPthread));
                const uintptr_t bottom = top - pthread_get_stacksize_np(targetPthread);
                // nothing here may allocate or take a lock: the stopped thread might hold the allocator's
                uintptr_t chain[48];
                unsigned chainLength = 0;
                if (pc) {
                    chain[chainLength++] = pc;
                    if (lr)
                        chain[chainLength++] = lr;
                    for (unsigned depth = 0; depth < 44 && fp >= bottom && fp + 16 <= top && !(fp & 7); ++depth) {
                        const uintptr_t next = *reinterpret_cast<uintptr_t *>(fp), ret = *reinterpret_cast<uintptr_t *>(fp + 8) & 0x7fffffffffull;
                        if (ret)
                            chain[chainLength++] = ret;
                        if (next <= fp)
                            break;
                        fp = next & 0x7fffffffffull;
                    }
                }
                thread_resume(target);
                if (chainLength) {
                    ++running;
                    std::string symbol;
                    uintptr_t offset;
                    const std::string leaf = imageOf(reinterpret_cast<void *>(chain[0]), &symbol, &offset);
                    ++leafImage[leaf];
                    ++leafSymbol[leaf + ":" + symbol];
                    std::set<std::string> seen;
                    for (unsigned c = 0; c < chainLength; ++c) {
                        const uintptr_t address = chain[c];
                        std::string s2;
                        uintptr_t o2;
                        seen.insert(imageOf(reinterpret_cast<void *>(address), &s2, &o2));
                    }
                    for (const std::string &name : seen)
                        ++inclusive[name];
                    // which functions of our plugin the sample is under: the nearest one (self + callees) and every one on the stack (inclusive)
                    std::set<std::string> functions;
                    bool nearest = true;
                    for (unsigned c = 0; c < chainLength; ++c) {
                        const uintptr_t address = chain[c];
                        std::string s2;
                        uintptr_t o2;
                        if (imageOf(reinterpret_cast<void *>(address), &s2, &o2) == "NativeMaliMetal") {
                            const std::string name = shortName(s2);
                            if (nearest)
                                ++plugin[name];
                            nearest = false;
                            functions.insert(name);
                        }
                    }
                    for (const std::string &name : functions)
                        ++pluginInclusive[name];
                }
            }
        }
        if (now - lastReport >= 6000000000ull) {
            lastReport = now;
            if (running >= 25) {
                mlog("prof: %u running samples of %u (display thread)", running, samples);
                profileReport("self by image", leafImage, running, 8, 4);
                profileReport("inclusive images", inclusive, running, 8, 4);
                profileReport("self by function", leafSymbol, running, 9, 3, 50);
                profileReport("plugin nearest", plugin, running, 12, 2, 70);
                profileReport("plugin inclusive", pluginInclusive, running, 14, 2, 70);
            }
            leafImage.clear();
            inclusive.clear();
            leafSymbol.clear();
            plugin.clear();
            pluginInclusive.clear();
            running = samples = 0;
        }
    }
    return nullptr;
}

// mali-plugin-stacks=Name1,Name2: in the processes of those names (a prefix is enough), the stacks of every thread, 15, 40 and 90 seconds after the
// plugin loaded. Frames are named from the exported symbols (dladdr) and, for the Objective-C methods the shared cache leaves unnamed, from the
// runtime's method tables: "-[Class selector]+offset". For finding where a process that should be drawing is waiting.
struct ObjcMethod {
    uintptr_t imp;
    const char *kind;   // "-" or "+"
    const char *cls;
    const char *sel;
};

std::vector<ObjcMethod> &objcMethods() {
    static std::vector<ObjcMethod> methods;
    static bool built;
    if (built)
        return methods;
    built = true;
    unsigned classCount = 0;
    Class *classes = objc_copyClassList(&classCount);
    for (unsigned i = 0; i < classCount; ++i) {
        for (int meta = 0; meta < 2; ++meta) {
            Class cls = meta ? object_getClass(classes[i]) : classes[i];
            unsigned count = 0;
            Method *list = class_copyMethodList(cls, &count);
            for (unsigned m = 0; m < count; ++m) {
                const uintptr_t imp = reinterpret_cast<uintptr_t>(method_getImplementation(list[m])) & 0x7fffffffffull;
                methods.push_back({imp, meta ? "+" : "-", class_getName(classes[i]), sel_getName(method_getName(list[m]))});
            }
            free(list);
        }
    }
    free(classes);
    std::sort(methods.begin(), methods.end(), [](const ObjcMethod &a, const ObjcMethod &b) { return a.imp < b.imp; });
    return methods;
}

std::string describeFrame(uintptr_t address) {
    Dl_info info = {};
    char text[200];
    if (!dladdr(reinterpret_cast<void *>(address), &info) || !info.dli_fname) {
        snprintf(text, sizeof text, "%#lx", (unsigned long)address);
        return text;
    }
    const char *slash = strrchr(info.dli_fname, '/');
    const char *image = slash ? slash + 1 : info.dli_fname;
    const uintptr_t offset = address - reinterpret_cast<uintptr_t>(info.dli_fbase);
    // the nearest Objective-C method at or below the address, if it is closer than the exported symbol
    std::vector<ObjcMethod> &methods = objcMethods();
    auto above = std::upper_bound(methods.begin(), methods.end(), address, [](uintptr_t a, const ObjcMethod &m) { return a < m.imp; });
    const ObjcMethod *best = above == methods.begin() ? nullptr : &*(above - 1);
    uintptr_t symbolAddress = info.dli_saddr ? reinterpret_cast<uintptr_t>(info.dli_saddr) : 0;
    if (best && address - best->imp < 0x4000 && best->imp >= reinterpret_cast<uintptr_t>(info.dli_fbase) && best->imp > symbolAddress) {
        snprintf(text, sizeof text, "%s[%s %s]+%#lx", best->kind, best->cls, best->sel, (unsigned long)(address - best->imp));
        return text;
    }
    if (info.dli_sname && info.dli_sname[0]) {
        snprintf(text, sizeof text, "%s!%.60s+%#lx", image, info.dli_sname, (unsigned long)(address - symbolAddress));
        return text;
    }
    snprintf(text, sizeof text, "%s+%#lx", image, (unsigned long)offset);
    return text;
}

void dumpStacks(const char *when) {
    struct Raw {
        thread_t thread;
        char name[48];
        int runState;
        uintptr_t chain[26];
        unsigned length;
    };
    static Raw raw[96];
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
        return;
    const thread_t self = mach_thread_self();
    unsigned n = 0;
    uintptr_t tops[96], bottoms[96];
    for (mach_msg_type_number_t i = 0; i < count && n < 96; ++i) {
        if (threads[i] == self)
            continue;
        Raw &r = raw[n];
        r = {};
        r.thread = threads[i];
        pthread_t p = pthread_from_mach_thread_np(threads[i]);
        if (p) {
            pthread_getname_np(p, r.name, sizeof r.name);
            tops[n] = reinterpret_cast<uintptr_t>(pthread_get_stackaddr_np(p));
            bottoms[n] = tops[n] - pthread_get_stacksize_np(p);
        } else {
            tops[n] = bottoms[n] = 0;
        }
        thread_basic_info_data_t basic = {};
        mach_msg_type_number_t basicCount = THREAD_BASIC_INFO_COUNT;
        if (thread_info(threads[i], THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&basic), &basicCount) == KERN_SUCCESS)
            r.runState = basic.run_state;
        ++n;
    }
    // Build the method table before anything is stopped: it allocates.
    objcMethods();
    for (unsigned i = 0; i < n; ++i) {
        Raw &r = raw[i];
        if (thread_suspend(r.thread) != KERN_SUCCESS)
            continue;
        arm_thread_state64_t state = {};
        mach_msg_type_number_t stateCount = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(r.thread, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &stateCount) == KERN_SUCCESS) {
            uintptr_t fp = (uintptr_t)(state.__fp & 0x7fffffffffull);
            r.chain[r.length++] = (uintptr_t)(state.__pc & 0x7fffffffffull);
            r.chain[r.length++] = (uintptr_t)(state.__lr & 0x7fffffffffull);
            for (unsigned depth = 0; depth < 22 && fp >= bottoms[i] && fp + 16 <= tops[i] && !(fp & 7); ++depth) {
                const uintptr_t next = *reinterpret_cast<uintptr_t *>(fp), ret = *reinterpret_cast<uintptr_t *>(fp + 8) & 0x7fffffffffull;
                if (ret)
                    r.chain[r.length++] = ret;
                if (next <= fp)
                    break;
                fp = next & 0x7fffffffffull;
            }
        }
        thread_resume(r.thread);
    }
    mlog("stacks %s: %u threads", when, n);
    for (unsigned i = 0; i < n; ++i) {
        Raw &r = raw[i];
        mlog("stack %u '%.30s' state %d:", i, r.name, r.runState);
        std::string line;
        for (unsigned f = 0; f < r.length; ++f) {
            const std::string frame = describeFrame(r.chain[f]);
            if (line.size() + frame.size() + 3 > 150) {
                mlog("  %s", line.c_str());
                line.clear();
            }
            line += (line.empty() ? "" : " < ") + frame;
        }
        if (!line.empty())
            mlog("  %s", line.c_str());
        mach_port_deallocate(mach_task_self(), r.thread);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_t));
    mach_port_deallocate(mach_task_self(), self);
}

void *stackThread(void *) {
    static const unsigned delays[3] = {15, 25, 50};
    for (unsigned i = 0; i < 3; ++i) {
        sleep(delays[i]);
        char when[32];
        snprintf(when, sizeof when, "#%u", i + 1);
        dumpStacks(when);
    }
    return nullptr;
}

bool stacksWanted() {
    char args[1024] = {};
    size_t size = sizeof args - 1;
    if (sysctlbyname("kern.bootargs", args, &size, nullptr, 0) != 0)
        return false;
    const char *at = strstr(args, "mali-plugin-stacks=");
    if (!at)
        return false;
    at += strlen("mali-plugin-stacks=");
    const char *end = at;
    while (*end && *end != ' ')
        ++end;
    const std::string list(at, end);
    const std::string me = getprogname();
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        if (comma == std::string::npos)
            comma = list.size();
        const std::string name = list.substr(start, comma - start);
        if (!name.empty() && me.compare(0, name.size(), name) == 0)
            return true;
        start = comma + 1;
    }
    return false;
}

// A client process lives under a memory limit (200 MB for the poster extensions: "crossed memory high watermark", then jetsam kills it, gpu41), and the
// GPU's buffers are mapped into it: once a second for the first half minute, the footprint, in MB.
void *gIcdHandle;   // the driver library, loaded in openDevice: what it can say about its calls into the kernel driver

void logMemory(const char *when) {
    {
        task_vm_info_data_t info = {};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS)
            mlog("mem %s: footprint %llu MB (peak %llu), internal %llu, device %llu, external %llu, compressed %llu, resident %llu, graphics %lld, virtual %llu",
                 when, (unsigned long long)(info.phys_footprint >> 20), (unsigned long long)(info.ledger_phys_footprint_peak >> 20), (unsigned long long)(info.internal >> 20),
                 (unsigned long long)(info.device >> 20), (unsigned long long)(info.external >> 20), (unsigned long long)(info.compressed >> 20),
                 (unsigned long long)(info.resident_size >> 20), (long long)(info.ledger_tag_graphics_footprint >> 20), (unsigned long long)(info.virtual_size >> 20));
    }
}

void *memoryThread(void *) {
    for (unsigned i = 0; i < 40; ++i) {
        logMemory("t");
        typedef size_t (*TimingFn)(char *, size_t);
        TimingFn timing = gIcdHandle ? reinterpret_cast<TimingFn>(dlsym(gIcdHandle, "drm_darwin_timing")) : nullptr;
        char text[150];
        if (timing && timing(text, sizeof text))
            mlog("calls:%s", text);
        sleep(1);
    }
    return nullptr;
}

// The processes other than the render server that get a device of their own (the same runtime on its own connection; the render server's
// framebuffer choice, telemetry thread and display hand-off do not apply to them): the wallpaper extensions of the lock screen, which draw
// with Metal and crash without a device (AegirPoster and KaleidoscopePoster in xnu-probe/runs/gpu28).
bool clientProcess() {
    // Which of the posters is the one on the lock screen depends on the Data volume (a fresh one picks the collections / photos posters, an older
    // one the Aegir and Kaleidoscope ones), so every poster extension of the system gets a device; the idle ones never ask for it.
    static const char *const names[] = {
        "SpringBoard", "PosterBoard", "WidgetRenderer_D", "AegirPoster", "KaleidoscopePoster", "CollectionsPoster", "PhotosPosterProvider", "MercuryPosterExtension",
        "GradientPosterExtension", "EmojiPosterExtension", "ExtragalacticPoster", "HelloPosterExtension",
        "PridePosterExtension", "UnityPosterExtension"};
    for (const char *name : names)
        if (strcmp(getprogname(), name) == 0)
            return true;
    return false;
}


// ---- The display's GPU hand-off. With the display driver in GPU mode (boot-arg sheng-fb-gpu=1) a finished frame is not copied into the scanout buffer
// by the CPU (8 MB, about 40 ms through write-combined memory: 25 frames a second at most) but offered to this thread: the frame's pages arrive mapped
// into this process, the GPU copies them into the scanout buffer (also mapped here), and completing the frame retires it. The kernel side is
// GopFramebuffer's NativeGOPPresenter, reached through Mali (MALI_GOP, MALI_MAP_SCANOUT). Without the arg the first call is refused
// and this thread ends; while this thread is not waiting the display driver copies with the CPU, as before.
constexpr int kIOReturnTimeoutCode = (int)0xe00002d6;   // kIOReturnTimeout: nothing was offered in the time given (an idle display)

void *presenterThread(void *argument) {
    id<MTLDevice> device = (id<MTLDevice>)CFBridgingRelease(argument);
    IOKit &k = iokit();
    if (!k.matching || !k.getService || !k.open || !k.call || !k.map) {
        mlog("gop: IOKit is not usable");
        return nullptr;
    }
    unsigned connection = 0;
    if (const unsigned service = k.getService(0, k.matching(MALI_SERVICE)))
        k.open(service, mach_task_self(), 0, &connection);
    if (!connection) {
        mlog("gop: no connection to the Mali service");
        return nullptr;
    }
    mali_gop_in in = {MALI_GOP_MODE, 0, 0};
    mali_gop_out out = {};
    size_t outSize = 0;
    int r = -1;
    for (unsigned tries = 0; tries < 10; ++tries) {   // the hand-off appears with the display driver, which may start after the plugin
        outSize = sizeof out;
        r = k.call(connection, MALI_GOP, &in, sizeof in, &out, &outSize);
        if (r == 0)
            break;
        sleep(2);
    }
    if (r != 0) {
        mlog("gop: no GPU hand-off in the display driver (%#x): the CPU copies its frames", r);
        return nullptr;
    }
    const mali_gop_mode mode = out.mode;
    mlog("gop: mode %llux%llu stride %llu, %llu bytes, format %u", (unsigned long long)mode.width, (unsigned long long)mode.height,
         (unsigned long long)mode.stride, (unsigned long long)mode.bytes, mode.format);
    uint64_t scanAddress = 0, scanSize = 0;
    r = k.map(connection, MALI_MAP_SCANOUT, mach_task_self(), &scanAddress, &scanSize, 1 /* kIOMapAnywhere */ | 0x400 /* kIOMapWriteCombineCache */);
    if (r != 0 || !scanAddress) {
        mlog("gop: the scanout buffer is not mappable (%#x)", r);
        return nullptr;
    }
    const uint64_t scanBytes = (mode.bytes + 0xfff) & ~0xfffull;
    id<MTLBuffer> scanout = [device newBufferWithBytesNoCopy:(void *)(uintptr_t)scanAddress length:scanBytes options:MTLResourceStorageModeShared deallocator:nil];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!scanout || !queue) {
        mlog("gop: no GPU buffer over the scanout (%p) or no queue (%p)", (__bridge void *)scanout, (__bridge void *)queue);
        return nullptr;
    }
    mlog("gop: scanout mapped at %#llx (%llu bytes), the GPU copies the frames", (unsigned long long)scanAddress, (unsigned long long)scanSize);

    uint64_t frames = 0, failures = 0, importNs = 0, copyNs = 0, completeNs = 0, maxTotal = 0, sumTotal = 0, windowFrames = 0, last = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint64_t encodeNs = 0, commitNs = 0, waitNs = 0, releaseNs = 0, maxWait = 0;
    typedef size_t (*TimingFn)(char *, size_t);
    TimingFn timing = gIcdHandle ? reinterpret_cast<TimingFn>(dlsym(gIcdHandle, "drm_darwin_timing")) : nullptr;
    for (unsigned errors = 0; errors < 50;) {
        in = {MALI_GOP_WAIT, 0, 100};
        outSize = sizeof out;
        r = k.call(connection, MALI_GOP, &in, sizeof in, &out, &outSize);
        if (r == kIOReturnTimeoutCode)
            continue;
        if (r != 0) {
            if (++errors < 5)
                mlog("gop: wait for a frame failed (%#x)", r);
            usleep(20000);
            continue;
        }
        const mali_gop_swap swap = out.swap;
        const uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        uint32_t status = 1;
        @autoreleasepool {
            const uint64_t rowBytes = (uint64_t)swap.width * 4;
            const uint64_t srcBytes = (swap.bytes + 0xfff) & ~0xfffull;
            id<MTLBuffer> frame = [device newBufferWithBytesNoCopy:(void *)(uintptr_t)swap.address length:srcBytes options:MTLResourceStorageModeShared deallocator:nil];
            const uint64_t imported = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            importNs += imported - began;
            if (frame && swap.width == mode.width && swap.height == mode.height && swap.stride >= rowBytes) {
                const uint64_t e0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                id<MTLCommandBuffer> commands = [queue commandBuffer];
                id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
                if (swap.stride == mode.stride && swap.stride * swap.height <= swap.bytes) {
                    [blit copyFromBuffer:frame sourceOffset:0 toBuffer:scanout destinationOffset:0 size:swap.stride * swap.height];
                } else {
                    for (uint32_t y = 0; y < swap.height; ++y)
                        [blit copyFromBuffer:frame sourceOffset:y * swap.stride toBuffer:scanout destinationOffset:y * mode.stride size:rowBytes];
                }
                [blit endEncoding];
                const uint64_t e1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                [commands commit];
                const uint64_t e2 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                [commands waitUntilCompleted];
                const uint64_t e3 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                status = commands.status == MTLCommandBufferStatusCompleted ? 0 : 2;
                encodeNs += e1 - e0;
                commitNs += e2 - e1;
                waitNs += e3 - e2;
                if (e3 - e2 > maxWait)
                    maxWait = e3 - e2;
            }
            const uint64_t r0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            frame = nil;
            releaseNs += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - r0;
        }
        const uint64_t copied = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        copyNs += copied - began;
        in = {MALI_GOP_COMPLETE, status, swap.sequence};
        outSize = 0;
        k.call(connection, MALI_GOP, &in, sizeof in, nullptr, nullptr);
        const uint64_t done = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        completeNs += done - copied;
        ++frames;
        ++windowFrames;
        sumTotal += done - began;
        if (done - began > maxTotal)
            maxTotal = done - began;
        if (status)
            ++failures;
        if (frames == 1 || done - last >= 10000000000ull) {
            mlog("gop: %llu frames (%llu in the last %llu ms), %llu failed; per frame: import %llu us, copy+wait %llu us, complete %llu us, total %llu us (longest %llu us)",
                 (unsigned long long)frames, (unsigned long long)windowFrames, (unsigned long long)((done - last) / 1000000), (unsigned long long)failures,
                 (unsigned long long)(importNs / frames / 1000), (unsigned long long)((copyNs - importNs) / frames / 1000), (unsigned long long)(completeNs / frames / 1000),
                 (unsigned long long)(sumTotal / frames / 1000), (unsigned long long)(maxTotal / 1000));
            mlog("gop: per frame: encode %llu us, commit %llu us, GPU wait %llu us (longest %llu us), release %llu us", (unsigned long long)(encodeNs / frames / 1000),
                 (unsigned long long)(commitNs / frames / 1000), (unsigned long long)(waitNs / frames / 1000), (unsigned long long)(maxWait / 1000),
                 (unsigned long long)(releaseNs / frames / 1000));
            char text[150];
            if (timing && timing(text, sizeof text))
                mlog("gop: calls so far:%s", text);
            last = done;
            windowFrames = 0;
        }
    }
    mlog("gop: giving up after %llu frames", (unsigned long long)frames);
    return nullptr;
}

id openDevice(unsigned port) {
    static id device;
    static bool tried;
    @synchronized([NativeMaliMetalDevice class]) {
        if (tried)
            return device;
        tried = true;

        // The discovery releases the accelerator port when it is done.
        IOKit &k = iokit();
        if (k.retain)
            k.retain(port);
        const uint64_t began = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
        if (strcmp(getprogname(), "backboardd") != 0)
            logMemory("entry");
        mlog("metal: plugin in %s (pid %d), accelerator port %#x", getprogname(), getpid(), port);
        const bool server = strcmp(getprogname(), "backboardd") == 0;
        if (server)
            logFramebufferChoice("open");

        char dir[512];
        char icd[640];
        if (!bundleDirectory(dir, sizeof dir)) {
            mlog("metal: no bundle directory");
            return nil;
        }
        snprintf(icd, sizeof icd, "%s/libvulkan_panfrost.dylib", dir);
        if (access(icd, R_OK) != 0) {
            mlog("metal: no PanVK at %s (errno %d)", icd, errno);
            return nil;
        }
        uint64_t registryID = 0;
        if (k.entryID)
            k.entryID(port, &registryID);

        // The render server compresses its framebuffer (an Apple/AGX layout this GPU does not write) unless told not to; the display driver takes
        // plain rows.
        setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 1);
        // The runtime's creation log reports each VkPipeline making's time to the device's sink, which the status line's vkpipe row sums.
        setenv("METAL2VULKAN_LOG_PIPELINES", "1", 0);
        CFPreferencesSetAppValue(CFSTR("CAEnableFramebufferCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
        CFPreferencesSetAppValue(CFSTR("CAEnableIOSurfaceCompression"), kCFBooleanFalse, kCFPreferencesCurrentApplication);

        // The runtime refuses to open without a writable shader-cache directory, and which directories the render server's sandbox lets it write is
        // not known from here: the user cache and temp directories, /tmp and /var/tmp, the first that opens a device.
        char candidates[5][512] = {};
        if (!confstr(_CS_DARWIN_USER_CACHE_DIR, candidates[0], sizeof candidates[0] - 16))
            candidates[0][0] = 0;
        if (!confstr(_CS_DARWIN_USER_TEMP_DIR, candidates[1], sizeof candidates[1] - 16))
            candidates[1][0] = 0;
        strlcpy(candidates[2], "/tmp/", sizeof candidates[2]);
        strlcpy(candidates[3], "/private/var/tmp/", sizeof candidates[3]);
        // The mobile user's temp directory (/var/mobile/tmp) is emptied by launchd's boot task dirs_cleaner at every boot, so the pipelines the
        // runtime compiled were compiled again each boot (the "cache hit 0 miss N" of every boot) although Data persists. The user's
        // Library/Caches is not emptied: tried first (upstream's finding, AdrenoMetal NativeAdrenoMetalPlugin.mm).
        if (const passwd *user = getpwuid(getuid()); user && user->pw_dir && user->pw_dir[0]) {
            snprintf(candidates[4], sizeof candidates[4] - 16, "%s/Library/Caches/com.apple.%s", user->pw_dir, getprogname());
            mkdir(candidates[4], 0700);
            strlcat(candidates[4], "/", sizeof candidates[4]);
        }
        {
            // What the load of the driver library costs on its own (dyld binds its imports by walking export tries; gpu43 caught the poster's main
            // thread in that): the runtime's own dlopen of the same file below then finds it loaded.
            const uint64_t before = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
            void *loaded = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
            gIcdHandle = loaded;
            mlog("metal: loading the driver library took %llu ms (%s)", (unsigned long long)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000 - before),
                 loaded ? "ok" : "failed");
        }
        {
            // mali-ycbcr-memo=N (default 4096, 0 = off): the runtime converts a two-plane YCbCr texture (the lock screen's 3416x3416 wallpaper)
            // at every encoder that samples it, 74 ms of GPU each here; with this it hashes N sampled 64-byte chunks of the planes' bytes and the
            // count of the writes encoded to them, and skips the conversion while both are as they were (m2v-patches/0100, ycbcr_memo.rs).
            char memo[32];
            if (!bootArgValue("mali-ycbcr-memo", memo, sizeof memo))
                strlcpy(memo, "4096", sizeof memo);
            setenv("M2V_YCBCR_MEMO", memo, 0);
        }
        {
            // mali-ycbcr-filter=device: a linear sample of a converted two-plane YCbCr texture (the wallpaper) is the device's own filter of the
            // converted image's texels, as every other texture's is, and not Metal's filter emulated exactly in the fragment function (about 570
            // instructions a sample: on the board at 750 MHz the wallpaper's full-screen pass took 22.5 ms with the emulation, 7.9 ms without).
            // Unset (or =exact): the emulation (m2v-patches/0101, bind/plan/slots.rs).
            char filter[16];
            if (bootArgValue("mali-ycbcr-filter", filter, sizeof filter))
                setenv("M2V_YCBCR_FILTER", filter, 0);
        }
        if (server)
            startDumpFromBootArgs();
        // mali-cache=NAME: the shader cache is the directory "m2v-NAME" (keep the name short) instead of "m2v": an empty one, so every pipeline is compiled
        // again. The shader compiler's statistics (mali-bifrost-debug=shaderdb) are printed for a pipeline that is compiled, not for one the cache has.
        char leaf[24] = "m2v";
        if (char name[16]; bootArgValue("mali-cache", name, sizeof name)) {
            strlcat(leaf, "-", sizeof leaf);
            strlcat(leaf, name, sizeof leaf);
        }
        static const unsigned order[] = {4, 0, 1, 2, 3};
        for (unsigned i : order) {
            if (device)
                break;
            if (!candidates[i][0])
                continue;
            strlcat(candidates[i], leaf, sizeof candidates[i]);
            mkdir(candidates[i], 0700);
            if (access(candidates[i], W_OK) != 0) {
                mlog("metal: cache directory %d (%s) is not writable", i, candidates[i]);
                continue;
            }
            mlog("metal: +%llu ms, opening %s with cache %s", (unsigned long long)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000 - began), icd, candidates[i]);
            native_bridge_metal_set_console([](const char *text) { mlog("fe: %s", text); });
            @try {
                device = native_bridge_metal_open(icd, candidates[i], registryID, missing);
            } @catch (NSException *exception) {
                mlog("metal: native_bridge_metal_open threw %s", exception.reason.UTF8String);
                device = nil;
            }
        }
        if (!device) {
            // No directory the process may write (a sandboxed extension): the runtime may run without its shader cache then (translator patch 0017).
            setenv("METAL2VULKAN_SHADER_CACHE_OPTIONAL", "1", 1);
            const char *fallback = candidates[4][0] ? candidates[4] : candidates[0][0] ? candidates[0] : candidates[1][0] ? candidates[1] : "/var/mobile/tmp/m2v";
            mlog("metal: no writable cache directory: opening without a shader cache (%s)", fallback);
            @try {
                device = native_bridge_metal_open(icd, fallback, registryID, missing);
            } @catch (NSException *exception) {
                mlog("metal: native_bridge_metal_open threw %s", exception.reason.UTF8String);
                device = nil;
            }
        }
        if (device) {
            // The process's first problem (a refusal, a missing call), once, from the thread that hit it: a client that crashes right after
            // its first refusal leaves no other trace (the once-a-second readers never run before it).
            native_bridge_metal_problem_hook = [](const char *problem) { mlog("first problem: %s", problem ? problem : "?"); };
            char problem[200] = {};
            if (native_bridge_metal_first_problem(problem, sizeof problem) && problem[0])
                mlog("first problem so far: %s", problem);
        }
        if (device && !server) {
            pthread_t thread;
            if (pthread_create(&thread, nullptr, memoryThread, nullptr) == 0)
                pthread_detach(thread);
        }
        mlog("metal: %s (%llu ms)", device ? "device ready, handing it to the discovery" : "no device", (unsigned long long)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000 - began));
        if (device && server) {
            pthread_t thread;
            if (pthread_create(&thread, nullptr, statusThread, nullptr) == 0)
                pthread_detach(thread);
        }
        if (device && server) {
            pthread_t thread;
            if (bootArgHas("mali-profile=1")) {
                if (pthread_create(&thread, nullptr, cpuThread, nullptr) == 0)
                    pthread_detach(thread);
                if (pthread_create(&thread, nullptr, profileThread, nullptr) == 0)
                    pthread_detach(thread);
            }
            if (bootArgHas("mali-timeline=1")) {
                if (pthread_create(&thread, nullptr, timelineThread, nullptr) == 0)
                    pthread_detach(thread);
            }
        }
        if (device && server) {
            pthread_t thread;
            if (pthread_create(&thread, nullptr, presenterThread, (void *)CFBridgingRetain(device)) == 0)
                pthread_detach(thread);
        }
        return device;
    }
}

void selftestMark(uint32_t marker) { mlog("selftest: step %#x", marker); }

}  // namespace

// For the board's test program (usertest/): the bridge's own self-test (compute, render pass, no-copy import of host pages, optionally the
// translation of QuartzCore's library) over PanVK, reported through mlog. dlsym'd from the bundle's executable; returns 0 or the failed step's marker.
extern "C" __attribute__((visibility("default"))) int nmm_selftest(int quartzcore) {
    char dir[512], icd[640];
    if (!bundleDirectory(dir, sizeof dir)) {
        mlog("selftest: no bundle directory");
        return 1;
    }
    snprintf(icd, sizeof icd, "%s/libvulkan_panfrost.dylib", dir);
    const char *cache = "/private/var/tmp/m2v-selftest";
    mkdir("/private/var/tmp", 0755);
    mkdir(cache, 0700);
    std::vector<uint8_t> library;
    if (quartzcore) {
        if (FILE *f = fopen("/System/Library/Frameworks/QuartzCore.framework/default.metallib", "rb")) {
            uint8_t chunk[65536];
            size_t n;
            while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
                library.insert(library.end(), chunk, chunk + n);
            fclose(f);
        } else {
            mlog("selftest: QuartzCore's metallib is not readable (errno %d)", errno);
        }
    }
    mlog("selftest: %s, cache %s, QuartzCore library %zu bytes", icd, cache, library.size());
    NmbSelftest result = {};
    const uint32_t code = nmb_selftest(icd, cache, library.empty() ? nullptr : library.data(), library.size(), selftestMark, &result);
    mlog("selftest: result %#x stage %#x compute_bad %u render_bad %u import_bad %u qc functions %u kernels %u/%u pairs %u/%u", code, result.stage,
         result.compute_bad, result.render_bad, result.import_bad, result.qc_functions, result.qc_kernels_ok, result.qc_kernels_failed, result.qc_pairs_ok,
         result.qc_pairs_failed);
    mlog("selftest: %s", result.message);
    return (int)code;
}

@implementation NativeMaliMetalDevice
- (id)initWithAcceleratorPort:(unsigned)port {
    self = [super init];
    if (!self || !port)
        return nil;
    if (strcmp(getprogname(), "backboardd") != 0 && !clientProcess())
        return nil;
    return openDevice(port);
}
@end
