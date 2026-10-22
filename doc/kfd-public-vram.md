# KFD public VRAM

`misc/kfd-public-vram` asks the kernel for VRAM that the CPU can load and store through PCI BAR 0, and reports whether every such request returns `EINVAL`.

The copy test in `vram-test` does not make these requests. `hipMemcpy` does not need the BAR window. This program is the check for the allocation that does. See `bar-check.md` for the window itself and the BIOS settings that resize it.

## What it asks for

KFD is the AMD Kernel Fusion Driver. Its device file is `/dev/kfd`. The call is `AMDKFD_IOC_ALLOC_MEMORY_OF_GPU`.

Host access in this program is the flag `KFD_IOC_ALLOC_MEM_FLAGS_PUBLIC` on an allocation whose type is `KFD_IOC_ALLOC_MEM_FLAGS_VRAM`. Runtimes set that flag when a CPU load or store should land in VRAM through the PCI BAR.

GTT (`KFD_IOC_ALLOC_MEM_FLAGS_GTT`) is host RAM placed in the GPU address space. The program prints one page of GTT with `PUBLIC` and `WRITABLE` as a contrast. That line is outside the tally. GTT does not use BAR 0.

The SVM ioctl is not used. A disabled SVM interface returns `EINVAL` for every attribute, which does not describe this allocation.

HIP is queried in a child process. The parent then opens `/dev/kfd` itself. Opening it in the same process after HIP has acquired the GPU VM returns `EBUSY`.

Device selection prefers a GPU whose name contains `7900 XTX`. When that GPU is absent and HIP can name another device, the program uses that device. When the HIP query fails, it uses the first KFD node under `/sys/class/kfd/kfd/topology/nodes`.

## What is counted

A host-access line counts only when the same size and the same attribute flags succeed without `PUBLIC`, and that control allocation is freed. An `EINVAL` on a counted line is then a rejection of the public flag, not of the size or of the other attributes. A control allocation that does not succeed is printed as `not counted`.

The attribute sets are the combinations a VRAM allocation accepts without `PUBLIC`. `read-only` omits `WRITABLE`.

| Name | Flags added to VRAM, before PUBLIC |
|------|-------------------------------------|
| writable | `WRITABLE` |
| read-only | none |
| coherent | `WRITABLE`, `COHERENT` |
| uncached | `WRITABLE`, `UNCACHED` |
| ext-coherent | `WRITABLE`, `EXT_COHERENT` |
| contiguous | `WRITABLE`, `CONTIGUOUS` |
| executable | `WRITABLE`, `EXECUTABLE` |
| no-substitute | `WRITABLE`, `NO_SUBSTITUTE` |
| aql-queue | `WRITABLE`, `AQL_QUEUE_MEM` |
| coherent+contiguous | `WRITABLE`, `COHERENT`, `CONTIGUOUS` |
| coherent+uncached | `WRITABLE`, `COHERENT`, `UNCACHED` |
| coherent+ext-coherent | `WRITABLE`, `COHERENT`, `EXT_COHERENT` |
| executable+coherent | `WRITABLE`, `EXECUTABLE`, `COHERENT` |
| no-substitute+contiguous | `WRITABLE`, `NO_SUBSTITUTE`, `CONTIGUOUS` |
| aql+coherent | `WRITABLE`, `AQL_QUEUE_MEM`, `COHERENT` |

Each set is tried at these sizes, when the size is a multiple of the host page and is not larger than sysfs `mem_info_vram_total`:

- the host page size
- 64 KiB, 2 MiB, 16 MiB, 64 MiB, 128 MiB, 256 MiB, 512 MiB, 1 GiB
- the CPU-visible size from `mem_info_vis_vram_total`, when that file is readable
- one page past that visible size, when the sum still fits in VRAM total

A size already in the list is not repeated. On a 4 KiB page and a 256 MiB visible window, the visible size matches the 256 MiB seed, and the extra page makes ten sizes. Ten sizes times the fifteen sets is 150 counted lines when every control allocation succeeds.

When a public allocation succeeds, the program maps it through the render node at the ioctl's mmap offset, unmaps it, and frees the allocation. A failure to free any allocation stops the run with exit code 2.

## PCI BAR listing

Before the allocations, the program prints each assigned entry from the DRM device's sysfs `resource` file: index, type, CPU address range, and size. A 64-bit BAR occupies the next index as well. That empty slot is omitted. An expansion ROM is the seventh line and is labeled `ROM`. The listing does not change the verdict. BAR 0 is the VRAM window.

## Build and run

The source, the Makefile, and the binary stay in `misc/`. The top-level Makefile does not build this program.

```bash
cd misc
make            # build and run (default goal)
make build      # build misc/kfd-public-vram
make clean      # remove that binary
```

| Variable | Default | Description |
|----------|---------|-------------|
| `ROCM` | `/opt/rocm` | ROCm installation prefix |
| `CXX` | `g++` | Host compiler |

The program links `libamdhip64` for the device query. The allocations go through `/dev/kfd` and the matching `/dev/dri/renderD*` node.

## Exit codes

| Code | Verdict | Meaning |
|------|---------|---------|
| 0 | `all-einval` | Every counted host-access VRAM allocation returned `EINVAL` |
| 1 | `not-all-einval` | A counted allocation succeeded, or returned some other errno |
| 2 | `setup-failed` | The check could not be run, or no line could be counted |

Exit 0 means the public VRAM requests were rejected. It does not grade the copy path. Exit 1 means at least one counted request was accepted or failed for another reason. The summary prints the counts of `EINVAL`, other errno, allocations, and lines that were not counted.

## Result on this host (2026-10-22)

RX 7900 XTX, PCI `0000:03:00.0`, KFD `gpu_id` 7840, kernel `7.0.0-38-generic`. CPU-visible VRAM is 256 MiB. All 150 counted allocations returned `EINVAL`. The same sizes without `PUBLIC` succeeded. The GTT contrast with `PUBLIC` succeeded. The verdict was `all-einval`, exit code 0.

```text
VRAM: 24560 MiB total, 256 MiB CPU-visible
PCI BAR:
  BAR 0   memory 64-bit prefetchable    0xa0000000-0xafffffff     256 MiB
  BAR 2   memory 64-bit prefetchable    0xb0000000-0xb01fffff     2 MiB
  BAR 4   I/O                           0x4000-0x40ff             256 bytes
  BAR 5   memory 32-bit                 0x90200000-0x902fffff     1 MiB
  ROM     shadowed                      0xc0000-0xdffff           128 KiB
```

BAR 0 matches `mem_info_vis_vram_total`. The other lines are the remaining assigned resources; the program names them from the `resource` flags only.
