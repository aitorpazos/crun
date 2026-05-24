# Kata Containers Split / Fork Design

## Problem

Kata runs containers inside VMs (QEMU/Cloud Hypervisor/Firecracker).
VM boot + OS + agent takes 50-300ms.

## Kata-Specific Challenges

- No host clone/setns access.
- Rootfs is virtiofs or block device inside guest.
- Snapshots are hypervisor-specific.
- Namespace sharing is VM-level, not host-level.

## Proposed Commands

kata-runtime split CHILD --from PARENT [flags]
kata-runtime fork  --from PARENT --count N PREFIX [flags]

Flags mirror crun but semantics differ because of VMs.

## Implementation

### Phase 1: Storage COW via Host Overlayfs

Mount overlayfs over parent_bundle/rootfs before CreateSandbox.
Transparent to hypervisor; guest sees normal read-write rootfs.

### Phase 2: VM Snapshot (hypervisor-specific)

- QEMU: savevm / loadvm
- Cloud Hypervisor: HTTP snapshot API
- Firecracker: CreateSnapshot / LoadSnapshot

Use PauseVM / SaveVM on parent, LoadVM into child.
Both resume independently.
Device IDs must not collide (Kata id seeding handles this).

### Phase 3: Differential Memory COW

If hypervisor supports diff snapshots, parent pages become read-only,
child uses COW on first write.

### Phase 4: Namespace Sharing (VM-level)

- --share-network: reuse same TAP/veth or network segment
- --share-pid: not applicable cross-VM
- Other namespaces: VM-guest isolated by design

## Benchmark Targets

| Approach                          | Expected Latency |
|-----------------------------------|------------------|
| Overlayfs + fresh VM boot         | ~80-120ms        |
| Overlayfs + VM template           | ~30-50ms         |
| VM snapshot restore               | ~20-40ms         |
| VM snapshot + diff memory         | ~10-20ms         |

## Relation to crun

Both share CLI UX (split, fork), flags, TTL orchestration.
Implementation differs in how child namespace/VM is instantiated.
| Layer          | crun              | Kata                    |
|----------------|-------------------|-------------------------|
| Host FS COW    | overlayfs         | overlayfs               |
| Memory COW     | CRIU              | VM snapshot             |
| Namespace      | setns()           | VM-level network reuse  |
| Init overhead  | ~5ms              | VM boot ~50ms (template)|
| Isolation      | cgroup+ns         | full VM                 |
