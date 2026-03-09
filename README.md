# Raw FUSE Parent/Worker Demo

This repo demonstrates one thing: **worker crash auto-restart without remount,
while avoiding permanent connection-dead states (`ENOTCONN`/transport endpoint broken)**.

The implementation is direct `/dev/fuse` protocol handling (no libfuse request loop).

## Core Objective

1. Parent mounts once and keeps mount lifetime stable.
2. Worker handles requests on a dedicated cloned `fuse_dev` fd.
3. Worker can die (`SIGKILL`) and be replaced immediately.
4. New worker should continue serving new requests without requiring remount.

## Restart Flow (Important)

```text
Parent:
  open /dev/fuse -> mount -> keep master_fd open forever (keeper)

Loop:
  clone worker fd via ioctl(FUSE_DEV_IOC_CLONE)
  fork worker
  pass cloned fd to worker via SCM_RIGHTS
  close parent's copy of that worker fd
  wait worker exit
  repeat

Worker:
  recv fd
  read/write /dev/fuse protocol loop
  if killed/crash -> process exits, fd closes, kernel releases that fuse_dev
```

Why this flow matters:
- Parent keeper fd prevents “last device closed -> full connection abort”.
- Per-worker cloned fd isolates worker generation state.
- Parent closing its worker-fd copy ensures worker death really releases that
  worker `fuse_dev` in kernel.

## Why `ioctl(FUSE_DEV_IOC_CLONE)` (not plain fd duplication)

`dup()`/inheritance keeps the same open-file-description and effectively the same
`fuse_dev` endpoint. For crash failover that is harder to reason about.

`FUSE_DEV_IOC_CLONE` gives each worker its own `fuse_dev` endpoint, so cleanup on
worker death is localized.

Kernel pointers:
- worker fd release path: `fs/fuse/dev.c:fuse_dev_release()` (`dev.c:2534+`)
- dead worker processing requests are ended: `dev.c:2547`, `dev.c:2550`
- ended with `-ECONNABORTED`: `fuse_dev_end_requests()` (`dev.c:2406-2414`)
- last device close aborts whole connection: `dev.c:2552-2556`

## RESEND Policy (Current Repo)

`FUSE_NOTIFY_RESEND` is intentionally **disabled** in this repo.

Reason:
- It can replay old request identity/state across worker generations.
- In complex filesystems this interacts badly with inode lifecycle/reuse and
  increases correctness burden.

Current policy is simpler and explicit:
1. Crash-window in-flight requests may fail.
2. New worker serves new requests.
3. No replay of pre-crash requests.

Reference pointers (if you later want to re-enable resend):
- `FUSE_NOTIFY_RESEND` enum: `include/uapi/linux/fuse.h:682`
- notify dispatch (`unique == 0`): `fs/fuse/dev.c:2200-2205`
- resend handler: `fs/fuse/dev.c:1991-2033`
- resend bit: `FUSE_UNIQUE_RESEND` `fuse.h:1017`
- capability bit: `FUSE_HAS_RESEND` `fuse.h:492`

## Failure Behavior Cheat Sheet

| Symptom | Meaning in this model | Expected? | What to do |
|---|---|---|---|
| one-time `Input/output error` right after killing worker | request was in-flight during crash window, got failed during worker-fd release | Yes | retry command; verify new worker logs appear |
| persistent `Transport endpoint is not connected` / `ENOTCONN` | whole FUSE connection likely aborted (keeper broken / last device closed) | No | restart demo; verify parent keeper fd never exits |
| worker reply write `ENOENT` | reply unique not present in current processing queue | Possible around crash boundary | correlate with crash timing; treat as old request completion race |
| shell still acts weird until `cd` | cwd/path refs may be stale after disruption | Sometimes | `cd / && cd <mount>`; this should be exceptional, not steady-state |

Design target: after worker restart, **new requests keep working without remount**.

## Build / Run

```bash
make build
make run MOUNTPOINT=/tmp/fuse-clone-demo
```

`run` uses `sudo` because this demo calls `mount(2)` directly and typically needs
`CAP_SYS_ADMIN`.

## Crash Test

Shell A:

```bash
make run MOUNTPOINT=/tmp/fuse-clone-demo
```

Shell B:

```bash
ls -la /tmp/fuse-clone-demo
cat /tmp/fuse-clone-demo/hello
kill -9 <worker_pid_from_logs>
ls -la /tmp/fuse-clone-demo
cat /tmp/fuse-clone-demo/hello
```

Pass condition:
- worker is respawned by parent
- transient failure may happen at kill edge
- subsequent new requests succeed without remount

## Cleanup

```bash
fusermount3 -uz /tmp/fuse-clone-demo
# or
sudo umount -l /tmp/fuse-clone-demo
```
