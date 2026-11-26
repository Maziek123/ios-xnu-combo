/*
 * op2_shell_sync.c: the kernel keeps the phone's op2shell up to date, so a plain `fastboot boot` is enough.
 *
 * op2shell (userland/op2shell: the USB root shell, SSH, and the configd services for USB and Wi-Fi) lives on the
 * phone's root filesystem as /usr/local/bin/op2-air-run, a zero-padded slot (tools/rootfs_shell.py) that launchd
 * runs once per boot (com.op2.air-run). tools/build-xnu.sh builds it into the kernel (bsd/kern/op2_shell_blob.h,
 * generated and gitignored: the path of out/shell/build/op2shell, its length and SHA-256). Here, after the root
 * filesystem is mounted (read-write on this phone) and before launchd starts, the file on the phone is compared
 * with that copy and, if it differs, overwritten in place from offset 0 and read back. The slot is never truncated:
 * bytes past the new binary are left as they were, as with tools/build-op2shell.sh --install.
 *
 * Boot-arg op2shell_sync=0 turns it off. A failed write leaves the file different, so the next boot tries again.
 */
#if defined(ARM64_BOARD_CONFIG_MSM8994)

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/kauth.h>
#include <sys/mount.h>
#include <pexpert/pexpert.h>

void op2_shell_sync(void);

#if __has_include("op2_shell_blob.h")
#include "op2_shell_blob.h"
__asm__(".section __TEXT,__const\n"
        ".p2align 4\n"
        ".globl _op2_shell_blob\n"
        "_op2_shell_blob:\n"
        ".incbin \"" OP2_SHELL_BLOB_PATH "\"\n"
        ".text\n");
extern const unsigned char op2_shell_blob[];
#endif

#define OP2_SHELL_FILE  "/usr/local/bin/op2-air-run"
#define OP2_SHELL_CHUNK (64u * 1024u)

#ifdef OP2_SHELL_BLOB_LEN
/* compare the file's first OP2_SHELL_BLOB_LEN bytes with the blob: 1 same, 0 different, -errno */
static int
op2_shell_same(vnode_t vp, vfs_context_t ctx, char *buf)
{
	for (uint32_t off = 0; off < OP2_SHELL_BLOB_LEN; off += OP2_SHELL_CHUNK) {
		uint32_t n = OP2_SHELL_BLOB_LEN - off < OP2_SHELL_CHUNK ? OP2_SHELL_BLOB_LEN - off : OP2_SHELL_CHUNK;
		int resid = 0;
		int err = vn_rdwr(UIO_READ, vp, buf, (int)n, (off_t)off, UIO_SYSSPACE, 0, vfs_context_ucred(ctx), &resid,
		    vfs_context_proc(ctx));
		if (err != 0) {
			return -err;
		}
		if (resid != 0 || memcmp(buf, op2_shell_blob + off, n) != 0) {
			return 0;
		}
	}
	return 1;
}
#endif

void
op2_shell_sync(void)
{
#ifndef OP2_SHELL_BLOB_LEN
	printf("OP2 SHELL: no op2shell built into this kernel (tools/build-xnu.sh embeds out/shell/build/op2shell)\n");
#else
	uint32_t en = 1;
	PE_parse_boot_argn("op2shell_sync", &en, sizeof(en));
	if (!en) {
		printf("OP2 SHELL: op2shell_sync=0: " OP2_SHELL_FILE " left as it is\n");
		return;
	}
	vfs_context_t ctx = vfs_context_kernel();
	vnode_t vp = NULLVP;
	int err = vnode_open(OP2_SHELL_FILE, FREAD | FWRITE, 0, 0, &vp, ctx);
	if (err != 0) {
		printf("OP2 SHELL: cannot open " OP2_SHELL_FILE " (errno %d); op2shell not updated\n", err);
		return;
	}
	char *buf = (char *)_MALLOC(OP2_SHELL_CHUNK, M_TEMP, M_WAITOK);
	if (buf == NULL) {
		(void)vnode_close(vp, FREAD | FWRITE, ctx);
		printf("OP2 SHELL: no memory; op2shell not updated\n");
		return;
	}
	int same = op2_shell_same(vp, ctx, buf);
	if (same == 1) {
		printf("OP2 SHELL: " OP2_SHELL_FILE " is the kernel's op2shell (%u bytes, sha256 %s)\n", OP2_SHELL_BLOB_LEN,
		    OP2_SHELL_BLOB_SHA);
	} else if (same < 0) {
		printf("OP2 SHELL: reading " OP2_SHELL_FILE " failed (errno %d); not updated\n", -same);
	} else {
		int resid = 0;
		err = vn_rdwr(UIO_WRITE, vp, (caddr_t)(uintptr_t)op2_shell_blob, (int)OP2_SHELL_BLOB_LEN, 0, UIO_SYSSPACE,
		    IO_SYNC | IO_UNIT, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
		if (err == 0 && resid == 0) {
			(void)VNOP_FSYNC(vp, MNT_WAIT, ctx);
		}
		int check = (err == 0 && resid == 0) ? op2_shell_same(vp, ctx, buf) : 0;
		printf("OP2 SHELL: " OP2_SHELL_FILE " differed: wrote the kernel's op2shell (%u bytes, sha256 %s): "
		    "errno %d, %d bytes not written, read back %s\n", OP2_SHELL_BLOB_LEN, OP2_SHELL_BLOB_SHA, err, resid,
		    check == 1 ? "identical" : "DIFFERENT (tries again next boot)");
	}
	_FREE(buf, M_TEMP);
	(void)vnode_close(vp, FREAD | FWRITE, ctx);
#endif
}

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
