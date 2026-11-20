/*
 * Zip-backed asset access for the Android-on-HarmonyOS translation layer.
 *
 * Unity's Android player never links an asset API (measured: zero AAsset imports). It reads game
 * data through Java instead:
 *
 *     Context.getAssets() -> AssetManager.openFd(path) -> AssetFileDescriptor(
 *         FileDescriptor descriptor, long startOffset, long length)
 *     -> the engine pread()s descriptor at startOffset + position
 *
 * The HAP no longer carries the decompressed boot tree, so the source of truth is the Android APK
 * the user imported once. Resolution is split in two:
 *
 *   - Stored entries (every song, illustration and audio asset, plus a few boot files) are served
 *     straight out of the APK: no copy, exact (fd, startOffset, length) semantics.
 *   - Deflated entries are materialised into the app cache BEFORE the engine starts, by
 *     ReaPrepareImportedAssets(). A manifest records what the cache must contain, and every start
 *     verifies it (presence + length), so a cleared or damaged cache is rebuilt up front instead of
 *     surfacing as a stall in the middle of the game.
 *
 * Each materialised file is checked against the archive's own CRC and length while it is written,
 * and its cache name carries that CRC and length, so a stale file from another APK is never served.
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <hilog/log.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "TransAsset"

#define LOGI(fmt, ...) OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, fmt, ##__VA_ARGS__)

namespace {

/* The imported original APK, verified and copied by ArkTS before the engine starts. */
const char *kApkPath = "/data/storage/el2/base/files/rea-original-phigros.apk";
/* Default cache root; ArkTS overrides it through ReaConfigureAssetCache. */
const char *kDefaultCacheRoot = "/data/storage/el2/base/cache/rea-apk-assets";
/* Everything the engine asks for lives under this prefix inside the APK. */
const char *kAssetPrefix = "assets/";
/* Written last, checked first: what the cache directory is supposed to contain. */
const char *kManifestName = "manifest.txt";
const char *kManifestBanner = "rea-asset-cache";
const int kManifestVersion = 1;

struct Entry {
    std::string name;
    uint16_t method = 0;
    uint64_t compressedSize = 0;
    uint64_t uncompressedSize = 0;
    uint64_t crc = 0;
    uint64_t localHeaderOffset = 0;
};

struct Archive {
    int fd = -1;
    std::map<std::string, Entry> entries;
    bool ready = false;
};

Archive g_apk;
std::mutex g_mutex;
std::string g_cacheRoot = kDefaultCacheRoot;

/* Preparation progress. Read by ArkTS while the worker runs, so it never takes g_mutex. */
std::atomic<uint64_t> g_prepareDone{0};
std::atomic<uint64_t> g_prepareTotal{0};
std::atomic<uint64_t> g_prepareBytes{0};
std::atomic<uint64_t> g_prepareBytesTotal{0};
std::atomic<int> g_prepareActive{0};

uint16_t U16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t U32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

bool Read(int fd, uint64_t offset, void *buffer, size_t size)
{
    size_t done = 0;
    while (done < size) {
        ssize_t n = pread(fd, (unsigned char *)buffer + done, size - done, (off_t)(offset + done));
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

bool Write(int fd, const void *buffer, size_t size)
{
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, (const unsigned char *)buffer + done, size - done);
        if (n <= 0) return false;
        done += (size_t)n;
    }
    return true;
}

bool MakeDirs(const std::string &path)
{
    for (size_t i = 1; i <= path.size(); i++) {
        if (i != path.size() && path[i] != '/') continue;
        if (mkdir(path.substr(0, i).c_str(), 0770) != 0 && errno != EEXIST) return false;
    }
    return true;
}

/*
 * Rejects anything that could escape the cache directory. Asset names come from the engine, but the
 * APK is user-supplied, so neither side is trusted as a path generator.
 */
bool Safe(const std::string &name)
{
    if (name.empty() || name[0] == '/') return false;
    for (size_t i = 0; i < name.size(); i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == '\\') return false;
        if (c == '.' && i + 1 < name.size() && name[i + 1] == '.') return false;
    }
    return true;
}

