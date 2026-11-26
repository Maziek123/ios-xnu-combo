/*
 * op2_shell_sync.c: the kernel keeps the phone's helper programs up to date, so a plain `fastboot boot` is enough.
 *
 * tools/build-xnu.sh builds two userland programs into the kernel (generated, gitignored headers next to this file:
 * the program's path on the build machine, its length and SHA-256), each only if it could be built:
 *  - op2shell (userland/op2shell: USB root shell, SSH, configd services) -> /usr/local/bin/op2-air-run, a zero-padded
 *    slot (tools/rootfs_shell.py) that launchd runs once per boot. Only updated in place: never created, never
 *    truncated (bytes past the new binary stay as they were, as with tools/build-op2shell.sh --install).
 *  - op2netd (userland/op2netd: only the configd services) -> /usr/sbin/op2netd, created when missing and cut to
 *    its size. It has no launchd job: when launchd starts /usr/sbin/wifid (Apple's Wi-Fi daemon, nothing to drive
 *    on this phone) exec_save_path() asks op2_netd_redirect() and gets /usr/sbin/op2netd instead; op2netd forks
 *    its publisher and then execs the real wifid itself (not redirected), so wifid's job carries on.
 * Run from bsdinit_task once the root filesystem is mounted (read-write on this phone), before launchd.
 * Boot-args op2shell_sync=0 / op2netd=0 turn each off. A failed write leaves the file different, so the next boot
 * tries again.
 *
 * The boot-time lines are long gone from the 256 KB ramoops ring by the time anyone reads the log, so the outcomes
 * are kept and op2_shell_status() repeats them, with which of the programs run, for Wi-Fi step 8's log lines.
 */
#if defined(ARM64_BOARD_CONFIG_MSM8994)

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/kauth.h>
#include <sys/mount.h>
#include <pexpert/pexpert.h>

void op2_shell_sync(void);
void op2_keep(const char *fmt, ...) __printflike(1, 2);   /* bsd/dev/op2_keeplog.c */
#define KLOG(...) do { printf(__VA_ARGS__); op2_keep(__VA_ARGS__); } while (0)   /* console + the kept log */
void op2_shell_status(char *out, size_t n);
size_t op2_netd_redirect(char *path, size_t len, size_t cap);

#if __has_include("op2_shell_blob.h")
#include "op2_shell_blob.h"
__asm__(".section __TEXT,__const\n.p2align 4\n.globl _op2_shell_blob\n_op2_shell_blob:\n"
        ".incbin \"" OP2_SHELL_BLOB_PATH "\"\n.text\n");
extern const unsigned char op2_shell_blob[];
#endif
#if __has_include("op2_netd_blob.h")
#include "op2_netd_blob.h"
__asm__(".section __TEXT,__const\n.p2align 4\n.globl _op2_netd_blob\n_op2_netd_blob:\n"
        ".incbin \"" OP2_NETD_BLOB_PATH "\"\n.text\n");
extern const unsigned char op2_netd_blob[];
#endif

#define CHUNK (64u * 1024u)
#define NETD_FROM "/usr/sbin/wifid"
#define NETD_TO   "/usr/sbin/op2netd"

static char shell_result[192] = "no op2shell built into this kernel (tools/build-xnu.sh: out/shell/build/op2shell)";
static char netd_result[192] = "no op2netd built into this kernel (tools/build-xnu.sh: out/op2netd/op2netd)";
static int netd_ready;            /* /usr/sbin/op2netd is the kernel's copy: redirect wifid */
static unsigned netd_redirects;

#if defined(OP2_SHELL_BLOB_LEN) || defined(OP2_NETD_BLOB_LEN)
/* compare the file's first len bytes with blob: 1 same, 0 different, -errno */
static int
same(vnode_t vp, vfs_context_t ctx, char *buf, const unsigned char *blob, uint32_t len)
{
	for (uint32_t off = 0; off < len; off += CHUNK) {
		uint32_t n = len - off < CHUNK ? len - off : CHUNK;
		int resid = 0;
		int err = vn_rdwr(UIO_READ, vp, buf, (int)n, (off_t)off, UIO_SYSSPACE, 0, vfs_context_ucred(ctx), &resid,
		    vfs_context_proc(ctx));
		if (err != 0) {
			return -err;
		}
		if (resid != 0 || memcmp(buf, blob + off, n) != 0) {
			return 0;
		}
	}
	return 1;
}

