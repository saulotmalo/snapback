/**
 * snapback — Nintendo Switch sysmodule
 *
 * Auto-reconnects Apple headphones (AirPods / Beats) on boot by initiating
 * a Bluetooth audio connection to any paired audio device(s) found in range.
 *
 * Targets: Atmosphere CFW, firmware 13.0.0+ (native BT audio support)
 * Build:   devkitPro / devkitA64 + libnx
 *
 * Config (optional):
 *   /switch/snapback/config.ini
 *
 * Log output:
 *   /switch/snapback/log.txt
 */

#include <switch.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cctype>
#include <strings.h>
#include <sys/stat.h>

// ─── Sysmodule identity ────────────────────────────────────────────────────
// CRITICAL: override libnx's weak __appInit. The default pulls in time:u (and
// friends). Holding time:u from a boot2 sysmodule blanks the HOME clock after
// Sphaira / other applets suspend — and if time:u is not in service_access the
// process User-Breaks (Atmosphere 2345-0038) instead. We only need sm + fs.
extern "C" {
    u32 __nx_applet_type     = AppletType_None;
    u32 __nx_fs_num_sessions = 1;

    void __libnx_initheap(void) {
        static char heap[256 * 1024];
        extern char* fake_heap_start;
        extern char* fake_heap_end;
        fake_heap_start = heap;
        fake_heap_end   = heap + sizeof(heap);
    }

    void __appInit(void) {
        Result rc = smInitialize();
        if (R_FAILED(rc))
            diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

        // Required so libnx picks the correct btm IPC (13.0+ GetDeviceInfo).
        // Without this, hosversion stays 0 and btmGetDeviceInfo returns 0x4B59.
        rc = setsysInitialize();
        if (R_SUCCEEDED(rc)) {
            SetSysFirmwareVersion fw = {};
            rc = setsysGetFirmwareVersion(&fw);
            if (R_SUCCEEDED(rc))
                hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            setsysExit();
        }

        rc = fsInitialize();
        if (R_FAILED(rc))
            diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));

        fsdevMountSdmc();
        // Intentionally NO timeInitialize / hidInitialize / pscmInitialize —
        // those forever sessions blank the HOME clock after Sphaira.
    }

    void __appExit(void) {
        fsdevUnmountAll();
        fsExit();
        smExit();
    }
}

// ─── Paths ─────────────────────────────────────────────────────────────────
#define CONFIG_PATH "sdmc:/switch/snapback/config.ini"
#define LOG_PATH    "sdmc:/switch/snapback/log.txt"

// ─── Mode ──────────────────────────────────────────────────────────────────
typedef enum { MODE_POLLING, MODE_WAKE } RunMode;
static RunMode g_mode = MODE_POLLING;

// ─── Tunables ──────────────────────────────────────────────────────────────
static int g_polling_interval_sec = 12;
static int g_boot_delay_sec       = 18;
static int g_max_retries          = 0;
// After a verified connect, exit the process so nothing stays resident.
// Default on: safest for the HOME clock. Set 0 in config.ini to keep polling.
static int g_exit_when_connected  = 1;

static BtdrvAddress g_filter_addr    = {};
static bool         g_filter_set     = false;
static char         g_filter_name[64] = {};

// ─── Logging ───────────────────────────────────────────────────────────────
// Open/append/close per line so FTP (and the next boot) can read the file —
// holding fopen for the life of the module returns "resource busy" over sys-ftp.
static int g_log_lines = 0;
static const int LOG_ROTATE_LINES = 500;
static bool g_log_ready = false;

static void log_open() {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/snapback", 0777);
    FILE* f = fopen(LOG_PATH, "w");
    if (f) fclose(f);
    g_log_lines = 0;
    g_log_ready = true;
}