/* Reads the end-of-central-directory record and builds the entry table. */
bool Parse(Archive &archive)
{
    off_t fileSize = lseek(archive.fd, 0, SEEK_END);
    if (fileSize <= 0) return false;

    /* The EOCD record sits within the last 64 KiB + 22 bytes. */
    size_t tailSize = fileSize < 70000 ? (size_t)fileSize : 70000;
    std::vector<unsigned char> tail(tailSize);
    if (!Read(archive.fd, (uint64_t)(fileSize - (off_t)tailSize), tail.data(), tailSize)) return false;

    ssize_t eocd = -1;
    for (ssize_t i = (ssize_t)tailSize - 22; i >= 0; i--) {
        if (tail[i] == 0x50 && tail[i + 1] == 0x4b && tail[i + 2] == 0x05 && tail[i + 3] == 0x06) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) return false;

    uint16_t entryCount = U16(&tail[eocd + 10]);
    uint32_t cdSize = U32(&tail[eocd + 12]);
    uint32_t cdOffset = U32(&tail[eocd + 16]);
    if (cdSize == 0) return false;

    std::vector<unsigned char> cd(cdSize);
    if (!Read(archive.fd, cdOffset, cd.data(), cdSize)) return false;

    size_t pos = 0;
    for (uint16_t index = 0; index < entryCount; index++) {
        if (pos + 46 > cd.size()) break;
        if (!(cd[pos] == 0x50 && cd[pos + 1] == 0x4b && cd[pos + 2] == 0x01 && cd[pos + 3] == 0x02)) break;

        Entry entry;
        entry.method = U16(&cd[pos + 10]);
        entry.crc = U32(&cd[pos + 16]);
        entry.compressedSize = U32(&cd[pos + 20]);
        entry.uncompressedSize = U32(&cd[pos + 24]);
        uint16_t nameLen = U16(&cd[pos + 28]);
        uint16_t extraLen = U16(&cd[pos + 30]);
        uint16_t commentLen = U16(&cd[pos + 32]);
        entry.localHeaderOffset = U32(&cd[pos + 42]);

        if (pos + 46 + nameLen > cd.size()) break;
        entry.name.assign((const char *)&cd[pos + 46], nameLen);

        /* Only the asset tree is ever served; skipping the rest keeps the table small. */
        if (entry.name.rfind(kAssetPrefix, 0) == 0) archive.entries[entry.name] = entry;

        pos += 46 + nameLen + extraLen + commentLen;
    }
    return !archive.entries.empty();
}

bool EnsureArchiveLocked()
{
    if (g_apk.ready) return g_apk.fd >= 0;
    g_apk.ready = true;

    g_apk.fd = open(kApkPath, O_RDONLY | O_CLOEXEC);
    if (g_apk.fd < 0) {
        LOGI("APK_ARCHIVE readable=%{public}d path=%{public}s", 0, kApkPath);
        return false;
    }
    if (!Parse(g_apk)) {
        close(g_apk.fd);
        g_apk.fd = -1;
        LOGI("APK_ARCHIVE unparsable path=%{public}s", kApkPath);
        return false;
    }
    LOGI("APK_ARCHIVE readable=%{public}d path=%{public}s entries=%{public}zu", 1, kApkPath, g_apk.entries.size());
    return true;
}

bool EnsureArchive()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    return EnsureArchiveLocked();
}

/* Data start of an entry: local header, then name and extra field. */
bool DataOffset(const Entry &entry, uint64_t *outOffset)
{
    unsigned char header[30];
    if (!Read(g_apk.fd, entry.localHeaderOffset, header, sizeof(header))) return false;
    if (!(header[0] == 0x50 && header[1] == 0x4b && header[2] == 0x03 && header[3] == 0x04)) return false;
    *outOffset = entry.localHeaderOffset + 30 + U16(&header[26]) + U16(&header[28]);
    return true;
}

/*
 * Cache file name: a flattened, printable form of the asset path plus the archive's own CRC and
 * length. Encoding the expected content into the name means a different APK never serves a stale
 * file, without a separate index to keep in sync.
 */
