// SPDX-License-Identifier: GPL-2.0-or-later
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <linux/fuse.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef FUSE_DEV_IOC_CLONE
#define FUSE_DEV_IOC_CLONE _IOW(229, 0, uint32_t)
#endif

#define LOGF(fmt, ...) \
	fprintf(stderr, "[raw-fuse pid=%ld] " fmt "\n", (long)getpid(), ##__VA_ARGS__)
#define ARR_SZ(a) (sizeof(a) / sizeof((a)[0]))

static const char *k_hello_name = "hello";
static char k_hello_str[512];
static volatile sig_atomic_t g_stop;
static volatile sig_atomic_t g_sigint_count;

struct fuse_req_view {
	struct fuse_in_header ih;
	const uint8_t *payload;
	size_t payload_len;
};

static void on_signal(int sig)
{
	if (sig == SIGTERM) {
		g_stop = 1;
		return;
	}
	if (sig == SIGINT)
		g_sigint_count++;
}

static long long monotonic_ms(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		return -1;
	return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int get_env_int(const char *name, int def)
{
	const char *s = getenv(name);
	char *end = NULL;
	long v;

	if (!s || !*s)
		return def;
	errno = 0;
	v = strtol(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0' || v < 0 || v > INT32_MAX)
		return def;
	return (int)v;
}

static void sleep_ms(int ms)
{
	struct timespec req, rem;

	if (ms <= 0)
		return;
	req.tv_sec = ms / 1000;
	req.tv_nsec = (long)(ms % 1000) * 1000000L;
	while (nanosleep(&req, &rem) < 0 && errno == EINTR)
		req = rem;
}

static void set_hello_str(pid_t worker_pid)
{
	snprintf(k_hello_str, sizeof(k_hello_str),
		 "hello from raw /dev/fuse demo (worker pid=%ld)\n"
		 "Try: kill -9 %ld\n"
		 "Or press Ctrl+C in the other terminal running this demo to stop the keeper process.\n",
		 (long)worker_pid, (long)worker_pid);
}

static int clone_fuse_dev_fd(int master_fd)
{
	int fd;
	uint32_t arg = (uint32_t)master_fd;

	fd = open("/dev/fuse", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (ioctl(fd, FUSE_DEV_IOC_CLONE, &arg) < 0) {
		int saved = errno;
		close(fd);
		errno = saved;
		return -1;
	}
	return fd;
}

static int send_fd(int sock, int fd)
{
	char c = 'f';
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { .iov_base = &c, .iov_len = sizeof(c) };
	struct msghdr msg = { 0 };
	struct cmsghdr *cmsg;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof(cbuf);

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));

	return sendmsg(sock, &msg, 0) < 0 ? -1 : 0;
}

static int recv_fd(int sock)
{
	char c;
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { .iov_base = &c, .iov_len = sizeof(c) };
	struct msghdr msg = { 0 };
	struct cmsghdr *cmsg;
	int fd = -1;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof(cbuf);

	if (recvmsg(sock, &msg, 0) <= 0)
		return -1;

	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
			memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
			return fd;
		}
	}
	return -1;
}

static int send_out_iov(int fd, int err, uint64_t unique,
			const struct iovec *iov, int iovcnt)
{
	struct fuse_out_header oh;
	struct iovec vec[8];
	size_t len = sizeof(oh);
	int i;

	if (iovcnt + 1 > (int)ARR_SZ(vec)) {
		errno = E2BIG;
		return -1;
	}

	for (i = 0; i < iovcnt; i++)
		len += iov[i].iov_len;

	oh.len = (uint32_t)len;
	oh.error = err;
	oh.unique = unique;

	vec[0].iov_base = &oh;
	vec[0].iov_len = sizeof(oh);
	for (i = 0; i < iovcnt; i++)
		vec[i + 1] = iov[i];

	if (writev(fd, vec, iovcnt + 1) < 0)
		return -1;
	return 0;
}

static int send_err(int fd, uint64_t unique, int err)
{
	return send_out_iov(fd, -err, unique, NULL, 0);
}

static size_t add_dirent(char *dst, size_t cap, off_t off, uint64_t ino,
			 const char *name, unsigned type)
{
	size_t nlen = strlen(name);
	size_t ent_len = FUSE_NAME_OFFSET + nlen;
	size_t padded = FUSE_DIRENT_ALIGN(ent_len);
	struct fuse_dirent *de;

	if (dst == NULL || cap < padded)
		return padded;

	de = (struct fuse_dirent *)dst;
	de->ino = ino;
	de->off = off;
	de->namelen = nlen;
	de->type = type;
	memcpy(de->name, name, nlen);
	memset(de->name + nlen, 0, padded - ent_len);
	return padded;
}