static void log_print(const char* fmt, ...) {
    if (!g_log_ready) return;
    if (g_log_lines >= LOG_ROTATE_LINES) {
        FILE* rot = fopen(LOG_PATH, "w");
        if (rot) {
            fprintf(rot, "[log] rotated\n");
            fclose(rot);
        }
        g_log_lines = 0;
    }
    FILE* f = fopen(LOG_PATH, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
    g_log_lines++;
}

// ─── Address helpers ───────────────────────────────────────────────────────
static void addr_to_str(const BtdrvAddress& a, char* out, size_t sz) {
    snprintf(out, sz, "%02X:%02X:%02X:%02X:%02X:%02X",
             a.address[0], a.address[1], a.address[2],
             a.address[3], a.address[4], a.address[5]);
}

static bool addr_eq(const BtdrvAddress& a, const BtdrvAddress& b) {
    return memcmp(a.address, b.address, 6) == 0;
}

// ─── Config parser ─────────────────────────────────────────────────────────
static bool parse_mac(const char* s, BtdrvAddress& out) {
    unsigned b[6] = {};
    if (sscanf(s, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0],&b[1],&b[2],&b[3],&b[4],&b[5]) != 6 &&
        sscanf(s, "%02X:%02X:%02X:%02X:%02X:%02X",
               &b[0],&b[1],&b[2],&b[3],&b[4],&b[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) out.address[i] = (u8)b[i];
    return true;
}

static void trim(char* s) {
    int n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r' ||
                     s[n-1] == ' '  || s[n-1] == '\t')) s[--n] = 0;
    int i = 0;
    while (s[i] == ' ' || s[i] == '\t') i++;
    if (i) memmove(s, s+i, strlen(s+i)+1);
}

static void load_config() {
    FILE* f = fopen(CONFIG_PATH, "r");
    if (!f) {
        log_print("[cfg] No config.ini — using defaults (polling mode)\n");
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';') continue;
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = line; trim(key);
        char* val = eq+1; trim(val);

        if (strcasecmp(key, "address") == 0) {
            if (parse_mac(val, g_filter_addr)) {
                g_filter_set = true;
                log_print("[cfg] Filter address: %s\n", val);
            }
        } else if (strcasecmp(key, "name") == 0) {
            strncpy(g_filter_name, val, sizeof(g_filter_name)-1);
            log_print("[cfg] Device name: %s\n", g_filter_name);
        } else if (strcasecmp(key, "mode") == 0) {
            if (strcasecmp(val, "wake") == 0) {
                g_mode = MODE_WAKE;
                log_print("[cfg] Mode: wake\n");
            } else {
                g_mode = MODE_POLLING;
                log_print("[cfg] Mode: polling\n");
            }
        } else if (strcasecmp(key, "polling_interval") == 0 ||
                   strcasecmp(key, "retry_interval") == 0) {
            g_polling_interval_sec = atoi(val);
            log_print("[cfg] Polling interval: %ds\n", g_polling_interval_sec);
        } else if (strcasecmp(key, "boot_delay") == 0) {
            g_boot_delay_sec = atoi(val);
            log_print("[cfg] Boot delay: %ds\n", g_boot_delay_sec);
        } else if (strcasecmp(key, "max_retries") == 0) {
            g_max_retries = atoi(val);
            log_print("[cfg] Max retries: %d\n", g_max_retries);
        } else if (strcasecmp(key, "exit_when_connected") == 0) {
            g_exit_when_connected = atoi(val);
            log_print("[cfg] Exit when connected: %d\n", g_exit_when_connected);
        }
    }
    fclose(f);
}

// ─── Bluetooth session ─────────────────────────────────────────────────────
static void bt_open() {
    Result rc = btmInitialize();
    log_print("[bt] btmInitialize => 0x%08X\n", rc);
    rc = btdrvInitialize();
    log_print("[bt] btdrvInitialize => 0x%08X\n", rc);
}

static void bt_close() {
    btdrvExit();
    btmExit();
}

