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
#include <sys/utsname.h>
#include <sys/syscall.h>
#include <dirent.h>

static inline int finit_module(int fd, const char *params, unsigned int flags) {
    return (int)syscall(__NR_finit_module, fd, params, flags);
}

static void load_boot_modules(void) {
    const char *modules[] = {
        "virtio_ring", "virtio", "virtio_blk",
        "scsi_mod", "sd_mod",
        "nvme", "nvme_core",
        "usbcore", "usb_storage",
        "sr_mod",
        NULL
    };

    struct utsname uts;
    if (uname(&uts) < 0) return;

    char modpath[256];
    snprintf(modpath, sizeof(modpath), "/lib/modules/%s", uts.release);

    for (int i = 0; modules[i]; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s.ko", modpath, modules[i]);
        if (access(path, F_OK) == 0) {
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                if (finit_module(fd, "", 0) != 0)
                    fprintf(stderr, "zenith-heart: insmod %s failed: %s\n",
                            modules[i], strerror(errno));
                close(fd);
            }
        }
    }
}

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

    fprintf(stderr, "zenith-heart: loading boot modules\n");
    load_boot_modules();

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

// --- Emergency Shell ---
// Built-in commands for debugging. No external dependencies.

static void cmd_help(void) {
    fprintf(stderr,
        "emergency shell commands:\n"
        "  ls [path]        list directory\n"
        "  cat <file>       print file contents\n"
        "  dmesg            kernel ring buffer\n"
        "  lsmod            loaded kernel modules\n"
        "  mount            show mounts\n"
        "  block            list block devices\n"
        "  ps               running processes\n"
        "  env              environment variables\n"
        "  uname [-a]       kernel info\n"
        "  insmod <file>    load kernel module\n"
        "  reboot           reboot system\n"
        "  poweroff         power off\n"
        "  exit             return to init\n"
        "  help             this message\n"
    );
}

static void cmd_ls(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : ".";
    DIR *d = opendir(path);
    if (!d) { fprintf(stderr, "ls: %s: %s\n", path, strerror(errno)); return; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL)
        fprintf(stderr, "%s%s%s\n",
            ent->d_name[0] == '.' && ent->d_name[1] == '\0' ? "." :
            ent->d_name[0] == '.' && ent->d_name[1] == '.' && ent->d_name[2] == '\0' ? ".." :
            "", ent->d_name, ent->d_type == DT_DIR ? "/" : "");
    closedir(d);
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "cat: usage: cat <file>\n"); return; }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { fprintf(stderr, "cat: %s: %s\n", argv[1], strerror(errno)); return; }
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(STDERR_FILENO, buf, n);
    close(fd);
}

static void cmd_dmesg(void) {
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
    if (fd < 0) { fprintf(stderr, "dmesg: open /dev/kmsg: %s\n", strerror(errno)); return; }
    lseek(fd, 0, SEEK_DATA);
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\n';
        write(STDERR_FILENO, buf, n + 1);
    }
    close(fd);
}

static void cmd_lsmod(void) {
    int fd = open("/proc/modules", O_RDONLY);
    if (fd < 0) { fprintf(stderr, "lsmod: %s\n", strerror(errno)); return; }
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(STDERR_FILENO, buf, n);
    close(fd);
}

static void cmd_mount(void) {
    int fd = open("/proc/mounts", O_RDONLY);
    if (fd < 0) { fprintf(stderr, "mount: %s\n", strerror(errno)); return; }
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(STDERR_FILENO, buf, n);
    close(fd);
}

static void cmd_block(void) {
    DIR *d = opendir("/sys/block");
    if (!d) { fprintf(stderr, "block: /sys/block: %s\n", strerror(errno)); return; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        fprintf(stderr, "/dev/%s\n", ent->d_name);
    }
    closedir(d);
}

static void cmd_ps(void) {
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *ent;
    fprintf(stderr, "  PID COMMAND\n");
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
        char path[256];
        snprintf(path, sizeof(path), "/proc/%s/comm", ent->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        char comm[256] = "?";
        ssize_t n = read(fd, comm, sizeof(comm) - 1);
        close(fd);
        if (n > 0) { comm[n - 1] = '\0'; if (comm[n - 2] == '\n') comm[n - 2] = '\0'; }
        fprintf(stderr, "%5s %s\n", ent->d_name, comm);
    }
    closedir(d);
}

