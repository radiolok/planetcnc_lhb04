# Stage 3 — PlanetCNC controller validation findings

Validation against a real PlanetCNC Mk3/4 controller
(`PlanetCNCLib64.dll` ver **20251124**, `C:\Program Files\PlanetCNC\`),
Windows 10/11, pendant clone KTURT (VID `0x10CE` / PID `0xEB70`, 6-byte
packets).

A diagnostic utility `tng_probe` (`tools/tng_probe/`) was added to probe the
TNG API read-only (`status` / `params` / `commands`) and drive test moves
(`jog` / `move`). All in-process tests require **administrator rights**
(libusbK controller access; `PlanetCNC64.exe` has `highestAvailable` manifest).

---

## 1. `Jog()` semantics — the critical finding

`bool Jog(bool step, double x, double y, double z)` behaves very differently
from what the API reference implies ("start or continue jogging").

### Step mode (`step=true`)

- The magnitude of `x/y/z` is **ignored**; the axis moves exactly by the
  `_jog_step` controller setting (default **0.1 mm**), in the direction of the
  sign.
- Verified: with `_jog_step=0.1`, both `Jog(true, 1.0)` and `Jog(true, 5.0)`
  moved 0.1 mm; after `SetParam("_jog_step", 1.0)`, `Jog(true, 1.0)` moved
  exactly 1.0 mm.
- The move is near-instantaneous (maximum acceleration), which causes the
  "huge-speed jerks" and lost steps the user observed.

### Continuous mode (`step=false`)

- `x/y/z` is a **velocity multiplier of `_jog_speed`**, not an absolute speed:
  **actual mm/s = value × `_jog_speed`**, capped at `_motion_maxspeed`.
- Measured (with `_jog_speed`=12, `_motion_maxspeed`=60):
  - `Jog(false, 0.5)` → 6 mm/s  (= 0.5 × 12)
  - `Jog(false, 5)`   → ~60 mm/s (= 5 × 12, capped)
  - `Jog(false, 60)`  → ~60 mm/s (capped)
- This is why the first velocity-servo attempts oscillated: the servo passed
  its desired mm/s directly, so the axis moved **12× faster** than intended and
  overshot the target by up to ~1.5 mm.

### Motors

- `Jog()` / `MoveAxis()` return `ok` but do **not move** while the motor enable
  signal is off. Motors must be enabled with **`M10 P1`** (`StartCode`).

---

## 2. `MoveAxis()` — precise absolute positioning

`bool MoveAxis(double speed, int axis, double val)` moves a single axis to the
**absolute** motor position `val` precisely and smoothly (clean accel/decel),
**without** the `_jog_round` rounding that `Jog()` applies at stop.

- Speed scale: **actual mm/min = value / 10** (i.e. for N mm/min pass `N*10`).
  Verified: `MoveAxis(speed=1000)` → ~100 mm/min; `speed=20` → ~2 mm/min.
- Verified it reached an exact, non-rounded target (`185.19 mm`).

---

## 3. `Run()` blocks

`int Run(bool hideUI)` / `RunProfile` run the TNG message loop **on the calling
thread and do not return until `Exit()`** (both headless and GUI). Therefore:

- `TngApi::run/runProfile/exitTng/exitTngForce` must **not hold the mutex**,
  otherwise every other API call deadlocks behind it.
- The daemon must call `Run()` on a **dedicated thread** and drive the API from
  worker threads (`main.cpp` was fixed accordingly).
- `Exit()` may not stop the in-process TNG promptly; `ExitForce()` + a bounded
  grace period is used (final graceful-shutdown semantics tracked in AC-06).

---

## 4. Parameter names (confirmed via `GetParam`)

| Parameter | Meaning | Value seen |
|---|---|---|
| `_ovrd_speedfeed` | feed override (1.0 = 100%) | 1.6 (160%) |
| `_ovrd_spindle` | spindle override | 1.0 |
| `_ovrd_speedtraverse` | traverse override | 1.0 |
| `_jog_speed` | jog speed, mm/s (also the `Jog()` velocity multiplier) | 12 |
| `_jog_speeddef` / `_jog_stepdef` / `_jog_rounddef` | defaults | 12 / 0.1 / 0.1 |
| `_jog_step` | step-mode jog distance, mm | 0.1 |
| `_jog_round` | position rounding at jog stop, mm | 0.1 |
| `_motion_maxspeed` | max speed, mm/s (caps `Jog()` continuous) | 60 |
| `_motion_maxacc` / `_motion_maxdec` | accel/decel | 200 |
| `_speed_feed` / `_speed_traverse` | feed / rapid, mm/min | 1600 / 2000 |

The previous guesses `SpeedFeedOverride` / `SpeedSpindleOverride` return **NaN**
(not a valid parameter); config updated to `_ovrd_speedfeed` / `_ovrd_spindle`.

---

## 5. Button codes — confirmed

The button code table in `src/usb/XhcProtocol.h` (18-button `xhc-hb04`
layout2) matches this pendant — confirmed manually in `mpg_gui` (buttons named
correctly). No changes needed.

---

## 6. DLL loading

`PlanetCNCLib64.dll` depends on `PlanetCNCCore64.dll` in the same directory.
`TngApi::loadLibrary` now uses `LoadLibraryExW(..., LOAD_WITH_ALTERED_SEARCH_PATH)`
so a full path works without adding the PlanetCNC directory to `PATH`.

---

## 7. Other findings

- `GetCmdId()` returns **0** for every name (including bogus ones) through the
  named-pipe (attach) interface — named-command buttons may not dispatch there.
  The `zero` button (`Machine.Work_Position.Offset.To_Zero`) also failed
  in-process; command-name resolution needs further investigation.
- `InfoWorkPosition3()` returns **NaN** until the work position is set/homed;
  `InfoMotorPosition3()` / `InfoMotorPosition(n)` read correctly.
- Soft limits are active (e.g. ~394.4 mm on X during testing) and stop jogging.
- `_jog_round` (0.1 mm) quantizes positions at jog stop; sub-0.1 mm precision
  requires lowering `_jog_round` (see AC-01).
- In-process startup requires admin rights (libusbK); the daemon/service will
  need elevation in deployment.

---

## 8. Wheel = position-following (final approach)

The pendant wheel drives a **velocity servo** in `JogController`:

1. The wheel accumulates a target coordinate (motor coords), initialized lazily
   from `InfoMotorPosition(axis)`.
2. Servo velocity = `min(gain·error, sqrt(2·decel·|error|))`, capped at
   `max_speed`, signed.
3. The velocity is divided by `jog_speed` (12) before being passed to
   `Jog(false, ...)` (see §1).
4. Hysteresis: stop when `|error| < 0.05 mm`, re-engage only above 0.15 mm.
5. `Jog()` is re-issued only when the velocity changes by >1 mm/s.

`max_speed` (mm/min) and `jog_speed` (mm/s) are YAML-configurable
(`config/mpgd.yaml`).

### Tried and rejected

- **Step mode** (`Jog(step=true)`): instant jumps → lost steps / jerks.
- **MoveAxis every tick**: re-planning mid-move → stop-start jerks.
- **MoveAxis only when idle**: precise but discrete "small steps".
- **Velocity servo without the ÷jog_speed fix**: axis moved 12× too fast →
  limit-cycle oscillation (switching axis stopped it, switching back resumed).
