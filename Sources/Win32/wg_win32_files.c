#include "wg_win32_files.h"
#include "wg_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>

#define TAG "Files"

static char s_exe_path[1024] = {0};
static char s_exe_dir[1024]  = {0};
static char s_wizard_bmp[1024] = {0};  // last-extracted welcome/wizard .bmp
static char s_cwd_win[1024]  = {0};    // current dir (Windows path), SetCurrentDirectory

const char *wg_files_wizard_bmp(void) {
    return s_wizard_bmp[0] ? s_wizard_bmp : NULL;
}

void wg_files_set_cwd(const char *win_path) {
    if (!win_path) { s_cwd_win[0] = 0; return; }
    strncpy(s_cwd_win, win_path, sizeof(s_cwd_win) - 1);
    s_cwd_win[sizeof(s_cwd_win) - 1] = 0;
}
const char *wg_files_get_cwd(void) {
    return s_cwd_win[0] ? s_cwd_win : NULL;
}
// The "bottle" — a persistent Windows prefix (like a Wine/CrossOver bottle or a
// GPTK bottle). drive_c is the guest C:\ root; every C:\ path maps under it, so
// the installed app lands in e.g. drive_c/Program Files (x86)/Steam instead of
// being scattered through one-off temp mappings.
static char s_drive_c[1024]  = {0};
static bool s_win_path_cached = false;

// Recursively create a directory and all missing parents (like `mkdir -p`).
static void mkdir_p(const char *path) {
    if (!path || !path[0]) return;
    char tmp[1024];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { /* keep going */ }
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

void wg_files_ensure_parents(const char *real_path) {
    if (!real_path || !real_path[0]) return;
    char parent[1024];
    strncpy(parent, real_path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    char *last = strrchr(parent, '/');
    if (last) { *last = '\0'; mkdir_p(parent); }
}

// Lazily create the bottle (drive_c + a few standard dirs). Anchored next to the
// loaded .exe (which lives in the app's Documents dir), so the bottle persists
// across runs.
static void ensure_bottle(void) {
    if (s_drive_c[0] || !s_exe_dir[0]) return;
    // If the exe already lives inside a bottle (".../drive_c/..."), anchor C:\
    // at that drive_c — a game run from its installed location must see the
    // existing prefix (its own dir at C:\Program Files (x86)\...), not a fresh
    // bottle nested next to its exe. Also makes a chain-loaded steam.exe see
    // the tree its installer just wrote.
    const char *dc = strstr(s_exe_path, "/drive_c/");
    if (dc) {
        size_t n = (size_t)(dc - s_exe_path) + strlen("/drive_c");
        if (n >= sizeof(s_drive_c)) n = sizeof(s_drive_c) - 1;
        memcpy(s_drive_c, s_exe_path, n);
        s_drive_c[n] = '\0';
    } else {
        snprintf(s_drive_c, sizeof(s_drive_c), "%sBottle/drive_c", s_exe_dir);
    }
    mkdir_p(s_drive_c);
    const char *subdirs[] = {
        "Temp", "Program Files", "Program Files (x86)", "windows", NULL
    };
    for (int i = 0; subdirs[i]; i++) {
        char sub[1024];
        snprintf(sub, sizeof(sub), "%s/%s", s_drive_c, subdirs[i]);
        mkdir(sub, 0755);
    }
    WG_LOGI(TAG, "Bottle drive_c: %s", s_drive_c);
}

const char *wg_files_drive_c(void) {
    ensure_bottle();
    return s_drive_c;
}

// Recursively delete a file or directory tree (no shell — iOS forbids system()).
static void rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                    continue;
                char child[1200];
                snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
                rm_rf(child);
            }
            closedir(d);
        }
        rmdir(path);
    } else {
        unlink(path);
    }
}