std::string CacheName(const std::string &name, const Entry &entry)
{
    std::string safe = name;
    for (size_t i = 0; i < safe.size(); i++) {
        unsigned char c = (unsigned char)safe[i];
        safe[i] = (c == '/' || c < 0x20 || c >= 0x7f) ? '_' : (char)c;
    }
    char suffix[48];
    snprintf(suffix, sizeof(suffix), ".%08lx.%llu", (unsigned long)entry.crc, (unsigned long long)entry.uncompressedSize);
    return g_cacheRoot + "/" + safe + suffix;
}

bool Cached(const std::string &path, const Entry &entry)
{
    struct stat info;
    if (stat(path.c_str(), &info) != 0) return false;
    return S_ISREG(info.st_mode) && (uint64_t)info.st_size == entry.uncompressedSize;
}

/*
 * Inflates one deflated entry into the cache, verifying the archive's CRC and length. Writes
 * through a temporary name so a partial file is never mistaken for a complete one.
 */
int Inflate(const Entry &entry, uint64_t dataOffset, int64_t *outSize)
{
    std::string finalPath = CacheName(entry.name, entry);
    std::string temporary = finalPath + ".part";
    int out = open(temporary.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out < 0) return -1;

    std::vector<unsigned char> input(64 * 1024);
    std::vector<unsigned char> output(64 * 1024);
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        close(out);
        unlink(temporary.c_str());
        return -1;
    }

    uint64_t remaining = entry.compressedSize;
    uint64_t readOffset = dataOffset;
    uint64_t written = 0;
    uLong crc = crc32(0L, Z_NULL, 0);
    int status = Z_OK;
    bool okay = true;
    while (remaining > 0 && status != Z_STREAM_END) {
        size_t chunk = remaining < input.size() ? (size_t)remaining : input.size();
        if (!Read(g_apk.fd, readOffset, input.data(), chunk)) { okay = false; break; }
        readOffset += chunk;
        remaining -= chunk;

        stream.next_in = input.data();
        stream.avail_in = (uInt)chunk;
        while (stream.avail_in > 0) {
            stream.next_out = output.data();
            stream.avail_out = (uInt)output.size();
            status = inflate(&stream, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_STREAM_END) { okay = false; break; }
            size_t produced = output.size() - stream.avail_out;
            if (produced) {
                crc = crc32(crc, output.data(), (uInt)produced);
                written += produced;
                if (!Write(out, output.data(), produced)) { okay = false; break; }
            }
            if (status == Z_STREAM_END) break;
        }
        if (!okay) break;
    }
    inflateEnd(&stream);

    if (okay && (status != Z_STREAM_END || written != entry.uncompressedSize || (uint64_t)crc != entry.crc)) {
        okay = false;
    }
    if (!okay) {
        LOGI("APK_ASSET_CACHE rejected CRC/size/IO name=%{public}s", entry.name.c_str());
        close(out);
        unlink(temporary.c_str());
        return -1;
    }
    if (fsync(out) != 0) {
        close(out);
        unlink(temporary.c_str());
        return -1;
    }
    close(out);
    if (rename(temporary.c_str(), finalPath.c_str()) != 0) {
        unlink(temporary.c_str());
        return -1;
    }

    int fd = open(finalPath.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    *outSize = (int64_t)written;
    return fd;
}

/* ---- cache manifest: what the cache directory is supposed to hold ------------------------- */

std::string ManifestPath() { return g_cacheRoot + "/" + kManifestName; }

/*
 * Identity of the materialised set: a CRC over the sorted "name size crc" lines of every deflated
 * entry. Unlike a digest of the central directory this is stable across repackaging, because it
 * describes content rather than how the APK happened to be compressed.
 */
