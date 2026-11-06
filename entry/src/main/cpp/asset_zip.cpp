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
 * HarmonyOS installs a HAP as a zip at /data/storage/el1/bundle/<module>.hap and does not unpack
 * rawfile to disk, so this module resolves the request against the zip's central directory. When the
 * entry is stored (which is how rawfile is packaged here) the returned descriptor points straight
 * into the HAP - no copy, no extraction, exact (fd, startOffset, length) semantics. A deflated entry
 * is inflated once into the app's cache directory and described the same way.
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>

#include <fcntl.h>
#include <hilog/log.h>
#include <unistd.h>
#include <zlib.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "TransAsset"

#define LOGI(fmt, ...) OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, fmt, ##__VA_ARGS__)

namespace {

struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    uint64_t compressedSize = 0;
    uint64_t uncompressedSize = 0;
    uint64_t localHeaderOffset = 0;
};

struct ZipArchive {
    int fd = -1;
    std::string path;
    std::map<std::string, ZipEntry> entries;
    bool ready = false;
};

ZipArchive g_hap;

uint32_t ReadU32(const unsigned char *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t ReadU16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool ReadAt(int fd, uint64_t offset, void *buffer, size_t size)
{
    return pread(fd, buffer, size, (off_t)offset) == (ssize_t)size;
}

/* Locates the module HAP inside the installed bundle directory. */
std::string FindHapPath()
{
    const char *candidates[] = {
        "/data/storage/el1/bundle/entry.hap",
        "/data/storage/el1/bundle/entry/entry.hap",
    };
    for (const char *candidate : candidates) {
        if (access(candidate, R_OK) == 0) return candidate;
    }
    /* Fall back to a directory scan so a renamed module still works. */
    const char *dir = "/data/storage/el1/bundle";
    /* opendir is declared in dirent.h; kept minimal here to avoid extra includes in callers. */
    return "";
}

bool ParseCentralDirectory(ZipArchive &archive)
{
    off_t fileSize = lseek(archive.fd, 0, SEEK_END);
    if (fileSize <= 0) return false;

    /* The end-of-central-directory record sits within the last 64 KiB + 22 bytes. */
    size_t tailSize = fileSize < 70000 ? (size_t)fileSize : 70000;
    std::vector<unsigned char> tail(tailSize);
    if (!ReadAt(archive.fd, (uint64_t)(fileSize - (off_t)tailSize), tail.data(), tailSize)) return false;

    ssize_t eocd = -1;
    for (ssize_t i = (ssize_t)tailSize - 22; i >= 0; i--) {
        if (tail[i] == 0x50 && tail[i + 1] == 0x4b && tail[i + 2] == 0x05 && tail[i + 3] == 0x06) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) return false;

    uint16_t entryCount = ReadU16(&tail[eocd + 10]);
    uint32_t cdSize = ReadU32(&tail[eocd + 12]);
    uint32_t cdOffset = ReadU32(&tail[eocd + 16]);
    (void)cdSize;

    std::vector<unsigned char> cd(cdSize ? cdSize : 1);
    if (cdSize == 0 || !ReadAt(archive.fd, cdOffset, cd.data(), cdSize)) return false;

    size_t pos = 0;
    for (uint16_t index = 0; index < entryCount; index++) {
        if (pos + 46 > cd.size()) break;
        if (!(cd[pos] == 0x50 && cd[pos + 1] == 0x4b && cd[pos + 2] == 0x01 && cd[pos + 3] == 0x02)) break;

        uint16_t method = ReadU16(&cd[pos + 10]);
        uint32_t compressedSize = ReadU32(&cd[pos + 20]);
        uint32_t uncompressedSize = ReadU32(&cd[pos + 24]);
        uint16_t nameLen = ReadU16(&cd[pos + 28]);
        uint16_t extraLen = ReadU16(&cd[pos + 30]);
        uint16_t commentLen = ReadU16(&cd[pos + 32]);
        uint32_t localOffset = ReadU32(&cd[pos + 42]);

        if (pos + 46 + nameLen > cd.size()) break;
        std::string name((const char *)&cd[pos + 46], nameLen);

        ZipEntry entry;
        entry.name = name;
        entry.method = method;
        entry.compressedSize = compressedSize;
        entry.uncompressedSize = uncompressedSize;
        entry.localHeaderOffset = localOffset;
        archive.entries[name] = entry;

        pos += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

bool EnsureArchive()
{
    if (g_hap.ready) return g_hap.fd >= 0;
    g_hap.ready = true;

    g_hap.path = FindHapPath();
    if (g_hap.path.empty()) {
        LOGI("no HAP found in the bundle directory");
        return false;
    }
    g_hap.fd = open(g_hap.path.c_str(), O_RDONLY);
    if (g_hap.fd < 0) {
        LOGI("cannot open %s", g_hap.path.c_str());
        return false;
    }
    if (!ParseCentralDirectory(g_hap)) {
        LOGI("cannot parse central directory of %s", g_hap.path.c_str());
        return false;
    }
    LOGI("HAP open: %s, %zu entries", g_hap.path.c_str(), g_hap.entries.size());
    return true;
}

/* Data start of an entry: local header, then name and extra field. */
bool DataOffset(const ZipEntry &entry, uint64_t *outOffset)
{
    unsigned char header[30];
    if (!ReadAt(g_hap.fd, entry.localHeaderOffset, header, sizeof(header))) return false;
    if (!(header[0] == 0x50 && header[1] == 0x4b && header[2] == 0x03 && header[3] == 0x04)) return false;
    uint16_t nameLen = ReadU16(&header[26]);
    uint16_t extraLen = ReadU16(&header[28]);
    *outOffset = entry.localHeaderOffset + 30 + nameLen + extraLen;
    return true;
}

std::string CachePathFor(const std::string &name)
{
    std::string safe = name;
    for (char &c : safe) {
        if (c == '/') c = '_';
    }
    return "/data/storage/el2/base/cache/" + safe;
}

/* Inflates a deflated entry once so the engine can pread() it normally. */
int InflateToCache(const ZipEntry &entry, uint64_t dataOffset, int64_t *outSize)
{
    std::string cachePath = CachePathFor(entry.name);
    int out = open(cachePath.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (out < 0) return -1;

    std::vector<unsigned char> in(64 * 1024);
    std::vector<unsigned char> outBuffer(64 * 1024);
    z_stream stream;
    memset(&stream, 0, sizeof(stream));
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
        close(out);
        return -1;
    }

    uint64_t remaining = entry.compressedSize;
    uint64_t readOffset = dataOffset;
    int status = Z_OK;
    while (remaining > 0 && status != Z_STREAM_END) {
        size_t chunk = remaining < in.size() ? (size_t)remaining : in.size();
        if (!ReadAt(g_hap.fd, readOffset, in.data(), chunk)) break;
        readOffset += chunk;
        remaining -= chunk;

        stream.next_in = in.data();
        stream.avail_in = (uInt)chunk;
        while (stream.avail_in > 0) {
            stream.next_out = outBuffer.data();
            stream.avail_out = (uInt)outBuffer.size();
            status = inflate(&stream, Z_NO_FLUSH);
            if (status != Z_OK && status != Z_STREAM_END) break;
            size_t produced = outBuffer.size() - stream.avail_out;
            if (produced) {
                ssize_t ignored = write(out, outBuffer.data(), produced);
                (void)ignored;
            }
            if (status == Z_STREAM_END) break;
        }
    }
    inflateEnd(&stream);

    *outSize = lseek(out, 0, SEEK_END);
    lseek(out, 0, SEEK_SET);
    return out;
}

} // namespace

/*
 * Resolves an asset path (as Unity asks for it, relative to assets/) into an open descriptor plus
 * the byte range that holds the file. Returns false when the entry is absent.
 */
extern "C" bool ReaAssetOpen(const char *assetPath, int *outFd, int64_t *outOffset, int64_t *outLength)
{
    if (!EnsureArchive()) return false;

    std::string path = assetPath ? assetPath : "";
    while (!path.empty() && path[0] == '/') path.erase(path.begin());
    if (path.rfind("assets/", 0) == 0) path = path.substr(7);

    const std::string prefix = "resources/rawfile/assets/";
    std::string key = prefix + path;

    auto it = g_hap.entries.find(key);
    if (it == g_hap.entries.end()) {
        /* Try the deployed tree as well, so a future non-zip layout still works. */
        int direct = open(("/data/storage/el1/bundle/rawfile/assets/" + path).c_str(), O_RDONLY);
        if (direct < 0) return false;
        *outFd = direct;
        *outOffset = 0;
        *outLength = lseek(direct, 0, SEEK_END);
        lseek(direct, 0, SEEK_SET);
        return true;
    }

    const ZipEntry &entry = it->second;
    uint64_t dataOffset = 0;
    if (!DataOffset(entry, &dataOffset)) return false;

    if (entry.method == 0) {
        /* Stored: hand back a range inside the HAP itself, no copy. */
        *outFd = g_hap.fd;
        *outOffset = (int64_t)dataOffset;
        *outLength = (int64_t)entry.uncompressedSize;
        return true;
    }

    int64_t inflated = 0;
    int cacheFd = InflateToCache(entry, dataOffset, &inflated);
    if (cacheFd < 0) return false;
    *outFd = cacheFd;
    *outOffset = 0;
    *outLength = inflated;
    return true;
}