void wg_files_reset_temp(void) {
    ensure_bottle();
    if (!s_drive_c[0]) return;
    char temp[1024];
    snprintf(temp, sizeof(temp), "%s/Temp", s_drive_c);
    DIR *d = opendir(temp);
    if (!d) return;
    struct dirent *e;
    int cleared = 0;
    while ((e = readdir(d))) {
        // NSIS plug-in dirs/files look like ns<hex>.tmp; clear them so each run
        // gets a fresh plug-ins directory (NSIS bails on a pre-existing one).
        size_t n = strlen(e->d_name);
        bool is_ns_tmp = (n > 4 && strncmp(e->d_name, "ns", 2) == 0 &&
                          strcasecmp(e->d_name + n - 4, ".tmp") == 0);
        if (is_ns_tmp) {
            char full[1100];
            snprintf(full, sizeof(full), "%s/%s", temp, e->d_name);
            rm_rf(full);
            cleared++;
        }
    }
    closedir(d);
    if (cleared) WG_LOGI(TAG, "Reset bottle Temp: cleared %d ns*.tmp", cleared);
}

typedef struct {
    FILE    *fp;
    uint32_t handle;
    bool     in_use;
    char     path[512];
    uint8_t *inj;        // in-memory override content (config injection); NULL = use fp
    long     inj_len, inj_pos;
} WGFileEntry;

static WGFileEntry s_files[WG_MAX_FILE_HANDLES] = {0};
static uint32_t s_next_handle = WG_FILE_HANDLE_BASE;

void wg_files_set_exe_path(const char *path) {
    strncpy(s_exe_path, path, sizeof(s_exe_path) - 1);
    strncpy(s_exe_dir, path, sizeof(s_exe_dir) - 1);
    char *last_slash = strrchr(s_exe_dir, '/');
    if (last_slash) *(last_slash + 1) = '\0';
    WG_LOGI(TAG, "EXE path: %s", s_exe_path);
    WG_LOGI(TAG, "EXE dir: %s", s_exe_dir);
    s_drive_c[0] = '\0';   // re-anchor the bottle to this exe's directory
    s_win_path_cached = false;
    ensure_bottle();
}

// Recursively find `name` under `dir`, writing the result to `out`.
static bool find_in_bottle(const char *dir, const char *name, char *out, int outsz) {
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        if (strcasecmp(e->d_name, name) == 0) {
            snprintf(out, outsz, "%s", full);
            closedir(d);
            return true;
        }
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (find_in_bottle(full, name, out, outsz)) {
                closedir(d);
                return true;
            }
        }
    }
    closedir(d);
    return false;
}

const char *wg_files_exe_win_path(void) {
    static char win[1024];
    if (s_win_path_cached) return win;
    ensure_bottle();
    size_t dc_len = strlen(s_drive_c);
    if (dc_len > 0 && strncmp(s_exe_path, s_drive_c, dc_len) == 0) {
        const char *rel = s_exe_path + dc_len;
        while (*rel == '/') rel++;
        snprintf(win, sizeof(win), "C:\\%s", rel);
        for (char *p = win; *p; p++) { if (*p == '/') *p = '\\'; }
    } else {
        const char *fname = strrchr(s_exe_path, '/');
        fname = fname ? fname + 1 : s_exe_path;
        char found[1024] = {0};
        if (dc_len > 0 && find_in_bottle(s_drive_c, fname, found, sizeof(found))) {
            const char *rel = found + dc_len;
            while (*rel == '/') rel++;
            snprintf(win, sizeof(win), "C:\\%s", rel);
            for (char *p = win; *p; p++) { if (*p == '/') *p = '\\'; }
            WG_LOGI(TAG, "Exe found in bottle: %s -> %s", fname, win);
        } else {
            snprintf(win, sizeof(win), "C:\\a.exe");
        }
    }
    s_win_path_cached = true;
    return win;
}

void wg_files_reset_win_path_cache(void) {
    s_win_path_cached = false;
}

static void fix_separators(char *path) {
    for (char *p = path; *p; p++) {
        if (*p == '\\') *p = '/';
    }
}

