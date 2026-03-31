# SnapBack

**SnapBack** is a Nintendo Switch sysmodule that automatically reconnects your Bluetooth headphones (AirPods, Beats, and other BT audio devices) when you turn on your Switch. No more manually pairing every time.

---

## Features

- Automatically reconnects paired Bluetooth audio devices on boot
- Resumes polling after disconnect — if headphones go out of range, they reconnect when back
- Screen-aware — pauses polling when screen is off, reconnects immediately on wake
- Companion config app to select your device and configure settings
- Lightweight background sysmodule with no performance impact

---

## Requirements

- Nintendo Switch running [Atmosphere](https://github.com/Atmosphere-NX/Atmosphere) CFW
- Bluetooth audio device paired to your Switch (AirPods, Beats, etc.)

---

## Installation

1. Download the latest release
2. Copy the contents to the root of your SD card — the folder structure is ready to drag and drop
3. Reboot your Switch

The sysmodule starts automatically on boot via `boot2.flag`.

---

## Configuration

Open the **SnapBack** app from the Homebrew Menu to configure:

- **Device** — select which paired Bluetooth device to reconnect
- **Polling interval** — how often to check for reconnection (default 12s)

Settings are saved to `sdmc:/switch/snapback/config.ini` and applied immediately without rebooting.

---

## Config file

You can also edit `config.ini` manually:

```ini
# Bluetooth address of the device to reconnect
filter_address = AA:BB:CC:DD:EE:FF

# Device name (for reference only)
device_name = My AirPods

# How often to check for reconnection in seconds
polling_interval = 12

# Delay on boot before first attempt (seconds)
boot_delay = 18

# Max reconnect attempts (0 = unlimited)
max_retries = 0
```

---

## Log

The sysmodule writes a log to `sdmc:/switch/snapback/log.txt` which is useful for troubleshooting.

---

## Contributing

Contributions are welcome via pull request. By submitting a PR you agree to license your contribution under the same terms as this project.

Please do not create competing forks intended for redistribution.

---

## License

[PolyForm Noncommercial License 1.0.0](LICENSE) — free for personal use, no commercial use permitted.