static int do_readdir_reply(int fd, const struct fuse_req_view *rq)
{
	const struct fuse_read_in *in;
	char dirbuf[512];
	size_t sz0, sz1, sz2, total;
	off_t off;
	size_t max;
	struct iovec iov = { 0 };

	if (rq->payload_len < sizeof(*in))
		return send_err(fd, rq->ih.unique, EINVAL);
	in = (const struct fuse_read_in *)rq->payload;
	off = (off_t)in->offset;
	max = in->size;

	sz0 = add_dirent(dirbuf, sizeof(dirbuf), 1, FUSE_ROOT_ID, ".", DT_DIR);
	sz1 = add_dirent(dirbuf + sz0, sizeof(dirbuf) - sz0, 2, FUSE_ROOT_ID, "..", DT_DIR);
	sz2 = add_dirent(dirbuf + sz0 + sz1, sizeof(dirbuf) - sz0 - sz1, 3, 2,
			 k_hello_name, DT_REG);
	total = sz0 + sz1 + sz2;

	if ((size_t)off >= total)
		return send_out_iov(fd, 0, rq->ih.unique, NULL, 0);

	iov.iov_base = dirbuf + off;
	iov.iov_len = total - (size_t)off;
	if (iov.iov_len > max)
		iov.iov_len = max;
	return send_out_iov(fd, 0, rq->ih.unique, &iov, 1);
}

static int handle_request(int fd, const struct fuse_req_view *rq)
{
	switch (rq->ih.opcode) {
	case FUSE_INIT: {
		const struct fuse_init_in *in;
		struct fuse_init_out out = { 0 };
		struct iovec iov;
		uint64_t flags64;

		if (rq->payload_len < sizeof(*in))
			return send_err(fd, rq->ih.unique, EINVAL);
		in = (const struct fuse_init_in *)rq->payload;
		out.major = FUSE_KERNEL_VERSION;
		out.minor = in->minor < FUSE_KERNEL_MINOR_VERSION ? in->minor : FUSE_KERNEL_MINOR_VERSION;
		out.max_readahead = in->max_readahead;
		flags64 = FUSE_ASYNC_READ | FUSE_BIG_WRITES;
		out.flags = (uint32_t)flags64;
		out.flags2 = (uint32_t)(flags64 >> 32);
		out.max_background = 64;
		out.congestion_threshold = 32;
		out.max_write = 128 * 1024;
		out.time_gran = 1;
		out.max_pages = 32;

		iov.iov_base = &out;
		iov.iov_len = sizeof(out);
		LOGF("INIT major=%u minor=%u", in->major, in->minor);
		return send_out_iov(fd, 0, rq->ih.unique, &iov, 1);
	}
	case FUSE_LOOKUP: {
		const char *name = (const char *)rq->payload;
		struct fuse_entry_out out = { 0 };
		struct iovec iov;

		if (rq->ih.nodeid != FUSE_ROOT_ID)
			return send_err(fd, rq->ih.unique, ENOENT);
		if (rq->payload_len == 0 || name[rq->payload_len - 1] != '\0')
			return send_err(fd, rq->ih.unique, EINVAL);

		if (strcmp(name, k_hello_name) != 0)
			return send_err(fd, rq->ih.unique, ENOENT);

		out.nodeid = 2;
		out.generation = 1;
		out.entry_valid = 1;
		out.attr_valid = 1;
		out.attr.ino = 2;
		out.attr.mode = S_IFREG | 0444;
		out.attr.nlink = 1;
		out.attr.size = strlen(k_hello_str);
		iov.iov_base = &out;
		iov.iov_len = sizeof(out);
		LOGF("LOOKUP name=%s", name);
		return send_out_iov(fd, 0, rq->ih.unique, &iov, 1);
	}
	case FUSE_GETATTR: {
		struct fuse_attr_out out = { 0 };
		struct iovec iov;
		out.attr_valid = 1;
		if (rq->ih.nodeid == FUSE_ROOT_ID) {
			out.attr.ino = FUSE_ROOT_ID;
			out.attr.mode = S_IFDIR | 0555;
			out.attr.nlink = 2;
		} else if (rq->ih.nodeid == 2) {
			out.attr.ino = 2;
			out.attr.mode = S_IFREG | 0444;
			out.attr.nlink = 1;
			out.attr.size = strlen(k_hello_str);
		} else {
			return send_err(fd, rq->ih.unique, ENOENT);
		}
		iov.iov_base = &out;
		iov.iov_len = sizeof(out);
		LOGF("GETATTR nodeid=%llu", (unsigned long long)rq->ih.nodeid);
		return send_out_iov(fd, 0, rq->ih.unique, &iov, 1);
	}
	case FUSE_OPENDIR:
	case FUSE_OPEN: {
		struct fuse_open_out out = { 0 };
		struct iovec iov = { .iov_base = &out, .iov_len = sizeof(out) };
		LOGF("OPEN opcode=%u nodeid=%llu", rq->ih.opcode,
		     (unsigned long long)rq->ih.nodeid);
		return send_out_iov(fd, 0, rq->ih.unique, &iov, 1);
	}
	case FUSE_READDIR:
		LOGF("READDIR nodeid=%llu", (unsigned long long)rq->ih.nodeid);
		return do_readdir_reply(fd, rq);
	case FUSE_READ: {
		const struct fuse_read_in *in;
		size_t len = strlen(k_hello_str);
		size_t off;
		size_t out_len;
		struct iovec iov;

		if (rq->payload_len < sizeof(*in))
			return send_err(fd, rq->ih.unique, EINVAL);
		in = (const struct fuse_read_in *)rq->payload;
		off = (size_t)in->offset;
		if (off >= len)
			return send_out_iov(fd, 0, rq->ih.unique, NULL, 0);
		out_len = len - off;
		if (out_len > in->size)
			out_len = in->size;
		iov.iov_base = (void *)(k_hello_str + off);
		iov.iov_len = out_len;
		LOGF("READ nodeid=%llu size=%u off=%llu", (unsigned long long)rq->ih.nodeid,
		     in->size, (unsigned long long)in->offset);
		return send_out_iov(fd, 0, rq->ih.unique, &iov, 1);
	}
	case FUSE_FORGET:
	case FUSE_BATCH_FORGET:
		return 0;
	case FUSE_RELEASE:
	case FUSE_RELEASEDIR:
	case FUSE_FLUSH:
	case FUSE_FSYNC:
	case FUSE_STATFS:
		return send_out_iov(fd, 0, rq->ih.unique, NULL, 0);
	default:
		LOGF("opcode=%u nodeid=%llu -> ENOSYS", rq->ih.opcode,
		     (unsigned long long)rq->ih.nodeid);
		return send_err(fd, rq->ih.unique, ENOSYS);
	}
}