uint32_t ContentDigest()
{
    std::vector<std::string> lines;
    lines.reserve(g_apk.entries.size());
    char buffer[64];
    for (std::map<std::string, Entry>::const_iterator it = g_apk.entries.begin(); it != g_apk.entries.end(); ++it) {
        if (it->second.method == 0) continue;
        snprintf(buffer, sizeof(buffer), " %llu %08lx", (unsigned long long)it->second.uncompressedSize,
                 (unsigned long)it->second.crc);
        lines.push_back(it->first + buffer);
    }
    std::sort(lines.begin(), lines.end());
    uLong crc = crc32(0L, Z_NULL, 0);
    for (size_t i = 0; i < lines.size(); i++) {
        crc = crc32(crc, (const Bytef *)lines[i].data(), (uInt)lines[i].size());
        crc = crc32(crc, (const Bytef *)"\n", 1);
    }
    return (uint32_t)crc;
}

uint64_t DeflatedBytes()
{
    uint64_t total = 0;
    for (std::map<std::string, Entry>::const_iterator it = g_apk.entries.begin(); it != g_apk.entries.end(); ++it) {
        if (it->second.method != 0) total += it->second.uncompressedSize;
    }
    return total;
}

size_t DeflatedCount()
{
    size_t count = 0;
    for (std::map<std::string, Entry>::const_iterator it = g_apk.entries.begin(); it != g_apk.entries.end(); ++it) {
        if (it->second.method != 0) count++;
    }
    return count;
}

/* Empties the cache directory. It belongs to this module, so nothing else is at risk. */
bool WipeLocked()
{
    DIR *directory = opendir(g_cacheRoot.c_str());
    if (directory == nullptr) return false;
    struct dirent *item = nullptr;
    int removed = 0;
    while ((item = readdir(directory)) != nullptr) {
        if (item->d_name[0] == '.' && (item->d_name[1] == 0 || (item->d_name[1] == '.' && item->d_name[2] == 0))) continue;
        std::string path = g_cacheRoot + "/" + item->d_name;
        if (unlink(path.c_str()) == 0) removed++;
    }
    closedir(directory);
    LOGI("APK_ASSET_CACHE wiped files=%{public}d", removed);
    return true;
}

bool WriteManifestLocked(uint32_t digest, size_t count)
{
    const std::string path = ManifestPath();
    const std::string temporary = path + ".part";
    FILE *file = fopen(temporary.c_str(), "wb");
    if (file == nullptr) return false;

    bool okay = fprintf(file, "%s %d %08lx %llu\n", kManifestBanner, kManifestVersion, (unsigned long)digest,
                        (unsigned long long)count) > 0;
    for (std::map<std::string, Entry>::const_iterator it = g_apk.entries.begin(); okay && it != g_apk.entries.end(); ++it) {
        if (it->second.method == 0) continue;
        okay = fprintf(file, "%llu %08lx %s\n", (unsigned long long)it->second.uncompressedSize,
                       (unsigned long)it->second.crc, it->first.c_str()) > 0;
    }
    if (okay && fflush(file) != 0) okay = false;
    if (okay && fsync(fileno(file)) != 0) okay = false;
    fclose(file);
    if (!okay) {
        unlink(temporary.c_str());
        return false;
    }
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        unlink(temporary.c_str());
        return false;
    }
    return true;
}

/*
 * Cheap integrity check for every start: the manifest must describe this archive, and every file it
 * names must still be present with the recorded length. Content is not re-read here - it was
 * verified against the archive CRC when it was written, and the cache name pins that CRC.
 */
