/*
 * Drop-in liblog.so for hosting the original Android libil2cpp.so on HarmonyOS.
 *
 * Why this works: the original libil2cpp.so lists liblog.so in DT_NEEDED, and HarmonyOS ships no
 * liblog.so, so the dynamic loader resolves that name to this library. Putting the bionic gaps here
 * as well means one file closes both the logging and the four unresolved symbols measured by
 * tools/ohos_resolvability.py:
 *
 *   __FD_SET_chk            bionic's hardened FD_SET wrapper
 *   android_set_abort_message   bionic abort-message hook
 *   strtoll_l / strtoull_l  POSIX locale-aware integer parsing (no locale on OHOS)
 *
 * Everything is written against OHOS's hilog so the output still lands in the device log.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <hilog/log.h>

/* ---- Android logging surface ------------------------------------------------------------- */

#define ANDROID_LOG_UNKNOWN 0
#define ANDROID_LOG_DEFAULT 1
#define ANDROID_LOG_VERBOSE 2
#define ANDROID_LOG_DEBUG   3
#define ANDROID_LOG_INFO    4
#define ANDROID_LOG_WARN    5
#define ANDROID_LOG_ERROR   6
#define ANDROID_LOG_FATAL   7
#define ANDROID_LOG_SILENT  8

static LogLevel ToHilog(int prio)
{
    switch (prio) {
        case ANDROID_LOG_VERBOSE:
        case ANDROID_LOG_DEBUG:
            return LOG_DEBUG;
        case ANDROID_LOG_INFO:
            return LOG_INFO;
        case ANDROID_LOG_WARN:
            return LOG_WARN;
        case ANDROID_LOG_ERROR:
        case ANDROID_LOG_FATAL:
            return LOG_ERROR;
        default:
            return LOG_INFO;
    }
}

/* Unity tags most of its output with "__android_log"; keep the tag as the hilog domain tag. */
static const char *SafeTag(const char *tag) { return tag ? tag : "Android"; }

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char buffer[1024];
    int written = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (written < 0) return written;

    OH_LOG_Print(LOG_APP, ToHilog(prio), 0x0000, SafeTag(tag), "%{public}s", buffer);
    return written + 1;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list args)
{
    char buffer[1024];
    int written = vsnprintf(buffer, sizeof(buffer), fmt, args);
    if (written < 0) return written;

    OH_LOG_Print(LOG_APP, ToHilog(prio), 0x0000, SafeTag(tag), "%{public}s", buffer);
    return written + 1;
}

int __android_log_write(int prio, const char *tag, const char *text)
{
    if (!text) return 0;
    OH_LOG_Print(LOG_APP, ToHilog(prio), 0x0000, SafeTag(tag), "%{public}s", text);
    return (int)strlen(text) + 1;
}

int __android_log_vprint_(int prio, const char *tag, const char *fmt, va_list args)
{
    return __android_log_vprint(prio, tag, fmt, args);
}

/*
 * Probe: this library is only pulled in as a DT_NEEDED of the original libil2cpp.so, so the tag
 * appearing in the device log proves that the Android payload mapped and its relocations resolved.
 * If the process dies without this line, the failure is at map/relocation time, not later.
 */
__attribute__((constructor)) static void ShimLoadedProbe(void)
{
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "I2CppShim",
                 "%{public}s", "SHIM_LOADED original libil2cpp mapped and relocated");
}

__attribute__((destructor)) static void ShimUnloadProbe(void)
{
    OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "I2CppShim", "%{public}s", "SHIM_UNLOAD");
}

/* ---- bionic gaps measured for the original libil2cpp.so ---------------------------------- */

/* bionic exposes a checked FD_SET; the check is a bounds assertion against FD_SETSIZE. */
void __FD_SET_chk(int fd, fd_set *set)
{
    if (!set || fd < 0 || fd >= FD_SETSIZE) return;
    FD_SET(fd, set);
}

/* Called by bionic when aborting; keep the message so a crash report could read it. */
static char g_abort_message[512];

void android_set_abort_message(const char *message)
{
    if (!message) {
        g_abort_message[0] = '\0';
        return;
    }
    strncpy(g_abort_message, message, sizeof(g_abort_message) - 1);
    g_abort_message[sizeof(g_abort_message) - 1] = '\0';
    OH_LOG_Print(LOG_APP, LOG_FATAL, 0x0000, "AndroidAbort", "%{public}s", g_abort_message);
}

/* OHOS has no xlocale; the *_l variants behave as the plain POSIX functions. */
long long strtoll_l(const char *nptr, char **endptr, int base, void *locale)
{
    (void)locale;
    return strtoll(nptr, endptr, base);
}

