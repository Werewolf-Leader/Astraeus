# Boreas Hardware Dependency Map

This document maps each Boreas control to its exact backing dependency (sysfs
path(s) and/or D-Bus interface) and describes the `Unavailable_State` behavior
and the exact operator-facing message shown when that dependency is missing.

It is a standalone deliverable for **Requirement 3** (Runtime Capability
Detection and Graceful Degradation), specifically:

- **Req 3.2** — a control that fails capability detection is placed in the
  `Unavailable_State`.
- **Req 3.3** — while in the `Unavailable_State`, the control is disabled and a
  visible text reason identifies the specific capability/dependency that failed.

All strings below are the **exact** messages produced by
`HardwareManager::detectCapabilities()` and the associated read/write paths in
`HardwareManager.cpp`. Capability is exposed to QML as
`capabilities[control].supported` and `capabilities[control].reason`; the QML
binds a control's `enabled`/`visible` state to `supported`, so an unsupported
control never surfaces an enabled write action (Req 3.7).

---

## How detection works

`detectCapabilities()` runs at construction and probes every control through the
injected platform-access interfaces (`ISysfsAccess`, `IDBusAccess`,
`ISettingsStore`). Each control records a `Capability{ supported, reason }`:

- A missing sysfs path, an unwritable attribute, or an unavailable D-Bus
  service maps to `supported = false` with a human-readable `reason`.
- Nothing in the probe path throws or terminates the application (Req 3.1–3.4).

The following controls are detected: `PowerProfile`, `GpuMode`, `FanCurve`,
`TemperatureRead`, `TemperatureThreshold`, `PowerLimits`.

---

## Dependency map

| Control | Backing sysfs path(s) | D-Bus interface (primary / fallback) | Behavior when missing (exact `Unavailable_State` message) |
|---|---|---|---|
| **Power profile** | `/sys/firmware/acpi/platform_profile` (+ `/sys/firmware/acpi/platform_profile_choices`); ASUS `throttle_thermal_policy` under `/sys/devices/platform/asus-nb-wmi/` | `org.asuslinux.Daemon` (asusd) — used as fallback when the ACPI attribute is absent | Disabled. `"Power profile control not available (platform_profile missing and asusd unavailable)"` |
| **GPU mode** | — (D-Bus only) | `org.supergfxctl.Daemon` (supergfxctl) | Disabled. `"supergfxctl D-Bus service not available"` if the service is down; `"supergfxctl reported no supported GPU modes"` if the service is up but reports no usable modes. **supergfxctl is being phased out — presence is not guaranteed (see caveat).** |
| **Fan curve** | asus-wmi `pwmN_auto_pointX_temp` / `pwmX` under hwmon | `org.asuslinux.Daemon` (asusd) `FanCurves`, per-profile | Disabled. Exact string `"Fan curve control not supported by current driver"` (Req 6.11). |
| **CPU temperature** | `/sys/class/hwmon/hwmon0/temp1_input` (coretemp / k10temp), millidegrees | — | Readout shows `"—"`; `readFailed` banner. See temperature-read messages below. |
| **GPU temperature** | `/sys/class/hwmon/hwmon1/temp1_input` (amdgpu / nvidia), millidegrees | — | **May be absent.** Readout shows `"—"`; capability reason `"GPU temperature sensor unavailable"` while CPU still works. |
| **Temperature threshold** | (no guaranteed hardware target; `/sys/devices/platform/asus-nb-wmi/throttle_thermal_policy` probed to decide hardware-vs-soft) | — | **Always available** as a Boreas-side soft limit. Labeled a monitoring/warning value unless a real driver throttle target is detected (Req 7.5, 7.6). |
| **PL1 (sustained)** | `ppt_pl1_spl` under `/sys/devices/platform/asus-nb-wmi/` (newer kernels: `/sys/devices/platform/asus-armoury/ppt_pl1_spl`) | — | Disabled. See PL1/PL2 messages + caveat below. |
| **PL2 (boost)** | `ppt_pl2_sppt` under `/sys/devices/platform/asus-nb-wmi/` (newer kernels: `/sys/devices/platform/asus-armoury/ppt_pl2_sppt`) | — | Disabled. See PL1/PL2 messages + caveat below. |

---

## Per-control detail

### Power profile

- **Primary:** the ACPI `platform_profile` attribute (`ISysfsAccess::exists`).
- **Fallback:** the asusd D-Bus service (`org.asuslinux.Daemon`).
- **Supported** if either the sysfs attribute exists or asusd is available.
- **Unavailable message:**
  `"Power profile control not available (platform_profile missing and asusd unavailable)"`
- **Read-path failures** (surfaced via `readFailed`, retain last-known value):
  - `"Active power profile read failed: platform_profile attribute not available"`
  - `"Active power profile read failed: could not read platform_profile"`
  - `"Active power profile read failed: unrecognized platform_profile value \"<value>\""`

### GPU mode

- **Backing:** the supergfxctl D-Bus daemon `org.supergfxctl.Daemon`.
- **Supported** only if the service is available AND it reports a non-empty set
  of supported modes.
