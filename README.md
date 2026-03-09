# raw_fuse_clone_parent_worker_demo

Hardcore demo with direct `/dev/fuse` protocol handling (no libfuse request loop).

## Model

1. Parent opens `/dev/fuse` and mounts `type=fuse` using mount(2).
2. Parent keeps `master_fd` as keeper and never reads requests itself.
3. Each worker restart uses `ioctl(FUSE_DEV_IOC_CLONE)` for dedicated worker fd.
4. Parent passes worker fd via `SCM_RIGHTS`; worker reads/writes raw FUSE messages.
5. On worker exit parent sends `FUSE_NOTIFY_RESEND` and respawns.

## Build

```bash
make -f Makefile.demo build-raw-parent-worker
```

## Run

```bash
make -f Makefile.demo run-raw-parent-worker MOUNTPOINT=/tmp/fuse-clone-demo
```

Notes:

- This path uses `mount(2)` directly and may require privileges (`CAP_SYS_ADMIN`).
- Implemented opcodes are minimal: INIT/LOOKUP/GETATTR/OPENDIR/READDIR/OPEN/READ and a few no-op replies.