static void cmd_insmod(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "insmod: usage: insmod <file.ko>\n"); return; }
    int fd = open(argv[1], O_RDONLY | O_CLOEXEC);
    if (fd < 0) { fprintf(stderr, "insmod: %s: %s\n", argv[1], strerror(errno)); return; }
    if (finit_module(fd, "", 0) != 0)
        fprintf(stderr, "insmod: %s: %s\n", argv[1], strerror(errno));
    else
        fprintf(stderr, "insmod: %s loaded\n", argv[1]);
    close(fd);
}

static void emergency_shell(void) {
    fprintf(stderr,
        "\n"
        "=== ZENITH EMERGENCY SHELL ===\n"
        "Type 'help' for commands.\n"
    );

    char line[1024];
    while (1) {
        fprintf(stderr, "zenith# ");
        fflush(stderr);

        ssize_t n = read(STDIN_FILENO, line, sizeof(line) - 1);
        if (n <= 0) break;
        line[n] = '\0';

        // Strip trailing newline
        if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
        if (n > 1 && line[n - 2] == '\r') line[n - 2] = '\0';

        if (line[0] == '\0') continue;

        // Tokenize
        char *argv[64];
        int argc = 0;
        char *tok = line;
        while (*tok && argc < 63) {
            while (*tok == ' ') *tok++ = '\0';
            if (*tok) argv[argc++] = tok;
            while (*tok && *tok != ' ') tok++;
        }
        argv[argc] = NULL;
        if (argc == 0) continue;

        if (strcmp(argv[0], "help") == 0 || strcmp(argv[0], "?") == 0) {
            cmd_help();
        } else if (strcmp(argv[0], "ls") == 0) {
            cmd_ls(argc, argv);
        } else if (strcmp(argv[0], "cat") == 0) {
            cmd_cat(argc, argv);
        } else if (strcmp(argv[0], "dmesg") == 0) {
            cmd_dmesg();
        } else if (strcmp(argv[0], "lsmod") == 0) {
            cmd_lsmod();
        } else if (strcmp(argv[0], "mount") == 0) {
            cmd_mount();
        } else if (strcmp(argv[0], "block") == 0) {
            cmd_block();
        } else if (strcmp(argv[0], "ps") == 0) {
            cmd_ps();
        } else if (strcmp(argv[0], "env") == 0) {
            extern char **environ;
            for (char **e = environ; *e; e++) fprintf(stderr, "%s\n", *e);
        } else if (strcmp(argv[0], "uname") == 0) {
            struct utsname uts;
            if (uname(&uts) == 0)
                fprintf(stderr, "%s %s %s %s %s\n",
                        uts.sysname, uts.nodename, uts.release, uts.version, uts.machine);
        } else if (strcmp(argv[0], "insmod") == 0) {
            cmd_insmod(argc, argv);
        } else if (strcmp(argv[0], "reboot") == 0) {
            reboot(LINUX_REBOOT_CMD_RESTART);
        } else if (strcmp(argv[0], "poweroff") == 0) {
            reboot(LINUX_REBOOT_CMD_POWER_OFF);
        } else if (strcmp(argv[0], "exit") == 0) {
            break;
        } else {
            fprintf(stderr, "%s: unknown command, type 'help'\n", argv[0]);
        }
    }
    fprintf(stderr, "zenith-heart: leaving emergency shell\n");
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

    if (pivoted == 0) {
        fprintf(stderr, "zenith-heart: already pivoted, spawning services\n");

        mount_proc();
        mount_sys();
        mount_dev();

        setup_sanitizers();

        setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin:/usr/libexec", 1);
        setenv("HOME", "/root", 1);

        mkdir("/run", 0755);
        mkdir("/tmp", 0755);
        mkdir("/var/log", 0755);

        fprintf(stderr, "zenith-heart: spawning services\n");

        char *const alsa_argv[] = {"alsactl", "restore", "-f",
                                   "/usr/share/alsa/alsa.conf", NULL};
        spawn_service("alsactl", alsa_argv);

        char *const iwd_argv[] = {"iwd", NULL};
        spawn_service("iwd", iwd_argv);

        char *const llama_argv[] = {"llama-server",
                                    "-m", "/usr/share/models/qwen-1.7b.gguf",
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
    }

    // Pivot failed, drop into emergency shell with /proc /sys /dev
    fprintf(stderr, "zenith-heart: pivot failed, dropping to emergency shell\n");
    mount_proc();
    mount_sys();
    mount_dev();
    emergency_shell();

    _exit(0);
}