unsigned long long strtoull_l(const char *nptr, char **endptr, int base, void *locale)
{
    (void)locale;
    return strtoull(nptr, endptr, base);
}

/* ---- bionic C runtime shims -------------------------------------------------------------- */

/* bionic exposes __errno() returning a TLS int*; musl names it __errno_location(). */
int *__errno(void);
int *__errno(void) { return __errno_location(); }

/*
 * bionic's stdio keeps a three-element FILE table and defines stdout/stderr/stdin as its
 * addresses. Android-built objects therefore reference the symbol __sF directly. OHOS's musl has
 * no such table, so a block of the same element size is provided. IL2CPP logs through logcat, not
 * stdio, so these entries are only expected to exist, not to be streamed to.
 */
typedef struct { unsigned char bytes[152]; } BionicFile;
BionicFile __sF[3];

/* Fortified helpers bionic exports that musl does not. */
int __open_2(const char *path, int flags);
int __open_2(const char *path, int flags) { return open(path, flags); }

char *__strncpy_chk2(char *dest, const char *src, size_t len, size_t destlen, size_t srclen);
char *__strncpy_chk2(char *dest, const char *src, size_t len, size_t destlen, size_t srclen)
{
    if (len == 0) return dest;
    if (len > destlen) {
        android_set_abort_message("__strncpy_chk2: dest overflow");
        return dest;
    }
    if (len > srclen) {
        memset(dest, 0, len);
        return dest;
    }
    return strncpy(dest, src, len);
}

/* Android system-property lookup; OHOS has no equivalent property service under this name. */
int __system_property_get(const char *name, char *value);
int __system_property_get(const char *name, char *value)
{
    (void)name;
    if (value) value[0] = '\0';
    return 0;
}

/*
 * glibc's per-thread atexit. musl has no public __cxa_thread_atexit_impl, so this accepts and
 * ignores the registration; handlers registered this way simply never run at thread exit, which is
 * how C++ thread_local destructors are cleaned up. Acceptable for a first-boot prober.
 */
int __cxa_thread_atexit_impl(void (*func)(void *), void *obj, void *dso_symbol)
{
    (void)func; (void)obj; (void)dso_symbol;
    return 0;
}

/* ---- bionic ctype table ------------------------------------------------------------------ */

/*
 * bionic's ctype.h expands isupper()/isspace()/... to reads of _ctype_[c+1], so an object compiled
 * against bionic headers needs this table exported by the runtime. The bit values below are
 * bionic's own, not glibc's.
 */
#define CT_UPPER 0x01
#define CT_LOWER 0x02
#define CT_DIGIT 0x04
#define CT_SPACE 0x08
#define CT_PUNCT 0x10
#define CT_CONTROL 0x20
#define CT_XDIGIT 0x40
#define CT_BLANK 0x80

char _ctype_[257];

__attribute__((constructor)) static void BuildCtypeTable(void)
{
    _ctype_[0] = 0; /* EOF slot */
    for (int c = 0; c < 256; c++) {
        unsigned char flags = 0;
        if (c >= 'A' && c <= 'Z') flags |= CT_UPPER;
        if (c >= 'a' && c <= 'z') flags |= CT_LOWER;
        if (c >= '0' && c <= '9') flags |= CT_DIGIT;
        if (c == ' ' || (c >= '\t' && c <= '\r')) flags |= CT_SPACE;
        if ((flags & (CT_UPPER | CT_LOWER | CT_DIGIT)) == 0 && c > 0x20 && c < 0x7f) flags |= CT_PUNCT;
        if (c < 0x20 || c == 0x7f) flags |= CT_CONTROL;
        if ((flags & CT_DIGIT) ||
            (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) flags |= CT_XDIGIT;
        if (c == ' ' || c == '\t') flags |= CT_BLANK;
        _ctype_[c + 1] = (char)flags;
    }
}

/* ---- remaining bionic helpers libunity imports ------------------------------------------ */

int __FD_ISSET_chk(int fd, const fd_set *set);
int __FD_ISSET_chk(int fd, const fd_set *set)
{
    if (!set || fd < 0 || fd >= FD_SETSIZE) return 0;
    return FD_ISSET(fd, set);
}

/*
 * _ZTH15gDeferredAction is the thread_local initialization routine for a C++ variable named
 * gDeferredAction. The defining translation unit is not present in the shipped library, so it is
 * provided as an empty initializer: the variable stays zero-initialised, which is what a deferred
 * action slot expects before anything is posted to it.
 */
void ReaTlsInit_gDeferredAction(void) __asm__("_ZTH15gDeferredAction");
void ReaTlsInit_gDeferredAction(void) { }
