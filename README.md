# Raw FUSE Parent/Worker Demo

This project is a **direct `/dev/fuse`** failover demo (no libfuse request loop).
It is meant to validate worker crash/restart semantics with explicit control of
`FUSE_DEV_IOC_CLONE` and FD handoff.

## Goal

Build a process model where:

1. Parent mounts once and keeps the mount alive.
2. Parent does not process requests.
3. Worker process handles requests via a cloned `fuse_dev` fd.
4. If worker crashes, parent starts a new worker without remount.

## Why This Design

### 1) Keep a dedicated keeper fd open in parent
If the last `/dev/fuse` device is closed, kernel aborts the connection.
Kernel path:
- `fs/fuse/dev.c:fuse_dev_release()` decrements `fc->dev_count`
- if it reaches zero, `fuse_abort_conn(fc)` is called
- code pointer: `dev.c:2552-2556`

So parent keeps `master_fd` open as a lifecycle anchor.

### 2) Use `ioctl(FUSE_DEV_IOC_CLONE)` per worker instance
Each worker gets its own `fuse_dev` (its own processing queue context) instead
of sharing one open-file-description across generations.

Kernel-side release behavior for a dead worker channel:
- `fuse_dev_release()` moves that device's `processing` requests to `to_end`
  (`dev.c:2547`)
- then `fuse_dev_end_requests()` marks them `-ECONNABORTED`
  (`dev.c:2406-2414`)

This isolates cleanup to the dead worker's channel and avoids mixing old/new
worker state on the same device endpoint.

### 3) Pass cloned worker fd with `SCM_RIGHTS`
Parent clones, passes fd to child, then closes its own copy of that worker fd.
This ensures worker death actually releases that `fuse_dev` and triggers kernel
cleanup for requests owned by that worker channel.

### 4) Send `FUSE_NOTIFY_RESEND` after worker exit
Parent sends unsolicited notify (`out_header.unique=0`) with
`error=FUSE_NOTIFY_RESEND`.

Protocol and kernel pointers:
- notify code: `include/uapi/linux/fuse.h:675-684`
- `FUSE_NOTIFY_RESEND = 7`: `fuse.h:682`
- notify dispatch for `unique == 0`: `dev.c:2200-2205`
- resend handler: `fuse_resend()` `dev.c:1991-2033`
- resend marker bit: `FUSE_UNIQUE_RESEND` `fuse.h:1017`
- capability bit: `FUSE_HAS_RESEND` `fuse.h:492`

Important: resend can only requeue requests that are still in processing lists
when it runs. Requests already ended during `fuse_dev_release()` are gone.

## Runtime Behavior You Should Expect

When killing a worker with `SIGKILL`:

1. Some in-flight operations may fail once (typically visible as I/O errors in
   client syscalls that were already outstanding).
2. Parent logs worker death and spawns new worker (no resend in current build).
3. New requests should be handled by the new worker.

If you see persistent errors after restart, check whether they are new requests
or retries of requests that were in-flight during crash window.

## Build

```bash
make build
```

## Run

```bash
make run MOUNTPOINT=/tmp/fuse-clone-demo
```

`run` uses `sudo` in Makefile because this demo uses `mount(2)` directly and
usually needs `CAP_SYS_ADMIN`.

## Crash/Failover Test

In shell A:

```bash
make run MOUNTPOINT=/tmp/fuse-clone-demo
```

In shell B:

```bash
ls -la /tmp/fuse-clone-demo
cat /tmp/fuse-clone-demo/hello
```

Kill current worker (not parent):

```bash
kill -9 <worker_pid_from_logs>
```

Then run `ls`/`cat` again and verify new worker logs appear.

## Debug Tips

1. Distinguish one-shot crash-window failure vs persistent failure.
2. Watch for reply write failures (`ENOENT`) as a sign the request unique is no
   longer present in that worker's processing queue.
3. If mount is stuck after abnormal exit:

```bash
fusermount3 -uz /tmp/fuse-clone-demo
# or
sudo umount -l /tmp/fuse-clone-demo
```

## Scope / Limitations

This is a minimal protocol demo, not a production filesystem.
Implemented opcodes are intentionally small (INIT, LOOKUP, GETATTR,
OPENDIR/READDIR, OPEN/READ, plus a few no-op replies).
