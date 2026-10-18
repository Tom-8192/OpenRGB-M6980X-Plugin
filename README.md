# OpenRGB M6980X plugin

[Deutsch](README.de.md)

An [OpenRGB](https://gitlab.com/CalcProgrammer1/OpenRGB) **1.1** plugin for the **Gigabyte GHOST M6980X**
mouse (USB `E0FF:0001`, OEM Areson). It adds the mouse as a device with its per-profile LED colours.

The protocol was reverse engineered for the M6980X Configuration Tool (`m6980x-driver`); this plugin uses only
the LED part of it.

## What the mouse can do (and why the plugin is limited)

- Three RGB LEDs that always share **one colour**; how many of them are lit shows the DPI stage.
- **One colour per hardware profile** (3 profiles); the mouse shows the colour of the active profile.
- Every channel has **3 levels** (off, half, full), so 27 colours. OpenRGB colours are rounded to the nearest
  level (`0–63` off, `64–191` half, `192–255` full).
- The mouse keeps LED colours even without its "save" command, so every change is presumably written to its
  flash. That is why there is **no Direct mode** (effects, SDK animations): the plugin only writes when the
  rounded colour differs from what the mouse already shows.

## Behaviour

Two modes, chosen in the device view (the plugin remembers the last choice):

- **Per profile** (default): three LEDs *Profile 1–3*, each sets the colour of its hardware profile directly, also
  of inactive profiles. A colour is written once it stayed unchanged for a second (colour picker dragging).
  Switching profiles writes nothing. Quick colours set all three LEDs, so all three profiles.
- **Static**: one colour. Applying it (also with quick colours) writes it once into the **active profile**. After a
  switch with the profile button nothing is written; OpenRGB shows the colour of the new profile (checked every
  0.5 s).

- Starting OpenRGB or switching the mode changes nothing on the mouse; only applying a colour does. Exception:
  if an OpenRGB profile is active when the GUI starts (for example the service's startup profile), OpenRGB 1.1
  hands its plugin data to the plugin, which applies it like loading that profile.
- **OpenRGB profiles** save the mode and colours in the plugin section of the profile. Loading a profile applies
  them like a manual change (Static: into the active profile); only colours that differ are written. A profile
  loaded before the mouse is plugged in is applied once it connects.
- Neither mode is called "Direct"/"Custom", so effects and SDK clients do not take it over by themselves.
- Unplugging and replugging the mouse is detected; the device disappears and comes back.
- The plugin shares a lock with the M6980X Configuration Tool (`Local\m6980x_configtool` on Windows, `flock`
  on the hidraw device on Linux), so both can run at the same time.
- A status page is under *Settings → M6980X*; messages go to the OpenRGB log with the prefix `[M6980X]`.
- Other GHOST models with the same USB ID (M8000X, M8600, 6980) are rejected.

## Notes

- If other software wrote 0–255 values into the mouse (for example an older OpenRGB fork), the mouse keeps and
  reports them. The plugin shows such a colour as it is and replaces it with levels the next time a colour is
  applied.
- When the mouse is unplugged or the plugin is disabled, OpenRGB (1.0 and 1.1) logs
  `Device thread still active in base class destructor`. This comes from OpenRGB's `RGBController_Virtual`, whose
  destructor does not call `Shutdown()`; the base class stops the thread itself, so it is harmless.

## Tested

Windows 10, official OpenRGB 1.0 (Windows 64-bit, Qt 6.8.3), on the real mouse (2026-10-16): detection,
passive start, colour changes (only the active profile is written, nothing is written when the rounded colour is
unchanged), profile switches with the profile button, unplugging/replugging, disabling the plugin, running at the
same time as `m6980x_configtool`; both modes including quick colours, with the OpenRGB GUI running as client of the
OpenRGB Windows service (plugin in the GUI). Linux: build only, not yet tried on the mouse.

## Installation

The plugin must match the OpenRGB build: same Qt major version and compiler. The Windows artefact is built like
the official OpenRGB 1.1 Windows release (Qt 6.8.3, MSVC 2022, x64). OpenRGB 1.0 has the same plugin API (5);
plugin 1.1.0 is the last release built against 1.0.

- **Windows:** copy `OpenRGB-M6980X.dll` to `%APPDATA%\OpenRGB\plugins\` and restart OpenRGB.
- **Linux:** copy `libOpenRGB-M6980X.so` to `~/.config/OpenRGB/plugins/`. Build it against the same Qt as your
  OpenRGB package (an AppImage bundles its own Qt). For access to the mouse install the udev rule:
  ```
  sudo cp linux/60-openrgb-m6980x.rules /etc/udev/rules.d/
  sudo udevadm control --reload && sudo udevadm trigger
  ```

## Building

```
git clone --recurse-submodules <this repository>
```

The submodule `OpenRGB/` is OpenRGB at `release_1.1`; only its headers are used.

- **Windows:** Visual Studio 2022 (C++ build tools) and Qt 6.8.3 `msvc2022_64`, then
  `.\scripts\build-windows.ps1 [-QtDir C:\Qt\6.8.3\msvc2022_64]`. The script runs the protocol test and puts the
  DLL into `dist\`.
- **Linux:** `sudo apt install qt6-base-dev qmake6`, then
  ```
  mkdir build && cd build && qmake6 ../OpenRGB-M6980X.pro && make
  ```
  Test: `g++ -std=c++17 -Isrc tests/protocol_test.cpp src/M6980XProtocol.cpp -o protocol_test && ./protocol_test`

## Layout

| Path | Content |
|---|---|
| `src/HidTransport_*.cpp` | feature reports without report ID on interface 0: Windows HID class driver (SetupAPI, `HidD_*`), Linux hidraw ioctls; cross-process lock |
| `src/M6980XProtocol.*` | busy/done handshake, identify, active profile (`01 08`), LED colour (`01 06` / `01 86`) |
| `src/M6980XPlugin.*` | OpenRGB plugin: worker thread, virtual RGB controller (plugin API 5), status page |
| `tests/protocol_test.cpp` | tests against a simulated mouse |

## License

GPL-2.0-or-later, like OpenRGB.
