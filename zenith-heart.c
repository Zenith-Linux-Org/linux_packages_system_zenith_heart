#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <linux/reboot.h>
#include <sys/reboot.h>
#include <stdnoreturn.h>
#include <sys/ioctl.h>
#include <linux/loop.h>

static int pivot_to_erofs(void) {
    struct stat st;
    if (stat("/proc/1/exe", &st) == 0) {
        char link[256];
        ssize_t len = readlink("/proc/1/exe", link, sizeof(link) - 1);
        if (len > 0) {
            link[len] = '\0';
            if (strstr(link, "zenith-heart"))
                return 0;
        }
    }

    mkdir("/proc", 0755);
    mount("proc", "/proc", "proc", 0, NULL);
    mkdir("/sys", 0755);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mkdir("/dev", 0755);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
    mkdir("/tmp", 0755);
    mount("tmpfs", "/tmp", "tmpfs", 0, NULL);

    const char *devs[] = {"/dev/sr0", "/dev/sr1", "/dev/vda", "/dev/vdb",
                          "/dev/sda", "/dev/sdb", NULL};
    mkdir("/media", 0755);
    int found = 0;
    for (int i = 0; devs[i]; i++) {
        if (mount(devs[i], "/media", "iso9660", MS_RDONLY, NULL) == 0) {
            found = 1;
            break;
        }
        if (mount(devs[i], "/media", "vfat", MS_RDONLY, NULL) == 0) {
            found = 1;
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "zenith-heart: no boot media found\n");
        return -1;
    }

    FILE *f = fopen("/media/boot/rootfs.erofs", "rb");
    if (!f) {
        fprintf(stderr, "zenith-heart: rootfs.erofs not found on media\n");
        umount("/media");
        return -1;
    }
    fclose(f);

    fprintf(stderr, "zenith-heart: copying rootfs.erofs to tmpfs...\n");
    int in_fd = open("/media/boot/rootfs.erofs", O_RDONLY);
    int out_fd = open("/tmp/rootfs.erofs", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (in_fd < 0 || out_fd < 0) {
        fprintf(stderr, "zenith-heart: failed to open rootfs files\n");
        if (in_fd >= 0) close(in_fd);
        if (out_fd >= 0) close(out_fd);
        umount("/media");
        return -1;
    }
    char buf[65536];
    ssize_t n;
    while ((n = read(in_fd, buf, sizeof(buf))) > 0)
        write(out_fd, buf, n);
    close(in_fd);
    close(out_fd);
    umount("/media");

    mkdir("/newroot", 0755);

    // Set up loop device for erofs image
    int file_fd = open("/tmp/rootfs.erofs", O_RDONLY);
    if (file_fd < 0) {
        fprintf(stderr, "zenith-heart: open rootfs.erofs failed: %s\n", strerror(errno));
        return -1;
    }
    int loop_fd = open("/dev/loop0", O_RDWR);
    if (loop_fd < 0) {
        fprintf(stderr, "zenith-heart: open /dev/loop0 failed: %s\n", strerror(errno));
        close(file_fd);
        return -1;
    }
    if (ioctl(loop_fd, LOOP_SET_FD, file_fd) < 0) {
        fprintf(stderr, "zenith-heart: LOOP_SET_FD failed: %s\n", strerror(errno));
        close(loop_fd);
        close(file_fd);
        return -1;
    }
    close(file_fd);
    close(loop_fd);

    if (mount("/dev/loop0", "/newroot", "erofs", MS_RDONLY, NULL) < 0) {
        fprintf(stderr, "zenith-heart: mount erofs failed: %s\n", strerror(errno));
        return -1;
    }

    mkdir("/newroot/proc", 0755);
    mkdir("/newroot/sys", 0755);
    mkdir("/newroot/dev", 0755);
    mkdir("/newroot/tmp", 0755);

    mount("/proc", "/newroot/proc", "proc", 0, NULL);
    mount("/sys", "/newroot/sys", "sysfs", 0, NULL);
    mount("/dev", "/newroot/dev", "devtmpfs", 0, NULL);
    mount("/tmp", "/newroot/tmp", "tmpfs", 0, NULL);

    if (chroot("/newroot") < 0) {
        fprintf(stderr, "zenith-heart: chroot failed: %s\n", strerror(errno));
        return -1;
    }
    chdir("/");
    return 1;
}

static void mount_proc(void) {
    mkdir("/proc", 0755);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0 && errno != EBUSY)
        fprintf(stderr, "zenith-heart: mount /proc failed: %s\n", strerror(errno));
}