/* bring path to blob, creating and truncating if create; the outcome into out; 1 if the file is (now) the blob */
static int
sync_file(const char *what, const char *path, const unsigned char *blob, uint32_t len, const char *sha, int create,
    char *out, size_t outn)
{
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	char *buf = NULL;
	int ok = 0;
	int err = vnode_open(path, FREAD | FWRITE | (create ? O_CREAT : 0), 0755, 0, &vp, ctx);
	if (err != 0) {
		snprintf(out, outn, "cannot open %s (errno %d%s); %s not updated", path, err,
		    err == ENOENT ? ": not installed on this rootfs" : "", what);
		goto done;
	}
	buf = (char *)_MALLOC(CHUNK, M_TEMP, M_WAITOK);
	if (buf == NULL) {
		snprintf(out, outn, "no memory; %s not updated", what);
		goto close;
	}
	off_t size = -1;
	if (create) {
		struct vnode_attr va;
		VATTR_INIT(&va);
		VATTR_WANTED(&va, va_data_size);
		if (vnode_getattr(vp, &va, ctx) == 0) {
			size = (off_t)va.va_data_size;
		}
	}
	int s = same(vp, ctx, buf, blob, len);
	if (s == 1 && (!create || size == (off_t)len)) {
		snprintf(out, outn, "%s already was the kernel's %s (%u bytes, sha256 %s)", path, what, len, sha);
		ok = 1;
	} else if (s < 0) {
		snprintf(out, outn, "reading %s failed (errno %d); %s not updated", path, -s, what);
	} else {
		int resid = 0;
		err = vn_rdwr(UIO_WRITE, vp, (caddr_t)(uintptr_t)blob, (int)len, 0, UIO_SYSSPACE, IO_SYNC | IO_UNIT,
		    vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
		if (err == 0 && resid == 0 && create && size != (off_t)len) {
			struct vnode_attr va;
			VATTR_INIT(&va);
			VATTR_SET(&va, va_data_size, (off_t)len);
			err = vnode_setattr(vp, &va, ctx);
		}
		if (err == 0 && resid == 0) {
			(void)VNOP_FSYNC(vp, MNT_WAIT, ctx);
		}
		int check = (err == 0 && resid == 0) ? same(vp, ctx, buf, blob, len) : 0;
		ok = check == 1;
		snprintf(out, outn, "%s %s: wrote the kernel's %s (%u bytes, sha256 %s): errno %d, %d bytes not written, read "
		    "back %s", path, size == 0 ? "was new" : "differed", what, len, sha, err, resid,
		    ok ? "identical" : "DIFFERENT (tries again next boot)");
	}
	_FREE(buf, M_TEMP);
close:
	(void)vnode_close(vp, FREAD | FWRITE, ctx);
done:
	KLOG("OP2 SHELL: %s\n", out);
	return ok;
}
#endif

void
op2_shell_sync(void)
{
#if defined(OP2_SHELL_BLOB_LEN) || defined(OP2_NETD_BLOB_LEN)
	uint32_t en;
#endif
#ifdef OP2_SHELL_BLOB_LEN
	en = 1;
	PE_parse_boot_argn("op2shell_sync", &en, sizeof(en));
	if (en) {
		(void)sync_file("op2shell", "/usr/local/bin/op2-air-run", op2_shell_blob, OP2_SHELL_BLOB_LEN,
		    OP2_SHELL_BLOB_SHA, 0, shell_result, sizeof(shell_result));
	} else {
		snprintf(shell_result, sizeof(shell_result), "op2shell_sync=0: /usr/local/bin/op2-air-run left as it is");
	}
#else
	KLOG("OP2 SHELL: %s\n", shell_result);
#endif
#ifdef OP2_NETD_BLOB_LEN
	en = 1;
	PE_parse_boot_argn("op2netd", &en, sizeof(en));
	if (en) {
		netd_ready = sync_file("op2netd", NETD_TO, op2_netd_blob, OP2_NETD_BLOB_LEN, OP2_NETD_BLOB_SHA, 1,
		    netd_result, sizeof(netd_result));
	} else {
		snprintf(netd_result, sizeof(netd_result), "op2netd=0: not installed, wifid starts as usual");
	}
#else
	KLOG("OP2 SHELL: %s\n", netd_result);
#endif
}

/* exec_save_path(): launchd starting /usr/sbin/wifid gets /usr/sbin/op2netd (not op2netd's own exec of wifid).
 * path holds len bytes with the NUL, cap is its room; returns the new len. */
size_t
op2_netd_redirect(char *path, size_t len, size_t cap)
{
	char me[MAXCOMLEN + 1];
	if (!netd_ready || len != sizeof(NETD_FROM) || cap < sizeof(NETD_TO) ||
	    memcmp(path, NETD_FROM, sizeof(NETD_FROM)) != 0) {
		return len;
	}
	proc_selfname(me, sizeof(me));
	if (strcmp(me, "op2netd") == 0) {
		return len;
	}
	memcpy(path, NETD_TO, sizeof(NETD_TO));
	if (netd_redirects++ < 4) {
		KLOG("OP2 SHELL: %s starts " NETD_FROM ": " NETD_TO " first (it starts the real wifid itself)\n", me);
	}
	return sizeof(NETD_TO);
}

struct find_arg {
	const char *name;
	int pid;
};

static int
find_proc(proc_t p, void *arg)
{
	struct find_arg *f = (struct find_arg *)arg;
	char name[MAXCOMLEN + 1];
	proc_name(proc_pid(p), name, sizeof(name));
	if (f->pid == 0 && strcmp(name, f->name) == 0) {
		f->pid = proc_pid(p);
	}
	return PROC_RETURNED;
}

static int
running(const char *name)
{
	struct find_arg f = { name, 0 };
	proc_iterate(PROC_ALLPROCLIST, find_proc, &f, NULL, NULL);
	return f.pid;
}

/* for step 8: what the boot-time checks did, and which of the programs run */
void
op2_shell_status(char *out, size_t n)
{
	int sh = running("op2-air-run"), nd = running("op2netd");
	snprintf(out, n, "%s; op2-air-run %s | op2netd: %s; wifid starts redirected %u time(s), op2netd %s", shell_result,
	    sh ? "running" : "NOT running", netd_result, netd_redirects,
	    nd ? "running (publishes to configd)" : "NOT running");
}

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