// ─── Bluetooth helpers ─────────────────────────────────────────────────────
static int get_paired_audio_devices(BtdrvAddress* out, int max_out) {
    int count = 0;
    {
        const s32 MAX_DEV = 8;
        BtmDeviceInfoV13 devs[MAX_DEV] = {};
        s32 total = 0;
        Result rc = btmGetDeviceInfo(BtmProfile_Audio, devs, MAX_DEV, &total);
        if (R_SUCCEEDED(rc)) {
            log_print("[bt] btmGetDeviceInfo(Audio) => %d device(s)\n", (int)total);
            for (s32 i = 0; i < total && i < MAX_DEV && count < max_out; i++) {
                char addr_s[18];
                addr_to_str(devs[i].addr, addr_s, sizeof(addr_s));
                log_print("[bt]   paired[%d] = %s  \"%s\"\n", (int)i, addr_s, devs[i].name);
                if (!g_filter_set || addr_eq(devs[i].addr, g_filter_addr))
                    out[count++] = devs[i].addr;
            }
            return count;
        }
        log_print("[bt] btmGetDeviceInfo rc=0x%08X, trying legacy\n", rc);
    }
    {
        BtmDeviceInfoList list = {};
        Result rc = btmLegacyGetDeviceInfo(&list);
        if (R_SUCCEEDED(rc)) {
            log_print("[bt] btmLegacyGetDeviceInfo => %u device(s)\n", list.device_count);
            for (u32 i = 0; i < list.device_count && count < max_out; i++) {
                BtdrvAddress a = list.devices[i].addr;
                char addr_s[18];
                addr_to_str(a, addr_s, sizeof(addr_s));
                log_print("[bt]   legacy[%d] = %s\n", (int)i, addr_s);
                if (!g_filter_set || addr_eq(a, g_filter_addr))
                    out[count++] = a;
            }
        } else {
            log_print("[bt] btmLegacyGetDeviceInfo rc=0x%08X\n", rc);
        }
    }
    return count;
}

static bool is_audio_connected(const BtdrvAddress& target) {
    const s32 MAX_DEV = 8;
    BtmConnectedDeviceV13 connected[MAX_DEV] = {};
    s32 total = 0;
    Result rc = btmGetDeviceCondition(BtmProfile_Audio, connected, MAX_DEV, &total);
    if (R_SUCCEEDED(rc)) {
        for (s32 i = 0; i < total && i < MAX_DEV; i++)
            if (addr_eq(connected[i].address, target)) return true;
        return false;
    }
    BtmDeviceCondition cond = {};
    rc = btmLegacyGetDeviceCondition(&cond);
    if (R_SUCCEEDED(rc)) {
        for (u32 i = 0; i < cond.v900.connected_count && i < 8; i++)
            if (addr_eq(cond.v900.devices[i].address, target)) return true;
    }
    return false;
}

static Result connect_audio_device(const BtdrvAddress& addr) {
    char addr_s[18];
    addr_to_str(addr, addr_s, sizeof(addr_s));
    log_print("[bt] Connecting to %s ...\n", addr_s);

    Result rc = btdrvOpenAudioConnection(addr);
    log_print("[bt]   btdrvOpenAudioConnection => 0x%08X\n", rc);
    if (R_SUCCEEDED(rc)) return rc;

    rc = btdrvTriggerConnection(addr, (u16)0);
    log_print("[bt]   btdrvTriggerConnection => 0x%08X\n", rc);
    return rc;
}

// Wait up to ~5s for the device to appear in btmGetDeviceCondition.
// Old builds that "worked" often needed a full poll interval (~12s) before
// "already connected" showed up — 500ms was far too short.
static bool wait_until_connected(const BtdrvAddress& addr, int timeout_ms) {
    const int step_ms = 250;
    for (int waited = 0; waited < timeout_ms; waited += step_ms) {
        if (is_audio_connected(addr)) return true;
        svcSleepThread((u64)step_ms * 1'000'000ULL);
    }
    return is_audio_connected(addr);
}

