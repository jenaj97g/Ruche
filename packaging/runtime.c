/*
 * AppImage type 2 runtime for Ruche.
 *
 * The file is this ELF followed by a SquashFS image. On start it mounts the image with
 * FUSE (squashfuse, statically linked) and execs AppRun from it. When FUSE is not usable it
 * extracts the image once into ~/.cache and runs from there instead.
 *
 * Supports the usual flags: --appimage-extract [pattern], --appimage-extract-and-run,
 * --appimage-mount, --appimage-offset, --appimage-help, --appimage-version.
 */
#define _GNU_SOURCE
#include <squashfuse/ll.h>
#include <squashfuse/traverse.h>
#include <squashfuse/util.h>

#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <malloc.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Sections that AppImage tooling looks for (update info, signature). */
__attribute__((section(".upd_info"), used)) static char upd_info[1024] = "";
__attribute__((section(".sha256_sig"), used)) static char sha256_sig[1024] = "";
__attribute__((section(".sig_key"), used)) static char sig_key[8192] = "";

#define RUNTIME_VERSION "ruche-runtime 1"

static void die(const char *msg)
{
    fprintf(stderr, "AppImage: %s\n", msg);
    exit(1);
}

static off_t elf_size(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    Elf64_Ehdr h;
    off_t r = -1;
    if (pread(fd, &h, sizeof h, 0) == sizeof h && memcmp(h.e_ident, ELFMAG, SELFMAG) == 0 &&
        h.e_ident[EI_CLASS] == ELFCLASS64)
        r = (off_t)(h.e_shoff + (uint64_t)h.e_shentsize * h.e_shnum);
    close(fd);
    return r;
}

/* ------------------------------------------------------------------------- extraction */

static int mkdirs(char *path, mode_t mode)
{
    for (char *p = path + 1; *p; ++p) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(path, 0755) < 0 && errno != EEXIST) {
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(path, mode) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int extract(const char *image, off_t offset, const char *dest, const char *pattern)
{
    sqfs fs;
    if (sqfs_open_image(&fs, image, (size_t)offset) != SQFS_OK) {
        fprintf(stderr, "AppImage: cannot open the embedded image\n");
        return -1;
    }
    sqfs_traverse trv;
    if (sqfs_traverse_open(&trv, &fs, sqfs_inode_root(&fs)) != SQFS_OK) {
        sqfs_destroy(&fs);
        return -1;
    }
    char *buf = malloc(1 << 20);
    sqfs_err err = SQFS_OK;
    int rc = 0;
    while (sqfs_traverse_next(&trv, &err)) {
        if (trv.dir_end)
            continue;
        if (pattern && fnmatch(pattern, trv.path, FNM_LEADING_DIR) != 0)
            continue;
        char out[PATH_MAX];
        snprintf(out, sizeof out, "%s/%s", dest, trv.path);
        sqfs_inode inode;
        if (sqfs_inode_get(&fs, &inode, sqfs_dentry_inode(&trv.entry)) != SQFS_OK) {
            rc = -1;
            continue;
        }
        const mode_t mode = inode.base.mode & 07777;
        const mode_t type = sqfs_mode(inode.base.inode_type) & S_IFMT;
        /* make sure the parent exists (matters when extracting with a pattern) */
        char parent[PATH_MAX];
        snprintf(parent, sizeof parent, "%s", out);
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = 0;
            mkdirs(parent, 0755);
        }
        if (type == S_IFDIR) {
            if (mkdirs(out, mode | 0700) < 0)
                rc = -1;
        } else if (type == S_IFREG) {
            unlink(out);
            int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode | 0600);
            if (fd < 0) {
                rc = -1;
                continue;
            }
            sqfs_off_t pos = 0, total = (sqfs_off_t)inode.xtra.reg.file_size;
            while (pos < total) {
                sqfs_off_t n = total - pos < (1 << 20) ? total - pos : (1 << 20);
                if (sqfs_read_range(&fs, &inode, pos, &n, buf) != SQFS_OK || n <= 0) {
                    rc = -1;
                    break;
                }
                if (write(fd, buf, (size_t)n) != (ssize_t)n) {
                    rc = -1;
                    break;
                }
                pos += n;
            }
            fchmod(fd, mode);
            close(fd);
        } else if (type == S_IFLNK) {
            char target[PATH_MAX];
            size_t size = sizeof target;
            if (sqfs_readlink(&fs, &inode, target, &size) == SQFS_OK) {
                unlink(out);
                if (symlink(target, out) < 0)
                    rc = -1;
            }
        }
    }
    free(buf);
    sqfs_traverse_close(&trv);
    sqfs_destroy(&fs);
    return err == SQFS_OK ? rc : -1;
}

/* ------------------------------------------------------------------------- mounting */

struct server {
    struct fuse_session *se;
};

static void *serve(void *arg)
{
    struct server *s = arg;
    fuse_session_loop(s->se);
    return NULL;
}

