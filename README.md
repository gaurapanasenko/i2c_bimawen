# i2c_bimawen

Kernel module: transparent swap of DDC/CI VCP **0x10** (brightness) and **0x12** (contrast) for **HSJ Bimawen** (`product 0x0133`) on Intel **DP AUX** adapters (`AUX …/DDI …`).

## How it works

The Bimawen firmware wires DDC/CI brightness to VCP **0x12** and contrast to **0x10** (opposite of the DDC/CI spec). Userspace (`ddcutil`, KDE/libddcutil) talks to VCP **0x10** for brightness; without help, writes go to the wrong control.

**Who gets hooked.** On load (and on each new `i2c_adapter`), the module scans buses whose name contains **`AUX`**, reads EDID from `0x50`, and checks **HSJ product `0x0133`** plus **Bimawen** in the display name (unless `edid_relaxed=1`). Adapters that fail EDID during DP link training are retried for a few seconds (`retry.c`). Optional `adapter_nr` limits the scan to one bus.

**I²C interception.** For a matching adapter it saves `i2c_algorithm` and replaces only **`master_xfer`** — the path `/dev/i2c-*` and **ddcutil** use on DP AUX. Other algo entry points are untouched.

**VCP remap (`ddc.c`).** For messages to DDC/CI address **0x37**, the hook:

1. **Pre-call:** rewrites outbound VCP codes **0x10 ↔ 0x12** in the frame and fixes checksums; tracks byte-at-a-time writes in a small state machine (`i2c_bimawen_tx`) so chunked I²C still works.
2. **Real transfer:** calls the original `master_xfer`.
3. **Post-call:** munges **replies** the same way (so `getvcp 10` sees brightness), sniffs luminance for sysfs, then restores outbound buffers if the transfer failed.

From the host’s point of view, VCP 10 is brightness and 12 is contrast; the panel receives the swapped codes it expects.

**Sysfs backlight (`backlight.c`).** On hook, the module registers **`/sys/class/backlight/i2c_bimawen-N`** under the DRM connector parent. `bl_update_status` issues DDC GET/SET on luminance through the **already wrapped** adapter (remap applies). Pass-through DDC traffic updates the cached brightness unless the change came from this driver’s own read (`bl_get`).

```
  ddcutil / KDE / brightnessctl
              │
              ▼
   i2c dev (DP AUX) ──► wrapped master_xfer (hook.c)
              │              │ munge 0x10/0x12 (ddc.c)
              │              ▼
              │         original master_xfer ──► panel
              └────────► backlight sysfs (backlight.c)
```

| File | Role |
|------|------|
| `hook.c` | EDID match, install/uninstall wrapper, bus notifier |
| `ddc.c` | VCP swap, framing/parser, EDID helpers |
| `backlight.c` | `backlight_device`, DDC GET/SET for sysfs |
| `retry.c` | Delayed work when EDID or backlight probe is not ready yet |

## Build (local)

```bash
make
make check          # userspace self-test for ddc.c
make C=2            # same module build + kernel sparse (CHECK)
sudo make install
sudo modprobe i2c_bimawen
```

## Arch (DKMS)

Set `url=` in `archlinux/PKGBUILD` to your public git remote before publishing the package.

```bash
cd archlinux
./mkrelease.sh .
makepkg --geninteg && makepkg -si
```

Package autoloads **`i2c_bimawen`**. It registers **`/sys/class/backlight/i2c_bimawen-N`**.

Do **not** load out-of-tree **`ddcci` / `ddcci_backlight`** at the same time — KDE will fight two DDC stacks:

```bash
sudo modprobe -r ddcci_backlight ddcci 2>/dev/null
sudo modprobe i2c_bimawen
```

## Params

- `adapter_nr=6` — limit to one bus (optional)
- `edid_relaxed=1` — hook HSJ `0x0133` when the EDID has no **Bimawen** name text. Writing `1` rescans adapters. Writing `0` drops current hooks and scans again under the strict name match. **Risk:** any other HSJ `0x0133` panel gets the same VCP swap (brightness/contrast crossed).
- If EDID read fails at load (DP training), the module retries for ~5 s; then replug or `modprobe` again.

## Smoke test (hotplug / teardown)

```bash
sudo modprobe -r i2c_bimawen; sudo modprobe i2c_bimawen
dmesg | tail -5 | rg 'hooked|backlight'
# replug DP or: sudo modprobe -r i2c_bimawen && sleep 1 && sudo modprobe i2c_bimawen
ddcutil getvcp 10
brightnessctl -d i2c_bimawen-6 info
```

## brightnessctl

```bash
dmesg | rg 'hooked i2c|backlight i2c_bimawen'
brightnessctl -l
brightnessctl -d i2c_bimawen-6 set 40
```

## KDE slider vs brightnessctl

`brightnessctl` uses **`/sys/class/backlight/i2c_bimawen-6`** (kernel `bl_update`).  
Plasma’s per-monitor slider on **external** displays usually goes through **libddcutil** → `/dev/i2c-6` (DDC), not sysfs. After DDC SET/GET the module updates sysfs `brightness` so **brightnessctl** tracks KDE. The Plasma slider can still show an old percent until PowerDevil does its own DDC read (it caches the UI value). Disabling per-monitor **DDC/CI brightness** in Display settings makes Plasma follow sysfs when linked. Padded read buffers need correct checksum offset (`ddc_frame_len()` in `ddc.c`).

After reload, check that the backlight sysfs node sits under the connector (`…/card0-DP-1/.../i2c_bimawen-6`). If the slider is still bad:

- Plasma **6.4+**: Display settings → disable **DDC/CI brightness** for that monitor (forces the sysfs route when KWin has linked the device).
- Or `POWERDEVIL_NO_DDCUTIL=1` on `plasma-powerdevil.service` (see KDE Discuss) so PowerDevil does not run libddcutil in parallel.
- Keep **`ddcci` / `ddcci_backlight` unloaded**.

Bind brightness keys to `brightnessctl -d i2c_bimawen-6 set …` if you want the same path as the CLI.

## Check

```bash
dmesg | rg i2c_bimawen
ls /sys/class/backlight/
ddcutil setvcp 10 40
```

## Module checks (hardware)

`make check` covers **ddc.c** only. On the Bimawen machine:

- [ ] `dmesg` shows `hooked i2c-N` after load or within ~5 s (EDID retry)
- [ ] Unplug DP during those 5 s: no oops in `dmesg`
- [ ] `ddcutil getvcp 10` and `setvcp 10` change physical brightness
- [ ] `brightnessctl -d i2c_bimawen-N` read/write matches panel
- [ ] `sudo modprobe -r i2c_bimawen` while `ddcutil` idle and while a DDC read runs — no oops