bool VerifyManifestLocked(const char **outReason)
{
    *outReason = "manifest missing";
    FILE *file = fopen(ManifestPath().c_str(), "rb");
    if (file == nullptr) return false;

    char line[4096];
    char banner[32] = {0};
    int version = 0;
    unsigned long digest = 0;
    unsigned long long count = 0;
    if (fgets(line, sizeof(line), file) == nullptr ||
        sscanf(line, "%31s %d %8lx %llu", banner, &version, &digest, &count) != 4) {
        fclose(file);
        *outReason = "manifest header unreadable";
        return false;
    }
    if (strcmp(banner, kManifestBanner) != 0 || version != kManifestVersion) {
        fclose(file);
        *outReason = "manifest version mismatch";
        return false;
    }
    if ((uint32_t)digest != ContentDigest()) {
        fclose(file);
        *outReason = "manifest belongs to a different archive";
        return false;
    }
    if (count != (unsigned long long)DeflatedCount()) {
        fclose(file);
        *outReason = "manifest entry count mismatch";
        return false;
    }

    unsigned long long seen = 0;
    while (fgets(line, sizeof(line), file) != nullptr) {
        size_t length = strlen(line);
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) line[--length] = 0;
        if (length == 0) continue;
        char *first = strchr(line, ' ');
        char *second = first ? strchr(first + 1, ' ') : nullptr;
        if (first == nullptr || second == nullptr) continue;
        *first = 0;
        *second = 0;

        Entry entry;
        entry.uncompressedSize = strtoull(line, nullptr, 10);
        entry.crc = strtoul(first + 1, nullptr, 16);
        entry.name = second + 1;
        if (!Safe(entry.name)) {
            *outReason = "manifest path rejected";
            fclose(file);
            return false;
        }
        if (!Cached(CacheName(entry.name, entry), entry)) {
            *outReason = "cached file missing or truncated";
            fclose(file);
            return false;
        }
        seen++;
    }
    fclose(file);
    if (seen != count) {
        *outReason = "manifest is incomplete";
        return false;
    }
    return true;
}

/* Extracts every deflated entry, then records what was written. Runs before the engine starts. */
bool RebuildLocked()
{
    const size_t total = DeflatedCount();
    const uint64_t bytesTotal = DeflatedBytes();
    g_prepareTotal.store(total, std::memory_order_relaxed);
    g_prepareBytesTotal.store(bytesTotal, std::memory_order_relaxed);
    g_prepareDone.store(0, std::memory_order_relaxed);
    g_prepareBytes.store(0, std::memory_order_relaxed);
    LOGI("APK_ASSET_CACHE rebuild begin files=%{public}zu bytes=%{public}llu", total, (unsigned long long)bytesTotal);

    if (!WipeLocked()) return false;

    size_t index = 0;
    for (std::map<std::string, Entry>::const_iterator it = g_apk.entries.begin(); it != g_apk.entries.end(); ++it) {
        if (it->second.method == 0) continue;
        uint64_t dataOffset = 0;
        int64_t size = 0;
        if (!DataOffset(it->second, &dataOffset)) {
            LOGI("APK_ASSET_CACHE rejected offset name=%{public}s", it->first.c_str());
            return false;
        }
        int fd = Inflate(it->second, dataOffset, &size);
        if (fd < 0) return false;
        close(fd);
        index++;
        g_prepareDone.store(index, std::memory_order_relaxed);
        g_prepareBytes.fetch_add(it->second.uncompressedSize, std::memory_order_relaxed);
        if (index == total || (index % 64) == 0) {
            LOGI("APK_ASSET_CACHE prepared %{public}zu/%{public}zu", index, total);
        }
    }

    const uint32_t digest = ContentDigest();
    if (!WriteManifestLocked(digest, total)) {
        LOGI("APK_ASSET_CACHE manifest write failed");
        return false;
    }
    LOGI("APK_ASSET_CACHE ready files=%{public}zu bytes=%{public}llu digest=%{public}08lx", total, (unsigned long long)bytesTotal,
         (unsigned long)digest);
    return true;
}

} // namespace

/* Called by ArkTS once the sandbox cache directory is known; derives the asset cache under it. */
extern "C" bool ReaConfigureAssetCache(const char *directory)
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (directory == nullptr || directory[0] != '/') {
        LOGI("APK_ASSET_CACHE rejected dir");
        return false;
    }
    std::string root = std::string(directory) + "/rea-apk-assets";
    if (!MakeDirs(root)) {
        LOGI("APK_ASSET_CACHE unusable dir=%{public}s", root.c_str());
        return false;
    }
    g_cacheRoot = root;
    LOGI("APK_ASSET_CACHE root=%{public}s", g_cacheRoot.c_str());
    return true;
}

