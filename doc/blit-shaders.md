# Blit shaders and the round-1 copy failure

Filling every byte that `hipMemGetInfo` reports free leaves no VRAM for HIP to load its blit shaders. The first `hipMemcpy` on that cold context then returns `invalid argument`, even though the pointer and the size are valid.

## What the shaders are

`hipMemcpy` and `hipMemset` are host API calls. They do not always move bytes with DMA alone. On this ROCm stack, creating the null stream builds a `KernelBlitManager`. That manager calls `Device::createBlitProgram` and places small GPU programs, the blit shaders, in VRAM. Later copies and memsets dispatch those programs.

The load is lazy. A context that has never copied anything does not hold the shaders yet. `hipMemset` is a kernel dispatch: a small `hipMemcpy` can take a host-mapped shortcut and skip the manager, but `hipMemset` cannot. The shaders stay until `hipDeviceReset` destroys the manager.

## How the failure happens

Each round of this test allocates free VRAM and then copies into it. The order on a cold context is:

1. `hipMemGetInfo` reports nearly all VRAM free, because the shaders are not loaded.
2. The fill used to request that entire remainder. The last `hipMalloc` took `hipMemGetInfo` down to about 0.
3. The first real `hipMemcpy` created the null stream and tried to load the blit shaders.
4. The load failed inside HSA with `HSA_STATUS_ERROR_OUT_OF_RESOURCES`.
5. ROCClr reported `Couldn't create blit kernels!`, then `Could not create BlitManager!`, then `Cannot create new Stream object`.
6. `hipMemcpy` returned `hipErrorInvalidValue` (`invalid argument`).

The arguments are not the cause. An 8-byte copy of a valid pointer fails the same way. `hipMemset` on that full device reports out of memory instead: there is nowhere to put the program.

The failure shows up on round 1. The failed load leaves a few mebibytes reserved across the round's `hipDeviceReset`, so later rounds see a slightly smaller free size and can pass. `hipDeviceReset` still drops a successfully loaded blit manager, so every round starts cold again. A fill that reaches zero fails the first copy of that round, not only the first round of the process.

## What the test does

Two changes in `vram_test.cpp`. Priming is not a reliable hold on its own. The 64 MiB floor is what lets the first real copy finish the load when the scratch copy did not keep the shaders.

### Prime the copy engine before sampling free memory

`prime_copy_engine` runs at the start of every round, before `hipMemGetInfo` and before the fill:

1. `hipMalloc` 1 MiB.
2. `hipMemset` that allocation. This is the call that builds the blit manager.
3. `hipHostMalloc` 1 MiB, then `hipMemcpy` the whole buffer host-to-device. The size is large enough to miss a tiny-copy fast path. A 4 KiB prefix is not used.
4. `hipDeviceSynchronize`, so a deferred shader load cannot still be allocating when the scratch is freed.
5. Free the scratch. The destructor frees it on failure as well.

When the runtime keeps the shaders, freeing the scratch does not unload them. `hipMemGetInfo` then drops by the reservation (about 140 MiB on the stack where this was measured) even though the 1 MiB scratch is gone. The free-memory sample already excludes that reservation, so the exhaustion check does not charge it as unaccounted memory.

That hold is not reliable. A cold context has returned success from this scratch copy and then reported the original free size. The fill would again see the full device and take it down to zero.

### Leave 64 MiB unasked

`kTargetLeftover` is 64 MiB. `allocate_vram` stops when `hipMemGetInfo` reports that much free or less. Each request is the aligned free size minus 64 MiB, and at most 1 GiB. The last `hipMalloc` therefore cannot consume the remainder that the stop check was meant to keep.

If the scratch copy did not keep the shaders, the first real `hipMemcpy` of the fill still has 64 MiB in which to finish the load. 64 MiB is inside `kMaxLeftover` (256 MiB), so a round that stops there still counts as full: HIP free, unaccounted memory, and sysfs free must each be at most 256 MiB.

`RoundCleanup` calls `hipDeviceReset` after the round. Reset releases the runtime cache and destroys the blit manager, so the next round primes again and again leaves the 64 MiB.

## What a pass means here

The 64 MiB is deliberately left free. It is not a hole in the byte check. The host compare and the device-to-device compare still cover every byte that was allocated. A round that fails with `invalid argument` on the first copy, before any mismatch line, is this shader load running out of VRAM. It is not evidence that the pointer or the byte count was wrong, and it does not by itself name the layer under HIP. See the limitations in `README.md`.