const char *wg_files_map_path(uint32_t guest_path_addr, void *blink,
                               char *buf, int bufsize) {
    ensure_bottle();

    // Strip the Win32 long-path prefix \\?\ (or //?/): "\\?\C:\x" is identical to
    // "C:\x". Without this, apps that use it (Steam: \\?\C:\package\...) create a
    // literal "?" directory in the bottle. Also handle \\?\UNC\ -> \\ (rare).
    const char *src = buf;
    if ((src[0] == '\\' || src[0] == '/') && (src[1] == '\\' || src[1] == '/') &&
        src[2] == '?' && (src[3] == '\\' || src[3] == '/')) {
        src += 4;
        if (strncasecmp(src, "UNC\\", 4) == 0 || strncasecmp(src, "UNC/", 4) == 0)
            src += 4; // \\?\UNC\server\share -> server\share (best effort)
    }

    // Some guest path logic concatenates its base dir with an already-absolute
    // path, producing a doubled path like
    //   "C:/…/Win64/../../C:/…/Win64/../../../Engine/GlobalShaderCache-*.bin".
    // On Windows a drive letter ("X:\" or "X:/") appearing after position 0
    // restarts the absolute path, so the LAST such segment is the real target.
    // Use it (fixes UE4's GlobalShaderCache lookup, which otherwise "goes
    // missing" and aborts PreInit at CompileGlobalShaderMap).
    for (int i = (int)strlen(src) - 3; i > 0; i--) {
        if (((src[i] >= 'A' && src[i] <= 'Z') || (src[i] >= 'a' && src[i] <= 'z')) &&
            src[i+1] == ':' && (src[i+2] == '\\' || src[i+2] == '/')) {
            src += i;
            break;
        }
    }

    // The installer .exe itself is the file the user picked — it lives in the
    // app sandbox, not inside the bottle, so keep mapping it to the real path.
    if (strcasecmp(src, "C:\\a.exe") == 0 ||
        strcasecmp(src, "C:/a.exe") == 0) {
        return s_exe_path;
    }

    // Relative path (no "X:" drive, not rooted at a separator)? Resolve it
    // against the tracked current directory. NSIS's SetOutPath does a
    // SetCurrentDirectory then extracts files by bare name — without this they
    // land in the drive_c root instead of $INSTDIR (C:\Program Files\Steam).
    bool is_abs = (src[0] && src[1] == ':') || src[0] == '\\' || src[0] == '/';
    static char rel_join[1024];
    if (!is_abs && s_cwd_win[0]) {
        snprintf(rel_join, sizeof(rel_join), "%s\\%s", s_cwd_win, src);
        src = rel_join;
    }

    // Everything else lives on the bottle's C: drive. Strip a leading drive
    // letter ("C:") and any leading separators, then join under drive_c. This
    // makes C:\Temp\..., C:\Program Files (x86)\Steam\..., C:\Windows\..., etc.
    // all resolve to real, persistent paths inside the bottle — so an install
    // ends up in a proper drive_c tree instead of scattered temp files.
    const char *p = src;
    if (p[0] && p[1] == ':') p += 2;            // skip drive letter (any drive)
    while (*p == '\\' || *p == '/') p++;        // skip leading separators

    static char mapped[1024];
    if (*p)
        snprintf(mapped, sizeof(mapped), "%s/%s", s_drive_c, p);
    else
        snprintf(mapped, sizeof(mapped), "%s", s_drive_c);  // bare "C:\" -> root
    fix_separators(mapped);
    return mapped;
}