static void set_lowlevel_ops(struct fuse_lowlevel_ops *ops)
{
    memset(ops, 0, sizeof *ops);
    ops->init = sqfs_ll_op_init;
    ops->getattr = sqfs_ll_op_getattr;
    ops->opendir = sqfs_ll_op_opendir;
    ops->releasedir = sqfs_ll_op_releasedir;
    ops->readdir = sqfs_ll_op_readdir;
    ops->lookup = sqfs_ll_op_lookup;
    ops->open = sqfs_ll_op_open;
    ops->create = sqfs_ll_op_create;
    ops->release = sqfs_ll_op_release;
    ops->read = sqfs_ll_op_read;
    ops->readlink = sqfs_ll_op_readlink;
    ops->listxattr = sqfs_ll_op_listxattr;
    ops->getxattr = sqfs_ll_op_getxattr;
    ops->forget = sqfs_ll_op_forget;
    ops->statfs = stfs_ll_op_statfs;
}

/* Waits until process `pid` is gone. Uses a pidfd where available, polling otherwise. */
static void wait_for_exit(pid_t pid, int keepalive_fd)
{
#ifdef SYS_pidfd_open
    int pfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pfd >= 0) {
        struct pollfd p = {pfd, POLLIN, 0};
        for (;;) {
            const int r = poll(&p, 1, 20000);
            if (r > 0)
                break;
            if (r < 0 && errno != EINTR)
                break;
            malloc_trim(0); /* idle: give decompression scratch memory back */
        }
        close(pfd);
        return;
    }
#endif
    (void)keepalive_fd;
    for (int i = 0; kill(pid, 0) == 0 || errno == EPERM; ++i) {
        sleep(1);
        if (i % 20 == 19)
            malloc_trim(0);
    }
}

/*
 * Forks a server process that mounts the image and keeps serving until `owner` exits.
 * Returns 0 once the mount is ready.
 */
static int mount_image(const char *image, off_t offset, const char *mnt, pid_t owner, int quiet)
{
    int ready[2];
    if (pipe2(ready, O_CLOEXEC) < 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        close(ready[0]);
        /* Decompression buffers are large and short-lived: let them go straight back. */
        mallopt(M_ARENA_MAX, 1);
        mallopt(M_MMAP_THRESHOLD, 64 * 1024);
        mallopt(M_TRIM_THRESHOLD, 256 * 1024);
        setsid(); /* Ctrl-C in the terminal goes to the app, not to the file system */
        signal(SIGINT, SIG_IGN);
        signal(SIGHUP, SIG_IGN);
        signal(SIGPIPE, SIG_IGN);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            if (quiet)
                dup2(devnull, 2);
            if (devnull > 2)
                close(devnull);
        }
        sqfs_ll *ll = sqfs_ll_open(image, (size_t)offset);
        if (!ll) {
            if (write(ready[1], "F", 1) < 0) {}
            _exit(1);
        }
        struct fuse_lowlevel_ops ops;
        set_lowlevel_ops(&ops);
        struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
        fuse_opt_add_arg(&args, "ruche-appimage");
        fuse_opt_add_arg(&args, "-o");
        fuse_opt_add_arg(&args, "ro,nodev,nosuid,fsname=ruche.AppImage,subtype=appimage");
        sqfs_ll_chan ch;
        if (sqfs_ll_mount(&ch, mnt, &args, &ops, sizeof ops, ll) != SQFS_OK) {
            if (write(ready[1], "F", 1) < 0) {}
            _exit(1);
        }
        if (write(ready[1], "K", 1) < 0) {}
        close(ready[1]);
        struct server s = {ch.session};
        pthread_t t;
        pthread_create(&t, NULL, serve, &s);
        wait_for_exit(owner, -1);
        fuse_session_exit(ch.session);
        fuse_session_unmount(ch.session);
        pthread_join(t, NULL);
        fuse_session_destroy(ch.session);
        rmdir(mnt);
        _exit(0);
    }
    close(ready[1]);
    char c = 0;
    ssize_t n;
    while ((n = read(ready[0], &c, 1)) < 0 && errno == EINTR)
        ;
    close(ready[0]);
    if (n == 1 && c == 'K')
        return 0;
    waitpid(pid, NULL, 0);
    return -1;
}

/* ------------------------------------------------------------------------- running */

static void run(const char *appdir, const char *self, int argc, char **argv)
{
    char apprun[PATH_MAX], cwd[PATH_MAX];
    snprintf(apprun, sizeof apprun, "%s/AppRun", appdir);
    setenv("APPIMAGE", self, 1);
    setenv("APPDIR", appdir, 1);
    setenv("ARGV0", argv[0], 1);
    if (getcwd(cwd, sizeof cwd))
        setenv("OWD", cwd, 1);
    char **nargv = calloc((size_t)argc + 1, sizeof(char *));
    nargv[0] = apprun;
    for (int i = 1; i < argc; ++i)
        nargv[i] = argv[i];
    execv(apprun, nargv);
    perror("AppImage: cannot run AppRun");
    exit(1);
}