static int worker_loop(int dev_fd)
{
	uint8_t buf[1 << 20];

	for (;;) {
		ssize_t n = read(dev_fd, buf, sizeof(buf));
		struct fuse_req_view rq;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			LOGF("worker read err=%d (%s)", errno, strerror(errno));
			return 1;
		}
		if (n == 0) {
			LOGF("worker read EOF");
			return 0;
		}
		if ((size_t)n < sizeof(struct fuse_in_header)) {
			LOGF("worker short read=%zd", n);
			return 1;
		}

		memcpy(&rq.ih, buf, sizeof(rq.ih));
		rq.payload = buf + sizeof(rq.ih);
		rq.payload_len = (size_t)n - sizeof(rq.ih);

		if (handle_request(dev_fd, &rq) < 0) {
			LOGF("worker write reply failed err=%d (%s)", errno, strerror(errno));
			return 1;
		}
	}
}

static int mount_raw_fuse(int dev_fd, const char *mountpoint)
{
	char opts[512];
	unsigned uid = (unsigned)getuid();
	unsigned gid = (unsigned)getgid();

	snprintf(opts, sizeof(opts),
		 "fd=%d,rootmode=40000,user_id=%u,group_id=%u,max_read=131072", dev_fd, uid,
		 gid);

	if (mount("rawfuse", mountpoint, "fuse", MS_NOSUID | MS_NODEV, opts) < 0)
		return -1;
	return 0;
}

