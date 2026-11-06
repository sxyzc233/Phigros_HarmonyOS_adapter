/*
 * il2cpp data directory support.
 *
 * The engine resolves its own assets through the AssetManager (served from the HAP zip), but libil2cpp
 * reads global-metadata.dat with plain file IO and cannot see inside the archive. So the metadata is
 * extracted once to a real directory and il2cpp_set_data_dir is wrapped: if the path the engine passes
 * does not actually contain the metadata, the prepared directory is substituted and the substitution is
 * logged, so the intended path is still visible in the trace.
 */
#include <cstdio>
#include <cstring>
#include <string>

#include <cerrno>
#include <fcntl.h>
#include <hilog/log.h>
#include <sys/stat.h>
#include <unistd.h>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "TransIl2cpp"

#define LOGI(fmt, ...) OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG, fmt, ##__VA_ARGS__)

/* Implemented in asset_zip.cpp: resolves a path under the HAP's rawfile assets. */
extern "C" bool ReaAssetOpen(const char *assetPath, int *outFd, int64_t *outOffset, int64_t *outLength);

namespace {

const char *kPreparedDir = "/data/storage/el2/base/files/rea/4.0.1.2";
const char *kMetadataAsset = "bin/Data/Managed/Metadata/global-metadata.dat";
const char *kMetadataName = "global-metadata.dat";

void (*g_realSetDataDir)(const char *) = nullptr;

bool MakeDirectories(const std::string &path)
{
    /* Create each component; existing directories are fine. */
    for (size_t i = 1; i <= path.size(); i++) {
        if (i != path.size() && path[i] != '/') continue;
        std::string part = path.substr(0, i);
        if (mkdir(part.c_str(), 0770) != 0 && errno != EEXIST) return false;
    }
    return true;
}

bool FileExists(const std::string &path)
{
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

/* Copies one asset out of the HAP into a real file. Returns false when the asset is unavailable. */
bool ExtractAsset(const char *assetPath, const std::string &destination)
{
    int fd = -1;
    int64_t offset = 0;
    int64_t length = 0;
    if (!ReaAssetOpen(assetPath, &fd, &offset, &length)) {
        LOGW("asset not found: %s", assetPath);
        return false;
    }
    if (fd < 0 || length <= 0) {
        LOGW("asset has no payload: %s", assetPath);
        return false;
    }

    FILE *output = fopen(destination.c_str(), "wb");
    if (!output) {
        LOGW("cannot open %s for writing", destination.c_str());
        return false;
    }

    char buffer[64 * 1024];
    int64_t remaining = length;
    int64_t cursor = offset;
    bool ok = true;
    while (remaining > 0) {
        size_t want = (size_t)(remaining < (int64_t)sizeof(buffer) ? remaining : (int64_t)sizeof(buffer));
        ssize_t got = pread(fd, buffer, want, (off_t)cursor);
        if (got <= 0) {
            ok = false;
            break;
        }
        if (fwrite(buffer, 1, (size_t)got, output) != (size_t)got) {
            ok = false;
            break;
        }
        cursor += got;
        remaining -= got;
    }
    fclose(output);
    if (!ok) {
        LOGW("failed while writing %s", destination.c_str());
        return false;
    }
    LOGI("extracted %{public}s (%{public}lld bytes) -> %{public}s", assetPath, (long long)length, destination.c_str());
    return true;
}

} // namespace

/*
 * Extracts global-metadata.dat into the prepared directory. Both layouts are produced, because il2cpp
 * builds the path either as <dataDir>/Metadata/global-metadata.dat or <dataDir>/global-metadata.dat
 * depending on the platform, and the extra copy costs nothing after the first run.
 */
extern "C" void ReaPrepareIl2cppData()
{
    std::string root = kPreparedDir;
    MakeDirectories(root);

    std::string metadataDir = root + "/Metadata";
    MakeDirectories(metadataDir);

    bool copied = false;
    if (!FileExists(metadataDir + "/" + kMetadataName)) {
        copied = ExtractAsset(kMetadataAsset, metadataDir + "/" + kMetadataName);
    } else {
        copied = true;
    }
    if (copied && !FileExists(root + "/" + kMetadataName)) {
        ExtractAsset(kMetadataAsset, root + "/" + kMetadataName);
    }
    LOGI("il2cpp data prepared at %{public}s (copied=%{public}d)", kPreparedDir, copied ? 1 : 0);
}

/* Lets the slot filler hand over the real entry point before the wrapper is installed. */
extern "C" void ReaSetRealIl2cppDataDir(void *fn)
{
    g_realSetDataDir = (void (*)(const char *))fn;
}

/*
 * Replacement for il2cpp_set_data_dir. The engine's path is logged; if it does not contain the metadata,
 * the prepared directory is used instead so metadata loading can actually succeed.
 */
extern "C" void ReaIl2cppSetDataDir(const char *path)
{
    std::string requested = path ? path : "";
    std::string candidate = requested + "/Metadata/" + kMetadataName;
    std::string candidateFlat = requested + "/" + kMetadataName;

    bool usable = FileExists(candidate) || FileExists(candidateFlat);
    const char *effective = usable ? path : kPreparedDir;
    LOGI("il2cpp_set_data_dir(\"%{public}s\") usable=%{public}d -> \"%{public}s\"", requested.c_str(), usable ? 1 : 0, effective);

    if (g_realSetDataDir) {
        g_realSetDataDir(effective);
    }
}