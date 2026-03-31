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

static BtdrvAddress g_filter_addr    = {};
static bool         g_filter_set     = false;
static char         g_filter_name[64] = {};

// ─── Logging ───────────────────────────────────────────────────────────────
static FILE* g_log      = nullptr;
static int   g_log_lines = 0;
static const int LOG_ROTATE_LINES = 500;

static void log_open() {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/snapback", 0777);
    g_log = fopen(LOG_PATH, "w");
    g_log_lines = 0;
}

static void log_rotate() {
    if (!g_log) return;
    fclose(g_log);
    g_log = fopen(LOG_PATH, "w");
    g_log_lines = 0;
    if (g_log) {
        fprintf(g_log, "[log] rotated\n");
        fflush(g_log);
    }
}

static void log_print(const char* fmt, ...) {
    if (!g_log) return;
    if (g_log_lines >= LOG_ROTATE_LINES) log_rotate();
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
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
        }
    }
    fclose(f);
}

// ─── Bluetooth session ─────────────────────────────────────────────────────
static void bt_open() {
    Result rc = btmInitialize();
    if (R_FAILED(rc)) log_print("[bt] btmInitialize: 0x%08X\n", rc);
    rc = btdrvInitialize();
    if (R_FAILED(rc)) log_print("[bt] btdrvInitialize: 0x%08X\n", rc);
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

// Attempt connection to all candidates that are not already connected.
// Returns true if all candidates are connected (existing or newly established).
static bool attempt_all_candidates() {
    BtdrvAddress candidates[8] = {};
    int n = get_paired_audio_devices(candidates, 8);
    log_print("[main] %d candidate(s)\n", n);

    bool all_connected = true;
    for (int i = 0; i < n; i++) {
        if (is_audio_connected(candidates[i])) {
            char addr_s[18];
            addr_to_str(candidates[i], addr_s, sizeof(addr_s));
            log_print("[bt] %s already connected, skipping\n", addr_s);
            continue;
        }
        Result rc = connect_audio_device(candidates[i]);
        if (R_FAILED(rc)) {
            log_print("[main] Failed 0x%08X\n", rc);
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
    g_filter_set           = false;
    memset(&g_filter_addr, 0, sizeof(g_filter_addr));
    memset(g_filter_name,  0, sizeof(g_filter_name));
    load_config();
}

// ─── Polling mode ──────────────────────────────────────────────────────────
static void run_polling() {
    log_print("[main] Mode: polling (interval=%ds)\n", g_polling_interval_sec);
    int attempt = 0;
    bool was_connected = false;
    bool screen_on = true;

    // Register with PSC to track screen state (optional — falls back gracefully)
    PscPmModule psc = {};
    bool psc_ok = false;
    if (R_SUCCEEDED(pscmInitialize())) {
        if (R_SUCCEEDED(pscmGetPmModule(&psc, PscPmModuleId_Bluetooth, nullptr, 0, true))) {
            psc_ok = true;
            log_print("[psc] Screen-state tracking active\n");
        } else {
            pscmExit();
        }
    }

    while (true) {
        // Drain any pending PSC events to update screen_on state
        if (psc_ok) {
            while (R_SUCCEEDED(eventWait(&psc.event, 0))) {
                PscPmState state; u32 flags;
                if (R_SUCCEEDED(pscPmModuleGetRequest(&psc, &state, &flags))) {
                    if (state == PscPmState_Awake || state == PscPmState_ReadyAwaken) {
                        log_print("[psc] Screen on\n");
                        screen_on = true;
                    } else if (state == PscPmState_ReadySleep || state == PscPmState_ReadySleepCritical) {
                        log_print("[psc] Screen off -- pausing\n");
                        screen_on = false;
                    }
                    pscPmModuleAcknowledge(&psc, state);
                }
            }
        }

        // If screen is off, wait for it to come back on
        if (psc_ok && !screen_on) {
            PscPmState state; u32 flags;
            if (R_SUCCEEDED(eventWait(&psc.event, UINT64_MAX)) &&
                R_SUCCEEDED(pscPmModuleGetRequest(&psc, &state, &flags))) {
                if (state == PscPmState_Awake || state == PscPmState_ReadyAwaken) {
                    log_print("[psc] Screen on -- resuming\n");
                    screen_on = true;
                }
                pscPmModuleAcknowledge(&psc, state);
            }
            continue; // re-check state at top of loop
        }

        // Hot reload config each cycle
        reload_config();
        if (g_mode == MODE_WAKE) {
            log_print("[main] Config changed to wake mode, switching.\n");
            if (psc_ok) { pscPmModuleClose(&psc); pscmExit(); }
            return;
        }

        attempt++;
        log_print("[main] -- Attempt %d --\n", attempt);

        bt_open();
        bool connected = attempt_all_candidates();
        bt_close();

        if (connected && !was_connected)
            log_print("[main] Connected\n");
        was_connected = connected;

        if (g_max_retries > 0 && attempt >= g_max_retries) {
            log_print("[main] Reached max_retries=%d, stopping.\n", g_max_retries);
            break;
        }
        // When connected: poll infrequently to avoid IPC interference with audio
        // When disconnected: poll at the configured interval for fast reconnect
        int sleep_sec = connected ? 30 : g_polling_interval_sec;
        log_print("[main] Sleeping %ds ...\n", sleep_sec);
        svcSleepThread((u64)sleep_sec * 1'000'000'000ULL);
    }

    if (psc_ok) { pscPmModuleClose(&psc); pscmExit(); }
}

// ─── Wake mode ─────────────────────────────────────────────────────────────
static void run_wake() {
    log_print("[main] Mode: wake (trigger on screen-on)\n");

    Result rc = pscmInitialize();
    if (R_FAILED(rc)) {
        log_print("[psc] pscmInitialize failed: 0x%08X\n", rc);
        // Retry after a delay rather than tight-looping
        svcSleepThread(30'000'000'000ULL);
        return;
    }

    PscPmModule psc = {};
    rc = pscmGetPmModule(&psc, PscPmModuleId_Bluetooth, nullptr, 0, true);
    if (R_FAILED(rc)) {
        log_print("[psc] pscmGetPmModule failed: 0x%08X, retrying later\n", rc);
        pscmExit();
        svcSleepThread(30'000'000'000ULL);
        return;
    }
    log_print("[psc] PSC module registered\n");

    // Do one attempt on entry too
    log_print("[main] Initial attempt\n");
    bt_open();
    attempt_all_candidates();
    bt_close();

    while (true) {
        // Wait for a power state change event (timeout every 30s to check config)
        rc = eventWait(&psc.event, 30'000'000'000ULL);
        if (R_FAILED(rc)) {
            // Timeout or error — check if config changed, then loop
            reload_config();
            if (g_mode != MODE_WAKE) {
                log_print("[main] Config changed away from wake mode\n");
                break;
            }
            continue;
        }

        PscPmState state;
        u32 flags;
        rc = pscPmModuleGetRequest(&psc, &state, &flags);
        if (R_FAILED(rc)) {
            log_print("[psc] GetRequest failed: 0x%08X\n", rc);
            pscPmModuleAcknowledge(&psc, state);
            continue;
        }

        log_print("[psc] State change: %u\n", (u32)state);

        if (state == PscPmState_Awake || state == PscPmState_ReadyAwaken) {
            log_print("[main] Woke up -- attempting reconnect\n");
            // Brief delay to let BT stack settle after wake
            svcSleepThread(3'000'000'000ULL);
            bt_open();
            attempt_all_candidates();
            bt_close();
        }

        pscPmModuleAcknowledge(&psc, state);
    }

    pscPmModuleClose(&psc);
    pscmExit();
}

// ─── Entry point ───────────────────────────────────────────────────────────
int main(int, char**) {
    Result rc = fsInitialize();
    if (R_FAILED(rc)) return 1;
    fsdevMountSdmc();
    log_open();

    log_print("==============================================\n");
    log_print("  snapback v1.1  (%s %s)\n", __DATE__, __TIME__);
    log_print("==============================================\n");

    load_config();

    log_print("[main] Boot delay %ds ...\n", g_boot_delay_sec);
    svcSleepThread((u64)g_boot_delay_sec * 1'000'000'000ULL);

    while (true) {
        if (g_mode == MODE_WAKE)
            run_wake();
        else
            run_polling();
    }
    if (g_log) fclose(g_log);
    fsdevUnmountAll();
    fsExit();
    return 0;
}