static uint64_t fnv1a(const void *data, size_t n, uint64_t h)
{
    const unsigned char *p = data;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* Extracts once into ~/.cache/ruche-appimage/<id> and reuses it afterwards. */
static void extract_cached_and_run(const char *self, off_t offset, int argc, char **argv)
{
    struct stat st;
    if (stat(self, &st) < 0)
        die("cannot stat itself");
    uint64_t h = 1469598103934665603ULL;
    h = fnv1a(&st.st_size, sizeof st.st_size, h);
    h = fnv1a(&st.st_mtime, sizeof st.st_mtime, h);
    h = fnv1a(&offset, sizeof offset, h);
    const char *cache = getenv("XDG_CACHE_HOME");
    char base[PATH_MAX], dir[PATH_MAX], marker[PATH_MAX], tmp[PATH_MAX];
    if (cache && *cache)
        snprintf(base, sizeof base, "%s/ruche-appimage", cache);
    else
        snprintf(base, sizeof base, "%s/.cache/ruche-appimage", getenv("HOME") ? getenv("HOME") : "/tmp");
    snprintf(dir, sizeof dir, "%s/%016llx", base, (unsigned long long)h);
    snprintf(marker, sizeof marker, "%s/.complete", dir);
    if (access(marker, F_OK) != 0) {
        snprintf(tmp, sizeof tmp, "%s/.tmp-%d", base, (int)getpid());
        if (mkdirs(tmp, 0755) < 0 || extract(self, offset, tmp, NULL) != 0)
            die("FUSE is not available and extracting the AppImage failed");
        char m2[PATH_MAX];
        snprintf(m2, sizeof m2, "%s/.complete", tmp);
        int fd = open(m2, O_WRONLY | O_CREAT, 0644);
        if (fd >= 0)
            close(fd);
        if (rename(tmp, dir) != 0 && access(marker, F_OK) != 0)
            die("cannot move the extracted AppImage into place");
    }
    run(dir, self, argc, argv);
}

int main(int argc, char **argv)
{
    char self[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", self, sizeof self - 1);
    if (len < 0)
        die("cannot find itself");
    self[len] = 0;
    /* An unpacked target from --appimage-extract-and-run in another tool may pass this. */
    const char *target = getenv("TARGET_APPIMAGE");
    if (target && *target)
        snprintf(self, sizeof self, "%s", target);

    const off_t offset = elf_size(self);
    if (offset <= 0)
        die("not an AppImage");

    const char *arg = argc > 1 ? argv[1] : "";
    if (strncmp(arg, "--appimage-", 11) == 0) {
        if (!strcmp(arg, "--appimage-offset")) {
            printf("%lld\n", (long long)offset);
            return 0;
        }
        if (!strcmp(arg, "--appimage-version")) {
            printf("%s\n", RUNTIME_VERSION);
            return 0;
        }
        if (!strcmp(arg, "--appimage-extract")) {
            char dest[PATH_MAX];
            snprintf(dest, sizeof dest, "squashfs-root");
            mkdirs(dest, 0755);
            const int rc = extract(self, offset, dest, argc > 2 ? argv[2] : NULL);
            if (rc == 0)
                printf("Extracted into ./squashfs-root\n");
            return rc == 0 ? 0 : 1;
        }
        if (!strcmp(arg, "--appimage-extract-and-run")) {
            extract_cached_and_run(self, offset, argc - 1, argv + 1);
            return 1;
        }
        if (!strcmp(arg, "--appimage-mount")) {
            char mnt[] = "/tmp/.mount_RucheXXXXXX";
            if (!mkdtemp(mnt) || mount_image(self, offset, mnt, getpid(), 0) != 0)
                die("mounting failed");
            printf("%s\n", mnt);
            fflush(stdout);
            pause(); /* until Ctrl-C */
            return 0;
        }
        printf("Ruche AppImage\n"
               "  --appimage-extract [pattern]  unpack into ./squashfs-root\n"
               "  --appimage-extract-and-run    run without FUSE (unpacks to ~/.cache once)\n"
               "  --appimage-mount              mount and print the mount point\n"
               "  --appimage-offset             print the image offset\n"
               "  --appimage-version            print the runtime version\n");
        return 0;
    }

    if (getenv("APPIMAGE_EXTRACT_AND_RUN") || access("/dev/fuse", R_OK | W_OK) != 0) {
        extract_cached_and_run(self, offset, argc, argv);
        return 1;
    }
    const char *tmpdir = getenv("TMPDIR");
    char mnt[PATH_MAX];
    snprintf(mnt, sizeof mnt, "%s/.mount_RucheXXXXXX", tmpdir && *tmpdir ? tmpdir : "/tmp");
    if (!mkdtemp(mnt) || mount_image(self, offset, mnt, getpid(), 1) != 0) {
        if (mnt[0])
            rmdir(mnt);
        fprintf(stderr, "AppImage: FUSE mount failed, running from an extracted copy instead\n");
        extract_cached_and_run(self, offset, argc, argv);
        return 1;
    }
    run(mnt, self, argc, argv);
    return 1;
}