static void mount_sys(void) {
    mkdir("/sys", 0755);
    if (mount("sysfs", "/sys", "sysfs", 0, NULL) < 0 && errno != EBUSY)
        fprintf(stderr, "zenith-heart: mount /sys failed: %s\n", strerror(errno));
}

static void mount_dev(void) {
    mkdir("/dev", 0755);
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) < 0 && errno != EBUSY)
        fprintf(stderr, "zenith-heart: mount /dev failed: %s\n", strerror(errno));
}

// --- Sanitizer environment ---
// Must set BEFORE any dynamic library loads so ASan picks up options.

static void setup_sanitizers(void) {
    setenv("ASAN_OPTIONS",
           "abort_on_error=0:log_path=/var/log/asan.log:detect_leaks=0", 1);
    setenv("UBSAN_OPTIONS",
           "abort_on_error=0:log_path=/var/log/ubsan.log:print_stacktrace=1", 1);
    mkdir("/var/log", 0755);
}

static pid_t spawn_service(const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        execvp(path, argv);
        fprintf(stderr, "zenith-heart: exec %s failed: %s\n", path, strerror(errno));
        _exit(127);
    }
    if (pid < 0)
        fprintf(stderr, "zenith-heart: fork %s failed: %s\n", path, strerror(errno));
    return pid;
}

int main(void) {
    fprintf(stderr, "zenith-heart: PID 1 starting\n");

    int pivoted = pivot_to_erofs();
    if (pivoted > 0) {
        fprintf(stderr, "zenith-heart: pivoted to EROFS root\n");
        execv("/bin/zenith-heart", (char *const[]){"zenith-heart", NULL});
        fprintf(stderr, "zenith-heart: exec self failed: %s\n", strerror(errno));
        _exit(1);
    }

    // ASan needs /proc/self/maps
    mount_proc();
    mount_sys();
    mount_dev();

    setup_sanitizers();

    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin:/usr/libexec", 1);
    setenv("HOME", "/root", 1);

    mkdir("/run", 0755);
    mkdir("/tmp", 0755);
    mkdir("/var/log", 0755);
    mkdir("/usr/bin", 0755);
    mkdir("/usr/lib", 0755);
    mkdir("/usr/share/models", 0755);

    fprintf(stderr, "zenith-heart: spawning services\n");

    char *const alsa_argv[] = {"alsactl", "restore", "-f",
                               "/usr/share/alsa/alsa.conf", NULL};
    spawn_service("alsactl", alsa_argv);

    char *const iwd_argv[] = {"iwd", NULL};
    spawn_service("iwd", iwd_argv);

    char *const llama_argv[] = {"llama-server",
                                "-m", "/usr/share/models/qwen-1.7b-q4_k_m.gguf",
                                "--host", "127.0.0.1",
                                "--port", "8080",
                                "--gpu-layers", "0",
                                "--threads", "4",
                                NULL};
    spawn_service("llama-server", llama_argv);

    fprintf(stderr, "zenith-heart: services launched\n");

    fprintf(stderr, "zenith-heart: starting session\n");

    char *const nanox_argv[] = {"nanox", NULL};
    pid_t session = spawn_service("nanox", nanox_argv);

    if (session < 0) {
        char *const fish_argv[] = {"fish", NULL};
        spawn_service("fish", fish_argv);
    }

    fprintf(stderr, "zenith-heart: entering reap loop\n");
    while (1) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) {
            sleep(1);
            continue;
        }
        if (WIFEXITED(status))
            fprintf(stderr, "zenith-heart: child %d exited %d\n",
                    pid, WEXITSTATUS(status));
        else if (WIFSIGNALED(status))
            fprintf(stderr, "zenith-heart: child %d killed by %d\n",
                    pid, WTERMSIG(status));
    }

    _exit(0);
}
