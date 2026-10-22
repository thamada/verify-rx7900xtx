# Change History

> **Note**:
>   This document is the change history. Timestamps come from the `date` command, in `YYYY-MM-DD HH:MM:SS` form.
>   Newest entries are listed first.

## 2026-10-23 02:13:09 — Build and run kfd-public-vram without the contiguous flag

- `misc/kfd-public-vram` still builds when `linux/kfd_ioctl.h` has no `KFD_IOC_ALLOC_MEM_FLAGS_CONTIGUOUS`. That build uses the upstream value `1 << 23` and `make` prints a note
- Contiguous attribute sets run only when the kernel reports KFD ioctl 1.16 or newer. An older KFD skips them, prints a warning, and still runs the other sets
- On a terminal the build note, the runtime warning, and `FAIL:` lines are bold bright red (`NO_COLOR` and non-TTY stay plain)
- `doc/design.md` and `doc/bar-check.md` record the ioctl 1.16 gate. `doc/kfd-public-vram.md` is the full description

## 2026-10-22 16:42:25 — Check KFD public VRAM allocations for EINVAL

- `misc/kfd-public-vram` asks KFD for VRAM with the public flag, across the attribute sets and sizes the same allocation accepts without that flag, and exits 0 only when every counted request returns `EINVAL`
- One GTT allocation with the public flag is printed and left out of the tally. Startup prints each assigned PCI BAR from sysfs `resource`. The program builds only from `misc/`
- `doc/kfd-public-vram.md` describes the check. `doc/bar-check.md` and `doc/design.md` link to it
- On this host, 150 of 150 counted allocations returned `EINVAL`

## 2026-10-22 01:18:35 — Document the blit-shader headroom

- `doc/blit-shaders.md` explains why a fill that takes `hipMemGetInfo` to zero fails the first `hipMemcpy` with `invalid argument`, and why each round primes the copy engine and leaves 64 MiB
- `README.md` and `doc/design.md` link to it

## 2026-10-22 00:53:37 — Leave room for the blit shaders on round 1

- Each round runs `hipMemset` and a 1 MiB host-to-device copy on a scratch allocation, then frees it, before sampling free VRAM. That is what first loads the blit shaders
- The fill no longer requests the last 64 MiB that `hipMemGetInfo` reports. Taking that remainder left nothing for the shader load, and the first `hipMemcpy` of round 1 returned `invalid argument`
- `hipDeviceReset` drops the shaders, so every round loads them again and again leaves the 64 MiB

## 2026-10-22 00:07:15 — Red FAIL lines on stderr in vram-test

- Every `FAIL:` message from `vram-test` uses bold bright red on a terminal stderr (`NO_COLOR` and non-TTY stay plain)

## 2026-10-22 00:05:25 — Green CPU-visible VRAM line when Resizable BAR looks active

- When CPU-visible VRAM covers sysfs VRAM total, `vram-test` prints that startup line in bold bright green on a terminal (same `NO_COLOR` / non-TTY rules as the ReBAR warning)

## 2026-10-21 23:29:35 — Run on any HIP GPU when the RX 7900 XTX is absent

- Device selection still prefers a GPU whose name contains `7900 XTX`
- When that GPU is absent and HIP can read another device, the test uses the first such device and continues
- Setup still fails when HIP reports no device or cannot query one

## 2026-10-21 22:45:16 — Show the Resizable BAR warning in vram-view

- When `large_bar` is false, `vram-view` prints the CPU-visible VRAM warning immediately before the Summary. On a terminal the warning is red
- The Summary states the same fact: the copy checks do not use that window, so a pass does not show that software can map VRAM into the host

## 2026-10-21 19:46:16 — Report CPU-visible VRAM and the PCI BAR 0 size

- At startup `vram-test` reads sysfs `mem_info_vis_vram_total` and the size of PCI BAR 0, prints them, and warns when the CPU-visible size is below VRAM (Resizable BAR not active)
- The JSON log gains `cpu_visible_vram_bytes`, `large_bar` and `pci_bar0_bytes`. Pass and fail are unchanged: the copy test does not use the BAR window
- `doc/bar-check.md` explains the check, records the data from a 256 MiB BAR host, and gives the BIOS fix

## 2026-10-21 18:40:56 — State the layer limit at the end of the Summary

