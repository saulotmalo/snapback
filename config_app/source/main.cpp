/**
 * snapback — Config App (NRO)
 *
 * Launch from the Homebrew Menu to:
 *   1. Scan for paired Bluetooth audio devices
 *   2. Select your headphones
 *   3. Choose reconnect mode (polling / wake on screen-on)
 *   4. If polling: choose interval
 *   5. Write /switch/snapback/config.ini automatically
 *
 * Controls:
 *   D-Pad Up/Down  — move cursor
 *   A              — select / confirm
 *   B              — back / cancel
 *   +              — quit
 */

#include <switch.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>

#define CONFIG_PATH "sdmc:/switch/snapback/config.ini"

// ─── Simple console UI ──────────────────────────────────────────────────────

static void clear() { consoleClear(); }

static void header() {
    printf("\n  SnapBack - Config\n");
    printf("  -----------------------------------\n\n");
}

static void footer() {
    printf("\n  \x1b[90m+ : Quit   B : Back\x1b[0m\n");
}

// ─── Input helper ───────────────────────────────────────────────────────────

static u64 read_buttons_down(PadState* pad) {
    padUpdate(pad);
    return padGetButtonsDown(pad);
}

// ─── Bluetooth helpers ──────────────────────────────────────────────────────

struct AudioDevice {
    BtdrvAddress addr;
    char         name[0xF9];
};

static Result g_scan_rc = 0;  // last result from scan, for diagnostics

static int scan_paired_audio(AudioDevice* out, int max_out) {
    int count = 0;

    const s32 MAX_DEV = 8;
    BtmDeviceInfoV13 devs[MAX_DEV] = {};
    s32 total = 0;
    Result rc = btmGetDeviceInfo(BtmProfile_Audio, devs, MAX_DEV, &total);
    g_scan_rc = rc;
    if (R_SUCCEEDED(rc)) {
        for (s32 i = 0; i < total && i < MAX_DEV && count < max_out; i++) {
            out[count].addr = devs[i].addr;
            strncpy(out[count].name, devs[i].name, sizeof(out[count].name)-1);
            count++;
        }
        return count;
    }

    BtmDeviceInfoList list = {};
    rc = btmLegacyGetDeviceInfo(&list);
    g_scan_rc = rc;
    if (R_SUCCEEDED(rc)) {
        for (u32 i = 0; i < list.device_count && count < max_out; i++) {
            out[count].addr = list.devices[i].addr;
            snprintf(out[count].name, sizeof(out[count].name), "Device %u (legacy)", i);
            count++;
        }
    }
    return count;
}

static void addr_to_str(const BtdrvAddress& a, char* buf, size_t sz) {
    snprintf(buf, sz, "%02X:%02X:%02X:%02X:%02X:%02X",
             a.address[0], a.address[1], a.address[2],
             a.address[3], a.address[4], a.address[5]);
}

// ─── Config writer ──────────────────────────────────────────────────────────

struct Config {
    AudioDevice dev;
    bool        wake_mode;       // false = polling, true = wake
    int         polling_interval;
};

static bool write_config(const Config& cfg) {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/snapback", 0777);

    FILE* f = fopen(CONFIG_PATH, "w");
    if (!f) return false;

    char addr_s[18];
    addr_to_str(cfg.dev.addr, addr_s, sizeof(addr_s));

    fprintf(f, "# snapback - auto-generated\n");
    fprintf(f, "address          = %s\n", addr_s);
    fprintf(f, "name             = %s\n", cfg.dev.name);
    fprintf(f, "mode             = %s\n", cfg.wake_mode ? "wake" : "polling");
    fprintf(f, "polling_interval = %d\n", cfg.polling_interval);
    fprintf(f, "boot_delay       = 18\n");
    fprintf(f, "max_retries      = 0\n");
    fclose(f);
    return true;
}

// ─── Screens ────────────────────────────────────────────────────────────────

static void screen_no_devices(PadState* pad) {
    clear(); header();
    printf("  \x1b[33mNo paired Bluetooth audio devices found.\x1b[0m\n\n");
    printf("  btmGetDeviceInfo rc: 0x%08X\n\n", g_scan_rc);
    printf("  1. Go to System Settings\n");
    printf("  2. Bluetooth Audio > Pair Device\n");
    printf("  3. Put your AirPods / Beats in pairing mode\n");
    printf("  4. Pair them, then come back here.\n\n");
    printf("  Press any button to exit.\n");
    footer();
    while (appletMainLoop()) {
        if (read_buttons_down(pad)) break;
        consoleUpdate(nullptr);
    }
}

static void screen_success(PadState* pad, const Config& cfg) {
    char addr_s[18];
    addr_to_str(cfg.dev.addr, addr_s, sizeof(addr_s));

    clear(); header();
    printf("  \x1b[32mConfig saved!\x1b[0m\n\n");
    printf("  Device : %s\n", cfg.dev.name[0] ? cfg.dev.name : "(unknown)");
    printf("  Address: %s\n", addr_s);
    printf("  Mode   : %s\n", cfg.wake_mode ? "wake (screen-on trigger)" : "polling");
    if (!cfg.wake_mode)
        printf("  Interval: %ds\n", cfg.polling_interval);
    printf("\n  Reboot to apply.\n\n");
    printf("  Press any button to exit.\n");
    footer();
    while (appletMainLoop()) {
        if (read_buttons_down(pad)) break;
        consoleUpdate(nullptr);
    }
}

