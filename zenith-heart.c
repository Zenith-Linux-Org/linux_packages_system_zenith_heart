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

// --- Mount early filesystems ---

static void mount_proc(void) {
    mkdir("/proc", 0755);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        fprintf(stderr, "zenith-heart: mount /proc failed: %s\n", strerror(errno));
}

static void mount_sys(void) {
    mkdir("/sys", 0755);
    if (mount("sysfs", "/sys", "sysfs", 0, NULL) < 0)
        fprintf(stderr, "zenith-heart: mount /sys failed: %s\n", strerror(errno));
}

static void mount_dev(void) {
    mkdir("/dev", 0755);
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) < 0)
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

// --- Service spawning ---

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

__attribute__((unused)) static void wait_for(pid_t pid, const char *name) {
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
        fprintf(stderr, "zenith-heart: %s exited %d\n", name, WEXITSTATUS(status));
}

// --- Main init sequence ---

int main(void) {
    // PID 1 — never exit
    fprintf(stderr, "zenith-heart: PID 1 starting\n");

    // Phase 1: early mounts (ASan needs /proc/self/maps)
    mount_proc();
    mount_sys();
    mount_dev();

    // Phase 2: sanitizer env
    setup_sanitizers();

    // Phase 3: create required dirs
    mkdir("/run", 0755);
    mkdir("/tmp", 0755);
    mkdir("/var/log", 0755);
    mkdir("/usr/bin", 0755);
    mkdir("/usr/lib", 0755);
    mkdir("/usr/share/models", 0755);

    // Phase 4: spawn system services
    fprintf(stderr, "zenith-heart: spawning services\n");

    // ALSA — load sound modules
    char *const alsa_argv[] = {"alsactl", "restore", "-f",
                               "/usr/share/alsa/alsa.conf", NULL};
    spawn_service("alsactl", alsa_argv);

    // iwd — wireless daemon
    char *const iwd_argv[] = {"iwd", NULL};
    spawn_service("iwd", iwd_argv);

    // wpa_supplicant (alternative to iwd)
    // char *const wpa_argv[] = {"wpa_supplicant", "-i", "wlan0",
    //                          "-c", "/etc/wpa_supplicant.conf", NULL};
    // spawn_service("wpa_supplicant", wpa_argv);

    // llama-server — AI daemon (GPU probe first)
    char *const llama_argv[] = {"llama-server",
                                "-m", "/usr/share/models/qwen-1.7b-q4_k_m.gguf",
                                "--host", "127.0.0.1",
                                "--port", "8080",
                                "--gpu-layers", "0",  // default CPU
                                "--threads", "4",
                                NULL};
    spawn_service("llama-server", llama_argv);

    fprintf(stderr, "zenith-heart: services launched\n");

    // Phase 5: graphical session
    // Try nanox first, fall back to fish shell
    fprintf(stderr, "zenith-heart: starting session\n");

    char *const nanox_argv[] = {"nanox", NULL};
    pid_t session = spawn_service("nanox", nanox_argv);

    // If nanox fails, start fish shell
    if (session < 0) {
        char *const fish_argv[] = {"fish", NULL};
        spawn_service("fish", fish_argv);
    }

    // Phase 6: PID 1 reap loop — never exit
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

    return 0;  // unreachable
}