uint32_t wg_files_create(const char *real_path, uint32_t access, uint32_t creation) {
    if (!real_path || !real_path[0]) return 0xFFFFFFFF;

    // Determine open mode from creation disposition and access
    const char *mode;
    bool wants_write = (access & 0x40000000) != 0; // GENERIC_WRITE
    bool wants_read = (access & 0x80000000) != 0;  // GENERIC_READ

    switch (creation) {
        case 1: mode = wants_write ? "wb+" : "rb"; break; // CREATE_NEW
        case 2: mode = "wb+"; break;                       // CREATE_ALWAYS
        case 3: mode = wants_write ? "rb+" : "rb"; break;  // OPEN_EXISTING
        case 4: mode = wants_write ? "ab+" : "rb"; break;  // OPEN_ALWAYS
        case 5: mode = "wb"; break;                         // TRUNCATE_EXISTING
        default: mode = wants_write ? "wb+" : "rb"; break;
    }

    FILE *fp = fopen(real_path, mode);
    if (!fp && (creation == 2 || creation == 4)) {
        // CREATE_ALWAYS or OPEN_ALWAYS — create the full parent dir tree (deep
        // install paths like drive_c/Program Files (x86)/Steam/bin/ need every
        // intermediate directory), then retry.
        wg_files_ensure_parents(real_path);
        fp = fopen(real_path, mode);
    }
    if (!fp) {
        WG_LOGD(TAG, "Open failed: %s (mode=%s)", real_path, mode);
        return 0xFFFFFFFF;
    }

    // Find a free slot
    for (int i = 0; i < WG_MAX_FILE_HANDLES; i++) {
        if (!s_files[i].in_use) {
            s_files[i].fp = fp;
            s_files[i].handle = s_next_handle++;
            s_files[i].in_use = true;
            s_files[i].inj = NULL; s_files[i].inj_len = 0; s_files[i].inj_pos = 0;
            strncpy(s_files[i].path, real_path, sizeof(s_files[i].path) - 1);
            // CONFIG INJECTION (root fix for the anim-curve-compression fatal): UE4 reads
            // its Engine defaults ([Animation.DefaultObjectSettings]) from the pak's
            // BaseEngine.ini, which our pak layer doesn't deliver to the game's config
            // system — so GConfig has no default curve/bone compression settings path and
            // the boot fatals before the menu. Serve Engine.ini reads with those defaults
            // appended so they land in GConfig's Engine branch (the game reads this file
            // as the user config layer of GEngineIni).
            if (strcasestr(real_path, "Config") &&
                strcasestr(real_path, "Engine.ini") &&
                !strcasestr(real_path, "GameUserSettings")) {
                static const char *INJ =
                    "\n[Animation.DefaultObjectSettings]\n"
                    "BoneCompressionSettings=\"/Engine/Animation/DefaultAnimBoneCompressionSettings\"\n"
                    "CurveCompressionSettings=\"/Engine/Animation/DefaultAnimCurveCompressionSettings\"\n";
                long ilen = (long)strlen(INJ);
                fseek(fp, 0, SEEK_END); long flen = ftell(fp); fseek(fp, 0, SEEK_SET);
                if (flen < 0) flen = 0;
                uint8_t *buf = (uint8_t *)malloc((size_t)flen + (size_t)ilen);
                if (buf) {
                    size_t got = fread(buf, 1, (size_t)flen, fp);
                    memcpy(buf + got, INJ, (size_t)ilen);
                    s_files[i].inj = buf;
                    s_files[i].inj_len = (long)got + ilen;
                    s_files[i].inj_pos = 0;
                    WG_LOGI(TAG, "config-inject: %s (+anim defaults, %ld bytes)", real_path, s_files[i].inj_len);
                }
                fseek(fp, 0, SEEK_SET);
            }
            // Remember the wizard/welcome bitmap as it's extracted, so the
            // nsDialogs welcome page can draw it natively (the script normally
            // loads it via System::Call, which we don't emulate).
            if (strcasestr(real_path, "wizard") && strcasestr(real_path, ".bmp"))
                strncpy(s_wizard_bmp, real_path, sizeof(s_wizard_bmp) - 1);
            WG_LOGI(TAG, "Opened: %s -> handle 0x%X", real_path, s_files[i].handle);
            return s_files[i].handle;
        }
    }

    fclose(fp);
    return 0xFFFFFFFF;
}

static WGFileEntry *find_file(uint32_t handle) {
    for (int i = 0; i < WG_MAX_FILE_HANDLES; i++) {
        if (s_files[i].in_use && s_files[i].handle == handle)
            return &s_files[i];
    }
    return NULL;
}

