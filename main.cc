#include <err.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>

#include "pulse.h"

// number of samples to average
// setting it to 1 disables averaging
const unsigned net_samples = 2;
static_assert(net_samples > 1, "net_samples must be greater than 0");

static void print_disk_info(const char *path) {
    struct statvfs buf;
    if (statvfs(path, &buf) == -1) {
        fprintf(stderr, "error running statvfs on %s: %s\n", path, strerror(errno));
        return;
    }
    fsblkcnt_t used = buf.f_blocks - buf.f_bavail;
    printf(" %s: %.0f%%", path, used * 100.0 / buf.f_blocks);
}

static void print_battery() {
    FILE *file = fopen("/sys/class/power_supply/BAT0/energy_now", "r");
    unsigned long energy_now;
    fscanf(file, "%lu", &energy_now);
    fclose(file);
    file = fopen("/sys/class/power_supply/BAT0/energy_full", "r");
    unsigned long energy_full;
    fscanf(file, "%lu", &energy_full);
    fclose(file);

    unsigned long percentage = energy_now * 100 / energy_full;
    file = fopen("/sys/class/power_supply/BAT0/status", "rw");
    char *line = nullptr;
    size_t len;
    getline(&line, &len, file);
    fclose(file);
    const char *icon;
    if (strcmp(line, "Charging") == 0) {
            icon = "";
    } else {
        if (percentage < 10) {
            icon = "";
        } else if (percentage < 50) {
            icon = "";
        } else if (percentage < 90) {
            icon = "";
        } else {
            icon = "";
        }
    }
    free(line);

    printf("%s: %lu%%", icon, percentage);
}

static void print_net_usage() {
    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) {
        printf("net: error opening /proc/net/dev: %m");
        return;
    }

    static unsigned rx[1 + net_samples];
    static unsigned tx[1 + net_samples];

    char *ln = nullptr;
    for (size_t len = 0; getline(&ln, &len, fp) != 1;) {
        if (sscanf(ln, " wlp4s0: %u %*u %*u %*u %*u %*u %*u %*u %u",
                   &rx[net_samples], &tx[net_samples]) == 2) {
            // switch to kB
            rx[net_samples] /= 1000;
            tx[net_samples] /= 1000;
            break;
        }
    }
    free(ln);
    fclose(fp);

    static bool first_run = true;
    if (first_run) {
        for (unsigned i = 0; i < net_samples; i++) {
            rx[i] = rx[net_samples];
            tx[i] = tx[net_samples];
        }
        first_run = false;
    }

    unsigned rx_delta = 0;
    unsigned tx_delta = 0;
    for (unsigned i = 0; i < net_samples; i++) {
        rx_delta += rx[i + 1] - rx[i];
        tx_delta += tx[i + 1] - tx[i];
    }

    printf(": %4u↓ %4u↑", rx_delta / net_samples, tx_delta / net_samples);

    memmove(rx, rx + 1, sizeof rx[0] * net_samples);
    memmove(tx, tx + 1, sizeof tx[0] * net_samples);
}

static void print_load() {
    double loadavg;
    if (getloadavg(&loadavg, 1) == -1) {
        fputs("load: error", stdout);
        return;
    }
    printf(": %1.2f", loadavg);
}

static void print_mem() {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) {
        printf("mem: error opening /proc/meminfo: %m");
        return;
    }

    char line[256];
    unsigned long memtotal = 0, memavailable = 0;

    while (fgets(line, sizeof line, fp)) {
        sscanf(line, "MemTotal: %lu kB", &memtotal);
        sscanf(line, "MemAvailable: %lu kB", &memavailable);
    }

    unsigned long used = memtotal - memavailable;
    printf(": %.0f%%", used * 100.0 / memtotal);

    fclose(fp);
}

static void print_time() {
    time_t now;
    time(&now);
    char buf[sizeof "2011-10-08 07:07:09"];
    strftime(buf, sizeof buf, "%F %T", localtime(&now));
    fputs(buf, stdout);
}

static void print_sep() {
    fputs(" | ", stdout);
}

static void print_volume(PulseClient &client) {
    client.Populate();

    ServerInfo defaults = client.GetDefaults();
    Device *device = client.GetSink(defaults.sink);
    if (!device) errx(1, "no match found for device: %s", defaults.sink.c_str());

    int volume = device->Volume();

    const char *symbol = "🔊";

    if (device->Muted()) {
        symbol = "🔇";
    } else if (volume <= 33) {
        symbol = "🔈";
    } else if (volume <= 67) {
        symbol = "🔉";
    }

    printf("%s  %d%%", symbol, device->Volume());
}

int main() {
    PulseClient client("status");

    struct sigaction sa = {};
    sa.sa_handler = [](int) {
        fputs("received SIGPIPE, exiting\n", stderr);
        exit(1);
    };
    sigaction(SIGPIPE, &sa, nullptr);

    for (unsigned i = 0;; i++) {
        print_battery();
        print_sep();
        print_disk_info("/boot");
        print_sep();
        print_disk_info("/");
        print_sep();
        print_net_usage();
        print_sep();
        print_load();
        print_sep();
        print_mem();
        print_sep();
        print_volume(client);
        print_sep();
        print_time();
        putchar('\n');
        fflush(stdout);

        // sleep until the next second
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
            perror("clock_gettime");
            return 1;
        }
        ts.tv_sec++;
        if (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == -1) {
            perror("clock_nanosleep");
            return 1;
        }
    }
}
