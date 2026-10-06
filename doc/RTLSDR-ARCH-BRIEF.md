# Brief: 2x RTL-SDR backend for blah2 on Arch Linux

For: the coding agent running on the Arch Linux machine that will host the radar.
From: the agent that started this work on the user's macOS machine (2026-10-06).

## Ground rules

- **Your machine is the source of truth.** Everything below was worked out on a
  different machine (macOS, which can't run blah2). Paths, package names, device
  enumeration, kernel modules, and Docker behaviour may differ on your machine.
- **Explore the recommendations below before you act on them.** Check each claim
  against your system (`lsusb`, `rtl_test`, `docker info`, the repo itself). When
  they disagree, trust what you observe and tell the user what was different.
- Confirm with the user before any outward-facing action, such as pushing,
  creating repos, or opening PRs.

## Goal

Run blah2 (passive radar) using two clock-chained RTL-SDRs as the capture
front end:

| Role         | RTL-SDR serial | blah2 buffer          |
|--------------|----------------|-----------------------|
| Reference    | `00000001`     | `buffer1` (channel 0) |
| Surveillance | `00000002`     | `buffer2` (channel 1) |

These serials were read from USB descriptors on the Mac. Re-check them with
`rtl_test` or `lsusb -v -d 0bda:2838 | grep iSerial`. If a dongle shows the
default `00000001` on both devices, reprogram one with `rtl_eeprom -d <idx> -s 00000002`.

The user has confirmed the two dongles share a clock (one is clocked from the
other, as in the user's EOMP project), so their sample rates do not drift relative to each other.

## Why macOS was abandoned

- blah2's [CMakeLists.txt](../CMakeLists.txt) links a Linux-only SDRplay `.so`,
  needs UHD 4.8+, and expects the krakenrf librtlsdr fork (`rtlsdr_set_dithering`).
- blah2 is deployed with `docker compose` and `privileged: true`. Docker Desktop
  on macOS cannot pass USB devices into containers.

## Repository / fork workflow

The upstream repo is `30hours/blah2`, which the user does not own. The plan is a
fork at `github.com/jack-d-long/blah2`:

1. On the Mac, run `gh repo fork --remote --remote-name origin`. Afterwards
   `origin` is the fork and `upstream` is 30hours/blah2.
2. Do the work on a branch, e.g. `rtlsdr-backend`, then push it to the fork.
3. On Arch, run `git clone -b rtlsdr-backend https://github.com/jack-d-long/blah2 /opt/blah2`.
4. To sync with upstream later: `git fetch upstream && git merge upstream/main`.

**Status at time of writing:** the fork `jack-d-long/blah2` exists, and the branch
`rtlsdr-backend` contains this brief only. The backend has **not** been
implemented yet. Before doing anything else, run `git log --oneline upstream/main..HEAD`
(or look at the branch on GitHub) to see whether more has been pushed since. If so,
review it against the design below. If not, you may be the one implementing it;
confirm with the user first.

## Host setup (recommended; verify each step)

```bash
# packages (names are Arch's; check they still exist)
sudo pacman -S --needed docker docker-compose git rtl-sdr
sudo systemctl enable --now docker
sudo usermod -aG docker $USER   # re-login afterwards

# check the dongles, one at a time, then stop each with Ctrl-C
rtl_test -d 0
rtl_test -d 1
# expect both serials listed, and "lost at least N bytes" never printed
# when running `rtl_test -s 2048000` for ~30 s per device

# blah2 expects an external docker network
docker network create blah2

cd /opt/blah2
docker compose up -d --build
# web UI: http://localhost:49152
```

Things to check rather than assume:

- **Kernel DVB driver.** `dvb_usb_rtl28xxu` must not hold the dongles. The
  Dockerfile builds librtlsdr with `-DDETACH_KERNEL_DRIVER=ON`, and Arch's
  `rtl-sdr` package may ship a modprobe blacklist. Check with `lsmod | grep dvb`.
  If it is loaded, blacklist it.
- **USB device mount.** [docker-compose.yml](../docker-compose.yml) mounts
  `/dev/usb`, but on standard Linux the libusb device nodes are at `/dev/bus/usb`.
  `privileged: true` may already expose them. If the container can't open the
  dongles, add `- /dev/bus/usb:/dev/bus/usb` to the `blah2` service volumes.
- **Host librtlsdr vs container librtlsdr.** The container builds its own
  krakenrf fork. The host `rtl-sdr` package is only for diagnostics. Make sure no
  host process (`rtl_test`, `rtl_tcp`, gqrx) holds the dongles when blah2 starts.
- **USB topology.** Two RTL-SDRs at 2+ MS/s on one hub or controller can drop
  samples. Prefer separate root ports, and check with `lsusb -t`.

## Configuration (proposed `config/config-rtlsdr.yml`)

Base it on [config/config-hackrf.yml](../config/config-hackrf.yml). The capture
section would be:

```yaml
capture:
  fs: 2048000          # RTL-SDR stable range 900001-3200000; >2.4 MS/s drops samples
  fc: 473000000        # EOMP used 473 MHz DVB-T; set to the user's illuminator
  device:
    type: "RtlSdr"
    serial: ["00000001", "00000002"]   # [reference, surveillance]
    gain: [40.2, 40.2]                 # dB; snapped to nearest valid tuner gain
    sync:
      nCorr: 262144        # samples per channel used for each alignment correlation
      maxLag: 100000       # largest start offset searched (samples)
      interval: 10         # seconds between alignment re-checks
      minPeakRatio: 10     # correlation peak / median required to trust an estimate
```

Point docker-compose at it (`command: "/blah2/bin/blah2 -c config/config-rtlsdr.yml"`)
or copy it over `config/config.yml`.

## The time-sync problem (most important part)

Sharing a clock fixes **frequency** drift only. It does not make the two sample
streams start together. Three separate failures need handling:

1. **Random start offset.** Each dongle streams over its own USB connection, so
   `rtlsdr_read_async` on two devices starts at different instants. The integer
   sample offset between the streams is random on every run and can be thousands of
   samples (milliseconds). Without correction, every delay bin is wrong by that
   offset, and the Wiener-Hopf clutter filter can't cancel the direct path.
2. **USB sample drops.** librtlsdr does not report dropped samples. A drop on one
   dongle shifts the offset permanently, and nothing in the stream shows it.
3. **blah2 buffers slipping independently.** In [src/data/IqData.cpp](../src/data/IqData.cpp),
   `push_back` pops the oldest sample when full. The existing
   [Kraken](../src/capture/kraken/Kraken.cpp) backend pushes each channel from
   its own callback. If the processor falls behind, each buffer discards samples at
   a different moment, which silently misaligns the channels. (The Kraken backend
   also opens devices by index, not serial, and does no alignment, so don't reuse it as-is.)

The user's EOMP project solves (1) with `gr-multi_rtl` (`multi_rtl_source` with
`sync_center_freq`). It cross-correlates the channels after startup and drops
samples from the leading channel. Because the clocks are shared, the offset stays
fixed afterwards. The recommended blah2 design applies the same idea and adds
handling for (2) and (3):

- **New source `src/capture/rtlsdr/RtlSdr.{h,cpp}`**, registered in
  `Capture::VALID_TYPE` / `factory_source` and added to `CMakeLists.txt`.
  - Open each device by serial (`rtlsdr_get_index_by_serial`), set the same `fc`
    and `fs` on both, use manual gain, and disable dithering. Dithering
    decorrelates the two tuners' local oscillators; Kraken disables it for this
    reason.
  - Call `rtlsdr_reset_buffer` on both, then start both `rtlsdr_read_async`
    threads as close together as possible.
  - The callbacks write into **private per-channel staging FIFOs**, not into
    blah2's `IqData`.
- **Aligner thread** between the staging FIFOs and `buffer1`/`buffer2`:
  - *Acquire:* take `nCorr` samples from each channel and FFT cross-correlate
    them with FFTW (already linked). Find the peak lag within `±maxLag`. The
    direct-path signal dominates both antennas, so the true lag is about 0 for
    co-located antennas. Drop `|lag|` samples from the leading channel. Accept
    the result only if peak/median ≥ `minPeakRatio`, and retry otherwise.
  - *Stream:* move samples out **in pairs**, always the same count to both
    `buffer1` and `buffer2` under both locks, so the buffers can't diverge. If the
    staging FIFOs get too full, drop the same number of samples from both.
  - *Monitor:* every `interval` seconds, cross-correlate a short aligned snapshot.
    If the peak is no longer at lag 0, log it and re-acquire.
  - Log the measured offset and peak ratio at every acquire and re-check, so the user can see sync health.
- **Phase:** each R820T tuner adds a random constant phase per tune. A constant
  phase offset doesn't change the delay-Doppler map magnitude, and the clutter
  filter estimates it from the data, so phase calibration is not needed.
- **Replay/IQ save:** keep `Source::open_file`/`close_file` working and write the
  *aligned* samples, so recordings replay with the correct alignment.

## Verification

1. **Splitter test (best):** feed one antenna through a 2-way splitter to both
   dongles. After alignment the cross-correlation peak must be at lag 0. Restart
   blah2 several times: the raw offset should vary between runs, but the corrected lag should always be 0.
2. **Live:** in the web UI's delay-Doppler map, the direct-path/zero-Doppler
   clutter should sit at delay 0. Once the clutter filter is on, it should be
   strongly suppressed. A strong ridge at a non-zero delay that moves between
   restarts means alignment is broken.
3. **Soak:** run for an hour and watch the logs for re-acquire events. Frequent
   re-acquires mean USB drops; fix the USB topology or lower `fs`.
4. Make sure the existing unit tests still build and pass: `testAmbiguity`, `testTracker`, `testHammingNumber`.

## References

- User's EOMP project (GNU Radio 3.8, `gr-multi_rtl` sync): on the user's Mac at
  `~/Documents/Radar/EOMP` (`EOMPV1.py`, `mutlirtl_rx_to_cfile_2chan.grc`).
  It may not be on your machine; ask the user if you need it.
- `gr-multi_rtl`: https://github.com/ptrkrysik/multi-rtl
- krakenrf librtlsdr: https://github.com/krakenrf/librtlsdr