// Attempt connection to all candidates that are not already connected.
// Returns true only when every candidate is verified in the connected list.
static bool attempt_all_candidates() {
    BtdrvAddress candidates[8] = {};
    int n = get_paired_audio_devices(candidates, 8);
    log_print("[main] %d candidate(s)\n", n);
    if (n <= 0) return false;

    bool all_connected = true;
    for (int i = 0; i < n; i++) {
        char addr_s[18];
        addr_to_str(candidates[i], addr_s, sizeof(addr_s));

        if (is_audio_connected(candidates[i])) {
            log_print("[bt] %s already connected, skipping\n", addr_s);
            continue;
        }
        Result rc = connect_audio_device(candidates[i]);
        if (R_FAILED(rc)) {
            log_print("[main] Failed 0x%08X\n", rc);
            all_connected = false;
            continue;
        }
        if (wait_until_connected(candidates[i], 5000)) {
            log_print("[bt] %s verified connected\n", addr_s);
        } else {
            log_print("[bt] %s open ok but not verified yet\n", addr_s);
            all_connected = false;
        }
    }
    return all_connected;
}

// ─── Config hot reload ─────────────────────────────────────────────────────
static void reload_config() {
    // Reset to defaults before re-reading
    g_mode                 = MODE_POLLING;
    g_polling_interval_sec = 12;
    g_boot_delay_sec       = 18;
    g_max_retries          = 0;
    g_exit_when_connected  = 1;
    g_filter_set           = false;
    memset(&g_filter_addr, 0, sizeof(g_filter_addr));
    memset(g_filter_name,  0, sizeof(g_filter_name));
    load_config();
}

// ─── Polling mode ──────────────────────────────────────────────────────────
// No psc:m. No time:u. Brief btm/btdrv around each attempt only.
// Default: exit the process after a verified connect (clock-safe).
static void run_polling() {
    log_print("[main] Mode: polling (interval=%ds)\n", g_polling_interval_sec);
    log_print("[main] clock-safe: no time:u, no psc; exit_when_connected=%d\n",
              g_exit_when_connected);
    int attempt = 0;
    bool was_connected = false;

    while (true) {
        reload_config();
        // wake mode is treated as polling now (PSC path removed — it broke the clock)
        if (g_mode == MODE_WAKE)
            log_print("[main] mode=wake ignored (using polling; no PSC)\n");

        attempt++;
        log_print("[main] -- Attempt %d --\n", attempt);

        bt_open();
        bool connected = attempt_all_candidates();
        bt_close();

        if (connected && !was_connected)
            log_print("[main] Connected\n");
        else if (!connected && was_connected)
            log_print("[main] Lost connection — retrying\n");
        was_connected = connected;

        if (connected && g_exit_when_connected) {
            log_print("[main] Done - exiting process (clock-safe)\n");
            return;
        }

        if (g_max_retries > 0 && attempt >= g_max_retries) {
            log_print("[main] Reached max_retries=%d, stopping.\n", g_max_retries);
            return;
        }

        // Not verified yet: retry quickly. OpenAudioConnection may need a few
        // rounds before the headset lands in the connected list.
        int sleep_sec = connected ? 60 : g_polling_interval_sec;
        log_print("[main] Idle %ds (no services held) ...\n", sleep_sec);
        svcSleepThread((u64)sleep_sec * 1'000'000'000ULL);
    }
}

// ─── Entry point ───────────────────────────────────────────────────────────
int main(int, char**) {
    // sm/fs/sdmc already up via __appInit
    log_open();

    log_print("==============================================\n");
    log_print("  snapback v1.1-sft7  (%s %s)\n", __DATE__, __TIME__);
    log_print("==============================================\n");
    log_print("[init] hosversion=0x%08X\n", hosversionGet());

    load_config();

    log_print("[main] Boot delay %ds ...\n", g_boot_delay_sec);
    svcSleepThread((u64)g_boot_delay_sec * 1'000'000'000ULL);

    run_polling();

    log_print("[main] exit\n");
    return 0;
}