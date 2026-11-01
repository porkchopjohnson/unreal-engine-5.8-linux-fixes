# Physics Asset Editor lag: Vulkan deletion validation

Patch: `patches/0006-hash-vulkan-deferred-deletion-validation.patch`.

Measured 2026-10-31 in a local UE 5.8.3 Development editor on Linux/XWayland, RTX 3090. The reproduction involved 11–30 selected constraints with their limits visible. Hiding their visuals restored responsiveness while retaining the selection; moving the cursor out of the viewport alone did not.

## What was expensive

Constraint drawing generates dynamic meshes and retires many Vulkan views/descriptors. The original Debug/Development `FDeferredDeletionQueue2::EnqueueGenericResource` scans the entire pending `Entries` array under its lock to detect duplicate `(type, handle)` keys for each enqueue. Individual debugger snapshots found 21,692 pending entries with the visuals enabled and 10,016 hidden. Those queue counts were snapshots, not steady-state averages.

Three foreground stack samples were directional evidence, not a timing profile. One caught the RHI thread in this duplicate scan; another included descriptor-management work. The controlled comparison below subsequently established that restoring the scan reintroduced a substantial slowdown in this reproduction.

## Implementation and preserved invariants

- Per-queue hash maps use **both resource type and full 64-bit handle** as the key.
- The exact generic duplicate-delete assertion and message are retained.
- Counts track repeated non-generic entries with zero handles. This preserves the original scan's domain, including those entries, rather than indexing only generic handles.
- Existing queue CS is acquired before the short registry lock. No registry lock is held through Vulkan deletion or callbacks. Queue identities keep different devices separate; destruction checks emptiness and unregisters the queue.
- All four enqueue paths update tracking. Safe-frame swap removal decrements/removes the key; immediate clear and whole-array transfer reset the index under the queue CS.
- Tracking stops when an entry leaves `Entries`, matching the old scan. It does not extend protection to the separately transferred end-of-pipe array.
- Frame delay, eligibility, ownership rules, end-of-pipe submission and actual deletion calls are unchanged.

The index is `.cpp`-local. `VulkanDevice.h` and private device/queue layouts remain unchanged because external consumers such as TextureShare include Vulkan private headers and were reused precompiled.

## Synthetic benchmark

In a Debug/Development editor console:

```text
Vulkan.DeletionQueue.Benchmark
```

The command creates synthetic entries, never Vulkan resources. It uses the actual 64-byte `FEntry`, UE arrays/maps and locking, and performs 10,000 enqueue/remove pairs at a fixed queue population. Five trials per size/mode; setup is outside the timed portion. Indexed timing includes the registry lock and index removal.

| Pending entries | Median linear | Median indexed | Ratio |
| ---: | ---: | ---: | ---: |
| 10,016 | 79.385 ms | 0.617 ms | 128.7x |
| 21,692 | 214.758 ms | 0.616 ms | 348.6x |

The diagnostic checks same-handle/different-type keys, repeated keys, decrement/removal/re-enqueue, repeated non-generic zero handles, reset/reuse and reverse swap removal. This is a lookup benchmark, not a frame-rate speedup claim or an exhaustive resource-lifetime test.

## Controlled editor comparison

The user restored the same multi-constraint selection after restart, enabled its visuals and confirmed foreground readiness. Selection, camera and display settings were then held fixed. Four 30-second phases used an **A/B/B/A** order; the first 10 seconds of each were discarded. A Slate post-tick callback recorded `time.perf_counter()` intervals.

```text
r.Vulkan.DeferredDeletionQueue.LinearValidation 1
```

This maintains the new index **and** runs the original scan, asserting agreement. Setting it to `0` is normal hash-only operation and is the default.

| Phase | Linear cross-check | Samples | Mean interval | Median | P95 |
| --- | ---: | ---: | ---: | ---: | ---: |
| A1 | 1 | 150 | 133.500 ms | 139.629 ms | 153.140 ms |
| B1 | 0 | 600 | 33.334 ms | 33.318 ms | 34.733 ms |
| B2 | 0 | 600 | 33.334 ms | 33.340 ms | 34.784 ms |
| A2 | 1 | 147 | 136.454 ms | 141.941 ms | 151.952 ms |

Combined means: **134.962 ms vs 33.334 ms**, approximately **4.05x shorter editor tick intervals**. Re-enabling the scan reproduced the slowdown. No index-disagreement or duplicate-delete assertion occurred during the run.

### Repeat the comparison

Open the intended physics asset, select the constraints, enable their visuals, then execute this in the editor console using the script's absolute path:

```text
py /path/to/unreal-engine-5.8-linux-fixes/tools/phat_frame_ab.py
```

Leave the editor foregrounded and unchanged for two minutes. The script writes intervals to the system temporary directory as `ue583-phat-frame-ab.json` and restores hash-only validation when it completes. If interrupted, restore it manually with `r.Vulkan.DeferredDeletionQueue.LinearValidation 0`.

### Interpretation limits

- Scan-on includes hash maintenance, so this is not an exact original-binary baseline.
- Hash-only plateaued at 30 Hz; the limiter was not established. `t.MaxFPS=0`, `r.VSync=0`, `t.IdleWhenNotForeground=0`, `Slate.bAllowThrottling=1`, `Slate.TargetFrameRateForResponsiveness=35`, deletion delay=2 were queried. Application focus was not independently logged every frame.
- Intervals measure editor wall-clock ticks, not GPU frame timings. The callback overhead is present in both modes.
- Descriptor updates, dynamic-mesh churn and picking synchronization can still contribute to other PhAT workloads. The scan is a measured substantial cost here, not an exclusive explanation for every performance problem.

## Build

Use the [targeted precompiled-module workflow](BUILDING.md#reusing-a-precompiled-engine-for-a-targeted-module-fix), substituting `-Module=VulkanRHI`. Only that module was rebuilt: five unity compiles plus one link in the initial audited plan. After a compile correction, the successful retry was one compile and one link, taking 19.69 seconds. The temporary module-rule override was removed afterward. No complete editor rebuild or metadata regeneration was used.