/* Progress for the UI; lock free so polling never waits behind an extraction. */
extern "C" void ReaAssetPreparationStatus(uint64_t *done, uint64_t *total, uint64_t *bytes,
                                          uint64_t *bytesTotal, int *active)
{
    if (done) *done = g_prepareDone.load(std::memory_order_relaxed);
    if (total) *total = g_prepareTotal.load(std::memory_order_relaxed);
    if (bytes) *bytes = g_prepareBytes.load(std::memory_order_relaxed);
    if (bytesTotal) *bytesTotal = g_prepareBytesTotal.load(std::memory_order_relaxed);
    if (active) *active = g_prepareActive.load(std::memory_order_relaxed);
}

/*
 * Makes the cache complete before the engine is allowed to start: verifies the manifest written by a
 * previous run and rebuilds everything when it is missing, stale or incomplete. Returns false only
 * when the archive itself cannot be read or an entry fails its CRC, which the caller reports to the
 * user instead of starting an engine that cannot load anything.
 */
extern "C" bool ReaPrepareImportedAssets()
{
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!EnsureArchiveLocked()) {
        LOGI("APK_ASSET_SOURCE unavailable");
        return false;
    }

    const char *reason = nullptr;
    if (VerifyManifestLocked(&reason)) {
        LOGI("APK_ASSET_SOURCE verified files=%{public}zu bytes=%{public}llu", DeflatedCount(), (unsigned long long)DeflatedBytes());
        g_prepareTotal.store(DeflatedCount(), std::memory_order_relaxed);
        g_prepareDone.store(DeflatedCount(), std::memory_order_relaxed);
        g_prepareBytesTotal.store(DeflatedBytes(), std::memory_order_relaxed);
        g_prepareBytes.store(DeflatedBytes(), std::memory_order_relaxed);
        return true;
    }

    LOGI("APK_ASSET_CACHE rebuild needed reason=%{public}s", reason ? reason : "unknown");
    g_prepareActive.store(1, std::memory_order_relaxed);
    bool okay = RebuildLocked();
    g_prepareActive.store(0, std::memory_order_relaxed);
    if (!okay) LOGI("APK_ASSET_SOURCE preparation failed");
    return okay;
}

/*
 * Resolves an asset path (as Unity asks for it, relative to assets/) into an open descriptor plus
 * the byte range that holds the file. Returns false when the entry is absent.
 */
extern "C" bool ReaAssetOpen(const char *assetPath, int *outFd, int64_t *outOffset, int64_t *outLength)
{
    if (!EnsureArchive()) return false;

    std::string path = assetPath ? assetPath : "";
    while (!path.empty() && path[0] == '/') path.erase(path.begin());
    if (path.rfind(kAssetPrefix, 0) == 0) path = path.substr(strlen(kAssetPrefix));
    if (!Safe(path)) return false;

    const std::string key = std::string(kAssetPrefix) + path;
    std::map<std::string, Entry>::const_iterator it = g_apk.entries.find(key);
    if (it == g_apk.entries.end()) return false;

    const Entry &entry = it->second;
    uint64_t dataOffset = 0;
    if (!DataOffset(entry, &dataOffset)) return false;

    if (entry.method == 0) {
        /* Stored: hand back a range inside the APK itself, no copy. */
        *outFd = g_apk.fd;
        *outOffset = (int64_t)dataOffset;
        *outLength = (int64_t)entry.uncompressedSize;
        return true;
    }

    std::lock_guard<std::mutex> guard(g_mutex);
    const std::string cachePath = CacheName(entry.name, entry);
    if (Cached(cachePath, entry)) {
        int cached = open(cachePath.c_str(), O_RDONLY | O_CLOEXEC);
        if (cached >= 0) {
            *outFd = cached;
            *outOffset = 0;
            *outLength = (int64_t)entry.uncompressedSize;
            return true;
        }
    }

    /*
     * Not the normal path: preparation guarantees the file is there. Kept so an engine that is
     * already running survives the cache being cleared underneath it.
     */
    LOGI("APK_ASSET_CACHE miss after preparation name=%{public}s", entry.name.c_str());
    int64_t inflated = 0;
    int cacheFd = Inflate(entry, dataOffset, &inflated);
    if (cacheFd < 0) return false;
    *outFd = cacheFd;
    *outOffset = 0;
    *outLength = inflated;
    return true;
}
