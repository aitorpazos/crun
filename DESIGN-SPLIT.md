# Container Split / Fork Design

## Problem

Starting a container from a clean image costs 50-200ms even with crun
due to rootfs extraction, namespace setup, and process initialization.
For workloads needing many ephemeral containers (CI test runners,
AI agent clones), this is too slow.

## Solution

Add "crun split" and "crun fork" to create Copy-on-Write ephemeral
branches from a running parent container.

## Commands

crun split CHILD --from PARENT [flags]
crun fork  --from PARENT --count N PREFIX [flags]

### Flags per split

- --share-network : child joins parent netns
- --share-ipc     : child joins parent ipcns
- --share-uts     : child joins parent utsns
- --share-pid     : child joins parent pidns
- --share-user    : child joins parent userns
- --share-cgroup  : child joins parent cgroupns
- --criu          : checkpoint parent memory via CRIU

### Flags per fork only

- --count N       : create N children
- --ttl N         : auto-delete after N seconds

## Implementation

### Phase 1: Storage COW

Parent rootfs is lowerdir of an overlayfs mounted at child rootfs.
Writes isolated to child upperdir.

### Phase 2: Namespace Sharing

Reads parent PID from state, injects /proc/PID/ns/ paths into child
OCI spec. Kernel resolves at child creation time (no explicit setns).

### Phase 3: Memory COW via CRIU (experimental)

"criu dump" parent to child_bundle/.split-criu.
"criu restore" into child.
NOTE: tmpfs limitations; overlayfs-only path is the tested MVP.

### Phase 4: Fork Orchestration

fork loops split. Optional TTL spawns async delete watcher.

## Benchmarks (Lima VM, Ubuntu 24.04 aarch64, 4 CPU)

| Operation                  | Median |
|----------------------------|--------|
| crun run                   | 5ms    |
| crun split overlayfs       | 6-7ms  |
| crun split share-net+pid   | 6-7ms  |
| crun fork --count 10       | 28ms   |

## Deletion

crun delete CHILD
Removes upperdir, workdir, criu dir, and state directory.
No effect on parent.