bool wg_files_read(uint32_t handle, void *buf, uint32_t bytes, uint32_t *bytes_read) {
    WGFileEntry *f = find_file(handle);
    if (!f) return false;
    if (f->inj) {                              // serve from the injected buffer
        long avail = f->inj_len - f->inj_pos; if (avail < 0) avail = 0;
        long n = (long)bytes < avail ? (long)bytes : avail;
        if (n > 0) memcpy(buf, f->inj + f->inj_pos, (size_t)n);
        f->inj_pos += n;
        if (bytes_read) *bytes_read = (uint32_t)n;
        return true;
    }
    if (!f->fp) {
        memset(buf, 0, bytes);
        if (bytes_read) *bytes_read = bytes;
        return true;
    }
    size_t n = fread(buf, 1, bytes, f->fp);
    if (bytes_read) *bytes_read = (uint32_t)n;
    return true;
}

bool wg_files_write(uint32_t handle, const void *buf, uint32_t bytes, uint32_t *bytes_written) {
    WGFileEntry *f = find_file(handle);
    if (!f) return false;
    if (!f->fp) {
        if (bytes_written) *bytes_written = bytes;
        return true;
    }
    size_t n = fwrite(buf, 1, bytes, f->fp);
    if (bytes_written) *bytes_written = (uint32_t)n;
    return true;
}

uint32_t wg_files_get_size(uint32_t handle) {
    WGFileEntry *f = find_file(handle);
    if (!f) return 0xFFFFFFFF;
    if (f->inj) return (uint32_t)f->inj_len;
    if (!f->fp) return 0;
    long pos = ftell(f->fp);
    fseek(f->fp, 0, SEEK_END);
    long size = ftell(f->fp);
    fseek(f->fp, pos, SEEK_SET);
    return (uint32_t)size;
}

uint32_t wg_files_set_pointer(uint32_t handle, int32_t distance, uint32_t method) {
    WGFileEntry *f = find_file(handle);
    if (!f) return 0xFFFFFFFF;
    if (f->inj) {
        long base = (method == 1) ? f->inj_pos : (method == 2) ? f->inj_len : 0;
        f->inj_pos = base + distance; if (f->inj_pos < 0) f->inj_pos = 0;
        if (f->inj_pos > f->inj_len) f->inj_pos = f->inj_len;
        return (uint32_t)f->inj_pos;
    }
    int whence;
    switch (method) {
        case 0: whence = SEEK_SET; break;
        case 1: whence = SEEK_CUR; break;
        case 2: whence = SEEK_END; break;
        default: whence = SEEK_SET; break;
    }
    fseek(f->fp, distance, whence);
    return (uint32_t)ftell(f->fp);
}

// 64-bit seek — needed for pak footers, which sit near the end of multi-GB
// .pak files (offsets that don't fit in the 32-bit variant above).
uint64_t wg_files_set_pointer_64(uint32_t handle, int64_t offset, uint32_t method) {
    WGFileEntry *f = find_file(handle);
    if (f && f->inj) {
        long base = (method == 1) ? f->inj_pos : (method == 2) ? f->inj_len : 0;
        f->inj_pos = base + (long)offset; if (f->inj_pos < 0) f->inj_pos = 0;
        if (f->inj_pos > f->inj_len) f->inj_pos = f->inj_len;
        return (uint64_t)f->inj_pos;
    }
    if (!f || !f->fp) return 0;
    int whence = (method == 1) ? SEEK_CUR : (method == 2) ? SEEK_END : SEEK_SET;
    fseeko(f->fp, (off_t)offset, whence);
    off_t p = ftello(f->fp);
    return p < 0 ? 0 : (uint64_t)p;
}