- **Unavailable messages:**
  - Service down: `"supergfxctl D-Bus service not available"`
  - Service up, no modes: `"supergfxctl reported no supported GPU modes"`
- **Read-path failures** (via `readFailed`):
  - `"GPU mode read failed: supergfxctl D-Bus service not available"`
  - `"GPU mode read failed: supergfxctl reported no supported GPU modes"`
  - `"GPU mode read failed: could not read active GPU mode"`

### Fan curve

- **Backing:** asusd `FanCurves` support, probed via
  `IDBusAccess::fanCurvesSupported()`.
- **Unavailable message (exact, Req 6.11):**
  `"Fan curve control not supported by current driver"`

### CPU / GPU temperature (TemperatureRead)

- **Backing:** hwmon `tempN_input` in millidegrees. CPU and GPU sensors are
  probed independently so a GPU sensor can be missing while CPU works.
- **Aggregate capability:**
  - Neither readable → `supported = false`, reason
    `"No usable hwmon temperature sensor found"`.
  - CPU only → `supported = true`, reason `"GPU temperature sensor unavailable"`.
  - GPU only → `supported = true`, reason `"CPU temperature sensor unavailable"`.
  - Both → `supported = true`, no reason.
- **Read-path failures** (per sensor, via `readFailed`; sentinel rendered as `"—"`):
  - `"<sensor> temperature sensor unavailable"`
  - `"Failed to read <sensor> temperature"`
  - `"Unparseable <sensor> temperature reading"`
- Temperature polling runs on a `QTimer` at **1500 ms** (within the required
  1000–2000 ms range).

### Temperature threshold

- **Always available** as a Boreas-side soft limit (Req 7.6); never placed in
  the `Unavailable_State`.
- `tempThresholdIsHardwareLimit` is set to `true` only if
  `/sys/devices/platform/asus-nb-wmi/throttle_thermal_policy` exists; otherwise
  the threshold is treated as a `Soft_Limit` and labeled as a Boreas-side
  monitoring/warning value (Req 7.5, 7.6).

### PL1 / PL2 (power limits)

- **Backing:** `ppt_pl1_spl` / `ppt_pl2_sppt`. The probe resolves the primary
  location under `asus-nb-wmi` first and falls back to `asus-armoury`.
- **Supported** only if both attributes exist AND both are writable.
- **Unavailable messages:**
  - Attribute(s) missing: `"PL1/PL2 not exposed by current driver"`
  - Present but not writable: `"PL1/PL2 not writable by current driver"`
- When unsupported, power-limit bounds reset to the conservative estimate
  **1..200 W** with `estimated = true` so QML never shows stale bounds. Real
  bounds are adopted only if optional sibling `_min` / `_max` attributes are
  present and parse to `min < max`.

---

## PL1/PL2 blocking-write caveat

The PL1/PL2 sysfs writes are confirmed to exist, but on some kernels (e.g.
certain 7.x kernels on Strix Halo) the write is reported to **block for ~60 s
and then return an I/O error**. Because of this:

- Every PL write is treated as **fallible**.
- Writes use a **bounded timeout** (`kPowerLimitWriteTimeoutMs = 3000 ms`) so the
  UI does not hang.
- On a `Timeout`, the exact operator-facing message is:
  `"Power limit write timed out (known driver issue on some kernels)"`
- A **read-back is still performed** after a timeout/failure so the operator
  sees the value the hardware currently holds; the UI always displays the
  read-back value, never the merely requested value (Req 8.10, 10.2).
- If a write succeeds but the read-back cannot be completed, the discrepancy is
  reported:
  `"Power limits written for <profile> but read-back failed; could not confirm the applied values."`

---

## supergfxctl phase-out caveat

GPU mode support depends entirely on the `org.supergfxctl.Daemon` D-Bus service,
which **is being phased out** and may be absent on current/future systems.
Therefore:

- GPU mode capability is **runtime-detected** and degrades cleanly to the
  `Unavailable_State` with `"supergfxctl D-Bus service not available"` when the
  daemon is missing.
- dGPU disable / VFIO switches may require a **logout or reboot** to take full
  effect (disruptive). This is treated as an expected outcome: after a
  successful write, Boreas performs a read-back and, if it cannot confirm or the
  active mode differs, reports messages such as:
  - `"GPU mode switch to <mode> accepted, but the active mode could not be read back to confirm it. A logout or reboot may be required."`
  - `"GPU mode switch to <mode> requested; driver reports active mode \"<active>\". The change may require a logout or reboot to take full effect."`

---

## Source of truth

- Implementation: `HardwareManager.cpp` — `detectCapabilities()` and the
  individual `detect*()` probes, plus the read/write paths.
- Constants: `HardwareManager.h` — `kPowerLimitWriteTimeoutMs` (3000),
  `kTempPollIntervalMs` (1500), `kMaxLogEntries` (10).
- Design: `.kiro/specs/boreas-operator-ui/design.md` — "Hardware Dependency
  Map" and "Unverified / Risk Callout".
- Requirements: `.kiro/specs/boreas-operator-ui/requirements.md` — Requirement 3.