- The Summary ends by stating that a mismatch or a HIP error cannot separate a HIP fault from the kernel driver, firmware, PCIe, the copy engine, or VRAM
- The host check and the VRAM check name different copies, not the layer. Compute paths, corrected ECC, other engines, and rarer faults are outside the result

## 2026-10-21 18:25:51 — Add a Summary to the end of the report

- `vram-view` ends with a Summary in prose: the machine, the method, whether a failure was recorded, and what the log does and does not establish
- The Summary wraps on word boundaries within the report width

## 2026-10-21 18:10:14 — List every round as an O/X table

- The last block of `vram-view` is a table of host, VRAM, and round results
- `O` is pass and `X` is fail. The table wraps between columns so a round is not split

## 2026-10-21 18:03:18 — Plot bandwidth across the rounds

- `vram-view` draws host bandwidth and VRAM copy bandwidth as line charts at the end of the report, before the temperature chart
- Host rates share one scale. The VRAM copy rate uses its own scale
- The horizontal axis is the round number

## 2026-10-21 17:58:28 — Show the temperature chart at the end of the report

- `vram-view` prints the temperature line chart after the bandwidth summary and the per-round meters

## 2026-10-21 16:40:55 — Plot temperature from the start of the run to the end

- `vram-view` draws one line per sensor through the before and after sample of every round
- The horizontal axis is the round number. Dashed lines mark critical
- A log with no temperature samples omits the chart

## 2026-10-21 16:12:49 — Record GPU temperatures and show the thresholds

- Each round records edge, junction, and mem at the start and after the copies, in millidegrees Celsius, with critical and emergency limits
- `vram-view` draws a meter per sensor. `┃` is critical and the right end is emergency. A reading past a limit is marked on the meter and in the text
- Temperature does not change pass or fail

## 2026-10-21 15:52:06 — Add a terminal viewer for the JSON log

- `vram-view` draws a JSON log with allocation meters and bandwidth charts
- `make view` shows the newest log. `LOG=` selects a file. `--plain` turns color off
- `README.md` and `doc/design.md` describe the viewer

## 2026-10-21 15:38:34 — Replace the JSON log after every round

- After each round, write the rounds finished so far to a temporary file and `rename` it over the JSON log
- `result` stays `"in_progress"` until the run finishes, when it becomes `"passed"`, `"failed"`, or `"setup_failed"` and `exit_code` is added
- `make clean` removes a leftover temporary file and keeps the JSON log

## 2026-10-21 15:13:20 — Write the result log as JSON

- The result file is now `vram-test-YYYYMMDD-HHMMSS.json`
- Each round is an object. Mismatches include numeric offsets and hex expected and actual values
- `README.md` and `doc/design.md` describe the JSON object

## 2026-10-21 14:52:10 — Write a compact result log

- Each run writes `vram-test-YYYYMMDD-HHMMSS.log` with the host, GPU, one line per round, and the final result
- Progress stays on the terminal. `make clean` does not delete the log
- `README.md` and `doc/design.md` describe the file

## 2026-10-21 14:25:24 — Add a VRAM-to-VRAM copy check

- After the host copy check, each round clears the second half of the allocation and copies the first half onto it with `hipMemcpyDeviceToDevice`
- The second half is read back and compared with the first half's pattern
- `README.md` and `doc/design.md` describe the extra steps

## 2026-10-21 13:57:35 — Add a short README

- Added `README.md` with a short version of the usage and the per-round check
- Listed `README.md` in `doc/design.md`. The full procedure stays in that document

## 2026-10-21 13:54:07 — Use English for all repository text

- Rewrote user-facing messages, comments, `doc/design.md`, and this history in English
- Left the VRAM test procedure unchanged

## 2026-10-21 13:50:06 — Add the RX 7900 XTX VRAM test

- Added `vram_test.cpp`, which checks capacity, fills free VRAM, confirms it is exhausted, copies the data back, and compares every byte, for 10 rounds
- When host memory is smaller than VRAM, the test copies a pattern that can be rebuilt from the seed and the offset, one chunk at a time
- `make run` builds and runs the test. `ROUNDS` and `CHUNK_MIB` set the round count and the chunk size
- Documented the procedure, pass criteria, and exit codes in `doc/design.md`