int main(int argc, char **argv)
{
	int master_fd = -1;
	pid_t worker_pid = -1;
	int max_restarts = 0;
	int worker_spawn_delay_ms = 0;
	int restarts = 0;
	sig_atomic_t handled_sigint = 0;
	long long last_sigint_ms = -1;
	const char *mountpoint;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <mountpoint>\n", argv[0]);
		return 2;
	}
	mountpoint = argv[1];
	set_hello_str(getpid());
	max_restarts = get_env_int("MAX_RESTARTS", 0);
	worker_spawn_delay_ms = get_env_int("WORKER_SPAWN_DELAY_MS", 0);
	if (worker_spawn_delay_ms > 0)
		LOGF("worker spawn delay enabled: %d ms", worker_spawn_delay_ms);

	master_fd = open("/dev/fuse", O_RDWR | O_CLOEXEC);
	if (master_fd < 0) {
		perror("open /dev/fuse");
		return 1;
	}
	if (mount_raw_fuse(master_fd, mountpoint) < 0) {
		perror("mount fuse");
		close(master_fd);
		return 1;
	}

	LOGF("mounted on %s master_fd=%d", mountpoint, master_fd);
	LOGF("parent acts as keeper, worker has dedicated cloned fd");

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	while (!g_stop) {
		int sv[2];
		int worker_fd;
		int st;
		long long now_ms;

		if (max_restarts > 0 && restarts >= max_restarts)
			break;
		while (handled_sigint < g_sigint_count) {
			now_ms = monotonic_ms();
			if (last_sigint_ms >= 0 && now_ms >= 0 && now_ms - last_sigint_ms < 1000) {
				LOGF("SIGINT twice within 1s -> stopping parent");
				g_stop = 1;
				break;
			}
			last_sigint_ms = now_ms;
			handled_sigint++;
			LOGF("SIGINT received: restarting worker; press Ctrl-C again within 1s to stop parent");
		}
		if (g_stop)
			break;
		if (worker_spawn_delay_ms > 0) {
			LOGF("sleeping %d ms before spawning worker", worker_spawn_delay_ms);
			sleep_ms(worker_spawn_delay_ms);
		}

		if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, sv) < 0)
			break;

		worker_pid = fork();
		if (worker_pid < 0) {
			close(sv[0]);
			close(sv[1]);
			break;
		}
		if (worker_pid == 0) {
			int fd;
			signal(SIGINT, SIG_DFL);
			signal(SIGTERM, SIG_DFL);
			close(sv[0]);
			fd = recv_fd(sv[1]);
			close(sv[1]);
			if (fd < 0)
				_Exit(2);
			set_hello_str(getpid());
			LOGF("worker got fd=%d", fd);
			_Exit(worker_loop(fd));
		}

		close(sv[1]);
		worker_fd = clone_fuse_dev_fd(master_fd);
		if (worker_fd < 0) {
			close(sv[0]);
			kill(worker_pid, SIGKILL);
			waitpid(worker_pid, NULL, 0);
			break;
		}
		LOGF("spawned worker pid=%ld worker_fd=%d", (long)worker_pid, worker_fd);
		if (send_fd(sv[0], worker_fd) < 0)
			LOGF("send_fd failed err=%d (%s)", errno, strerror(errno));
		close(worker_fd);
		close(sv[0]);

		for (;;) {
			if (waitpid(worker_pid, &st, 0) < 0) {
				if (errno == EINTR) {
					while (handled_sigint < g_sigint_count) {
						now_ms = monotonic_ms();
						if (last_sigint_ms >= 0 && now_ms >= 0 &&
						    now_ms - last_sigint_ms < 1000) {
							LOGF("SIGINT twice within 1s -> stopping parent");
							g_stop = 1;
							break;
						}
						last_sigint_ms = now_ms;
						handled_sigint++;
						LOGF("SIGINT received: restarting worker; press Ctrl-C again within 1s to stop parent");
					}
					if (g_stop && worker_pid > 0)
						kill(worker_pid, SIGTERM);
					if (!g_stop)
						continue;
				}
				goto out;
			}
			break;
		}

		if (WIFSIGNALED(st))
			LOGF("worker pid=%ld killed by signal=%d", (long)worker_pid, WTERMSIG(st));
		else
			LOGF("worker pid=%ld exited status=%d", (long)worker_pid, WEXITSTATUS(st));

		/*
		 * Intentionally disabled: resend can replay old unique/node state and
		 * makes inode-lifecycle handling much more complex after worker crash.
		 */

		restarts++;
		worker_pid = -1;
	}

out:
	if (worker_pid > 0)
		kill(worker_pid, SIGTERM);
	umount2(mountpoint, MNT_DETACH);
	close(master_fd);
	return 0;
}