void wg_files_prepopulate_nsis_data(const char *exe_path, const char *tmp_path) {
    // Find the NSIS data section in the exe and copy it to the tmp file.
    // NSIS searches for 'NullsoftInst' marker, then reads the header
    // which tells it how much data follows.
    FILE *exe = fopen(exe_path, "rb");
    if (!exe) return;

    fseek(exe, 0, SEEK_END);
    long exe_size = ftell(exe);
    fseek(exe, 0, SEEK_SET);

    // Search for NullsoftInst marker
    uint8_t *exe_data = malloc(exe_size);
    if (!exe_data) { fclose(exe); return; }
    fread(exe_data, 1, exe_size, exe);
    fclose(exe);

    long nsi_offset = -1;
    for (long i = 0; i < exe_size - 16; i++) {
        if (memcmp(exe_data + i, "NullsoftInst", 12) == 0) {
            nsi_offset = i;
            break;
        }
    }

    if (nsi_offset < 0) {
        WG_LOGW("Files", "No NullsoftInst marker found in exe");
        free(exe_data);
        return;
    }

    // Parse NSIS firstheader
    // +12: header_length (compressed header size)
    // +16: archive_size (total data including header)
    uint32_t header_len, archive_size;
    memcpy(&header_len, exe_data + nsi_offset + 12, 4);
    memcpy(&archive_size, exe_data + nsi_offset + 16, 4);

    // Data starts at nsi_offset + 20 (after firstheader)
    long data_start = nsi_offset + 20;
    // File data starts after the compressed header
    long file_data_offset = data_start + header_len;
    long file_data_size = archive_size - header_len;

    if (file_data_offset < 0 || file_data_offset >= exe_size || file_data_size <= 0) {
        WG_LOGW("Files", "Invalid NSIS data layout");
        free(exe_data);
        return;
    }

    if (file_data_offset + file_data_size > exe_size) {
        file_data_size = exe_size - file_data_offset;
    }

    // Write the file data section to the tmp file
    FILE *tmp = fopen(tmp_path, "wb");
    if (!tmp) {
        WG_LOGW("Files", "Can't create prepopulated tmp file");
        free(exe_data);
        return;
    }

    fwrite(exe_data + file_data_offset, 1, file_data_size, tmp);
    fclose(tmp);
    free(exe_data);

    WG_LOGI("Files", "Pre-populated NSIS data: %ld bytes at offset %ld -> %s",
            file_data_size, file_data_offset, tmp_path);
}

uint32_t wg_files_create_null(void) {
    for (int i = 0; i < WG_MAX_FILE_HANDLES; i++) {
        if (!s_files[i].in_use) {
            s_files[i].fp = NULL; // null = discard writes, return success
            s_files[i].handle = s_next_handle++;
            s_files[i].in_use = true;
            strncpy(s_files[i].path, "/dev/null", sizeof(s_files[i].path));
            return s_files[i].handle;
        }
    }
    return 0xFFFFFFFF;
}

// Copy a just-written file to a stable cache path so it survives NSIS deleting
// its temp dir. Used for the welcome/wizard bitmap, which NSIS extracts to a
// temp dir, then loads (via System::Call LoadImage) only after the dir is gone.
static void wg_cache_wizard_bmp(const char *src) {
    const char *dc = wg_files_drive_c();
    if (!dc || !dc[0]) return;
    char dst[1024];
    snprintf(dst, sizeof dst, "%s/wg_wizard_cache.bmp", dc);
    FILE *in = fopen(src, "rb"); if (!in) return;
    FILE *out = fopen(dst, "wb"); if (!out) { fclose(in); return; }
    char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in); fclose(out);
    strncpy(s_wizard_bmp, dst, sizeof(s_wizard_bmp) - 1);
    s_wizard_bmp[sizeof(s_wizard_bmp) - 1] = '\0';
}

bool wg_files_close(uint32_t handle) {
    WGFileEntry *f = find_file(handle);
    if (!f) return false;
    if (f->inj) { free(f->inj); f->inj = NULL; f->inj_len = f->inj_pos = 0; }
    if (f->fp) fclose(f->fp);
    if (strcasestr(f->path, "wizard") && strcasestr(f->path, ".bmp"))
        wg_cache_wizard_bmp(f->path);
    f->in_use = false;
    WG_LOGD(TAG, "Closed handle 0x%X", handle);
    return true;
}