static void screen_error(PadState* pad, const char* msg) {
    clear(); header();
    printf("  \x1b[31mError: %s\x1b[0m\n\n", msg);
    printf("  Press any button to exit.\n");
    footer();
    while (appletMainLoop()) {
        if (read_buttons_down(pad)) break;
        consoleUpdate(nullptr);
    }
}

// Returns selected index or -1 if user quit/back
static int screen_list(PadState* pad, const char* title,
                        const char** items, int n) {
    int cursor = 0;
    while (appletMainLoop()) {
        u64 kDown = read_buttons_down(pad);
        if (kDown & HidNpadButton_Plus)  return -1;
        if (kDown & HidNpadButton_B)     return -1;
        if (kDown & HidNpadButton_Up)    { if (cursor > 0)   cursor--; }
        if (kDown & HidNpadButton_Down)  { if (cursor < n-1) cursor++; }
        if (kDown & HidNpadButton_A)     return cursor;

        clear(); header();
        printf("  %s\n\n", title);
        for (int i = 0; i < n; i++) {
            const char* prefix = (i == cursor) ? " \x1b[32m>\x1b[0m " : "   ";
            printf("%s%s\n", prefix, items[i]);
        }
        printf("\n  \x1b[90mD-Pad: navigate   A: confirm\x1b[0m\n");
        footer();
        consoleUpdate(nullptr);
    }
    return -1;
}

// Device selection — shows name + address
static int screen_select_device(PadState* pad, AudioDevice* devs, int n) {
    int cursor = 0;
    while (appletMainLoop()) {
        u64 kDown = read_buttons_down(pad);
        if (kDown & HidNpadButton_Plus)  return -1;
        if (kDown & HidNpadButton_B)     return -1;
        if (kDown & HidNpadButton_Up)    { if (cursor > 0)   cursor--; }
        if (kDown & HidNpadButton_Down)  { if (cursor < n-1) cursor++; }
        if (kDown & HidNpadButton_A)     return cursor;

        clear(); header();
        printf("  Select device to auto-reconnect:\n\n");
        for (int i = 0; i < n; i++) {
            char addr_s[18];
            addr_to_str(devs[i].addr, addr_s, sizeof(addr_s));
            const char* prefix = (i == cursor) ? " \x1b[32m>\x1b[0m " : "   ";
            printf("%s%-36s  %s\n", prefix,
                   devs[i].name[0] ? devs[i].name : "(unnamed device)", addr_s);
        }
        printf("\n  \x1b[90mD-Pad: navigate   A: confirm\x1b[0m\n");
        footer();
        consoleUpdate(nullptr);
    }
    return -1;
}

// ─── Entry point ────────────────────────────────────────────────────────────

int main(int, char**) {
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    Result rc;
    fsInitialize();
    fsdevMountSdmc();

    clear(); header();
    printf("  Initialising Bluetooth...\n");
    consoleUpdate(nullptr);

    bool btm_ok = false, btdrv_ok = false;
    rc = btmInitialize();
    if (R_SUCCEEDED(rc)) btm_ok = true;
    rc = btdrvInitialize();
    if (R_SUCCEEDED(rc)) btdrv_ok = true;

    if (!btm_ok && !btdrv_ok) {
        screen_error(&pad, "Could not open btm or btdrv.\n  Is Atmosphere running?");
        goto cleanup;
    }

    {
        AudioDevice devs[8] = {};
        int n = 0;

        // Retry scan up to 5 times — sysmodule may be briefly holding btm
        for (int try_ = 0; try_ < 5 && n == 0; try_++) {
            if (try_ > 0) {
                clear(); header();
                printf("  Scanning... (attempt %d/5)\n", try_ + 1);
                consoleUpdate(nullptr);
                // Release sessions, wait for sysmodule to finish its cycle, retry
                if (btdrv_ok) { btdrvExit(); btdrv_ok = false; }
                if (btm_ok)   { btmExit();   btm_ok   = false; }
                svcSleepThread(3'000'000'000ULL);
                rc = btmInitialize();
                if (R_SUCCEEDED(rc)) btm_ok = true;
                rc = btdrvInitialize();
                if (R_SUCCEEDED(rc)) btdrv_ok = true;
            }
            n = scan_paired_audio(devs, 8);
        }

        if (n == 0) {
            screen_no_devices(&pad);
            goto cleanup;
        }

        // Step 1: select device
        int dev_idx = screen_select_device(&pad, devs, n);
        if (dev_idx < 0) goto cleanup;

        // Step 2: select mode
        {
            Config cfg = {};
            cfg.dev          = devs[dev_idx];
            cfg.wake_mode    = false;
            cfg.polling_interval = 12; // default

            // Step 3: polling interval (only if polling mode)
            if (!cfg.wake_mode) {
                const char* intervals[] = {
                    " 5 seconds",
                    "10 seconds",
                    "15 seconds",
                    "30 seconds",
                    "60 seconds"
                };
                static const int interval_values[] = { 5, 10, 15, 30, 60 };
                int iv_idx = screen_list(&pad, "Select polling interval:", intervals, 5);
                if (iv_idx < 0) goto cleanup;
                cfg.polling_interval = interval_values[iv_idx];
            }

            // Save
            if (write_config(cfg))
                screen_success(&pad, cfg);
            else
                screen_error(&pad, "Could not write config.ini");
        }
    }

cleanup:
    if (btdrv_ok) btdrvExit();
    if (btm_ok)   btmExit();
    consoleExit(nullptr);
    fsdevUnmountAll();
    fsExit();
    return 0;
}