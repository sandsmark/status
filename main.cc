#include "udevconnection.h"

#ifdef ENABLE_NOTIFICATIONS
#include "notifications.h"
#endif

#include "json_helpers.h"

#include <err.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unordered_map>
#include <unordered_set>
#include <systemd/sd-bus.h>
#include <sys/sysinfo.h>
#include <mntent.h>

#include "pulse.h"

// number of samples to average
// setting it to 1 disables averaging
const unsigned net_samples = 5;
static_assert(net_samples > 1, "net_samples must be greater than 0");

const unsigned mem_samples = 5;
static_assert(mem_samples > 1, "mem_samples must be greater than 0");

static bool g_running = true;

static void print_disk_info(const char *path) {
    struct statvfs buf;
    if (statvfs(path, &buf) == -1) {
        fprintf(stderr, "error running statvfs on %s: %s\n", path, strerror(errno));
        return;
    }
    double gb_free = (double)buf.f_bavail * (double)buf.f_bsize / 1000000000.0;
    if (gb_free < 1) {
        printf("%s %.1f GB", path, gb_free);
        print_red();
    } else if (gb_free < 5) {
        printf("%s %.1f GB", path, gb_free);
        print_yellow();
    } else {
        printf("%s %.0f GB", path, gb_free);
        print_gray();
    }
}

static void do_suspend()
{
    sd_bus *bus = nullptr;
    int ret = sd_bus_open_system(&bus);
    if (ret < 0) {
        fprintf(stderr, "Failed to connect to system bus: %s", strerror(-ret));
        return;
    }

    sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *dbusRet = nullptr;
	ret = sd_bus_call_method(bus,
			"org.freedesktop.login1",           /* service to contact */
			"/org/freedesktop/login1",          /* object path */
			"org.freedesktop.login1.Manager",   /* interface name */
			"Suspend",                          /* method name */
			&error,                               /* object to return error in */
			&dbusRet,                                   /* return message on success */
			"b",                                 /* input signature */
			"true");                       /* first argument */

    if (ret < 0) {
        fprintf(stderr, "Failed to issue method call: %s\n", error.message);
    }

    sd_bus_error_free(&error);
    sd_bus_message_unref(dbusRet);
    sd_bus_unref(bus);
}


static void print_battery(UdevConnection *udevConnection)
{
    if (!udevConnection->power.valid) {
        fprintf(stderr, "udev invalid, failed to get battery\n");
        return;
    }

    const bool chargerOnline = udevConnection->power.chargerOnline;

    FILE *file = fopen("/sys/class/power_supply/BAT0/capacity", "r");
    if (!file) {
        puts("failed to open file for battery");
        return;
    }
    int percentage = -1;
    if (fscanf(file, "%d", &percentage) != 1) {
        puts("Failed to read battery capacity");
        fclose(file);
        return;
    }
    fclose(file);

    const bool batteryCharging = udevConnection->power.batteryCharging;

    const bool charging = chargerOnline && batteryCharging;

    if (charging && percentage > 97) {
        return;
    }

    if (charging) {
        printf("charging: %d%%", percentage);
        print_gray();
        return;
    }

    const int last_percentage = udevConnection->power.last_percentage;
    if (last_percentage < 100 && percentage < last_percentage && percentage < 5) {
        do_suspend();
    }
    udevConnection->power.last_percentage = percentage;

    printf("bat: %d%%", percentage);
    if (percentage < 10) {
        print_red();
    } else if (percentage < 20) {
        print_yellow();
    } else if (percentage < 30) {
        print_green();
    } else if (percentage > 90) {
        print_gray();
    }
}

static unsigned s_cpu_high_seconds = 0;
static unsigned s_cpu_count = 1;

static void print_cpu()
{
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        printf("cpu: error opening /proc/stat: %s\n", strerror(errno));
        return;
    }
    unsigned user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
    if (!fscanf(fp, "cpu %u %u %u %u %u %u %u %u %u %u",
                &user, &nice, &system, &idle, &iowait,
                &irq, &softirq, &steal, &guest, &guest_nice)) {
        fclose(fp);
        printf("cpu usage error");
        return;
    }
    fclose(fp);

    idle += iowait;
    unsigned nonidle = user + nice + system + irq + softirq + steal;
    static unsigned previdle = 0, prevnonidle = 0;
    const unsigned percent = (nonidle - prevnonidle) * 100.0 / (idle + nonidle - previdle - prevnonidle);

    printf("cpu: %3u%%", percent);
    previdle = idle;
    prevnonidle = nonidle;

    // Show feedback if CPU (core) is pegged
    // Approximate core thing, but it works (and is much simpler than parsing
    // the entire /proc/stat)
    if (percent * s_cpu_count > 80) {
        s_cpu_high_seconds++;
    } else {
        s_cpu_high_seconds = 0;
    }

    if (s_cpu_high_seconds > 120) {
        print_red();
    } else if (s_cpu_high_seconds > 30) {
        print_yellow();
    } else {
        print_gray();
    }

}

static void print_load() {
    double loadavg;
    if (getloadavg(&loadavg, 1) == -1) {
        fputs("load: error", stdout);
        return;
    }

    printf("load: %1.2f", loadavg);
    // Only print high load if CPU is not attracting attention
    if (loadavg > 2 && s_cpu_high_seconds < 30) {
        print_yellow();
    } else if (loadavg < 1) {
        print_gray();
    }
}

static void print_wifi_strength(const std::string &interface) {
    {
        FILE *fp = fopen(("/sys/class/net/" + interface + "/carrier").c_str(), "r");
        if (!fp) {
            printf("Unable to get carrier status for wifi");
            print_sep();
            return;
        }

        char *line = nullptr;
        size_t len;
        getline(&line, &len, fp);
        fclose(fp);
        if (strcmp(line, "0\n") == 0) {
            printf("wifi down");
            print_red();
            free(line);
            return;
        }
        free(line);
    }

    FILE *fp = fopen("/proc/net/wireless", "r");
    if (!fp) {
        printf("wifi: error opening /proc/net/wireless: %s\n", strerror(errno));
        return;
    }

    char *ln = nullptr;
    int strength = -1.0;

    const std::string matchString = " " + interface + ": %*u %d. %*f %*d %*u %*u %*u %*u %*u %*u";
    for (size_t len = 0; getline(&ln, &len, fp) != -1;) {
        if (sscanf(ln, matchString.c_str(), &strength) == 1) {
            break;
        }
    }
    free(ln);
    fclose(fp);

    if (strength < 0) {
        printf("wifi down");
        print_red();
    } else {
        printf("wifi: %3d%%", strength * 100 / 70);
        if (strength > 30) {
            print_gray();
        }
    }
}

static bool print_net_usage(const std::string &device) {
    static std::unordered_set<std::string> inited;

    {
        FILE *fp = fopen(("/sys/class/net/" + device + "/carrier").c_str(), "r");
        if (!fp) {
            inited.erase(device);
            return false;
        }

        const char status = getc(fp);
        fclose(fp);
        if (status != '1') {
            inited.erase(device);
            return false;
        }
    }

    FILE *fp = fopen("/proc/net/dev", "r");
    if (!fp) {
        inited.erase(device);
        return false;
    }

    static std::unordered_map<std::string, std::array<unsigned long, 1 + net_samples>> rx_map;
    static std::unordered_map<std::string, std::array<unsigned long, 1 + net_samples>> tx_map;
    size_t *rx = rx_map[device].data();
    size_t *tx = tx_map[device].data();

    char *ln = nullptr;
    for (size_t len = 0; getline(&ln, &len, fp) != -1;) {
        if (sscanf(ln, (" " + device + ": %lu %*u %*u %*u %*u %*u %*u %*u %lu").c_str(),
                   &rx[net_samples], &tx[net_samples]) == 2) {
            break;
        }
    }
    free(ln);
    fclose(fp);

    if (inited.find(device) == inited.end()) {
        for (unsigned i = 0; i < net_samples; i++) {
            rx[i] = rx[net_samples];
            tx[i] = tx[net_samples];
        }
        inited.insert(device);
    }

    unsigned long rx_delta = 0;
    unsigned long tx_delta = 0;
    for (unsigned i = 0; i < net_samples; i++) {
        rx_delta += rx[i + 1] - rx[i];
        tx_delta += tx[i + 1] - tx[i];
    }
    rx_delta /= net_samples + 1;
    tx_delta /= net_samples + 1;

    rx_delta /= 1024;
    tx_delta /= 1024;

    if (rx_delta > 100) {
        printf("rx: %5.1fmb ", rx_delta/1024.);
    } else {
        printf("rx: %5lukb ", rx_delta);
    }

    if (tx_delta > 100) {
        printf("tx: %5.1fmb", tx_delta / 1024.);
    } else {
        printf("tx: %5lukb", tx_delta);
    }

    if (rx_delta < 512 && tx_delta < 512) {
        print_gray();
    }

    memmove(rx, rx + 1, sizeof rx[0] * net_samples);
    memmove(tx, tx + 1, sizeof tx[0] * net_samples);

    return true;
}

static void print_mem() {
    FILE *fp = fopen("/proc/meminfo", "r");
    if (!fp) {
        printf("mem: error opening /proc/meminfo: %s\n", strerror(errno));
        return;
    }

    char *line = nullptr;
    unsigned long memtotal = 0, memavailable = 0;

    for (size_t len = 0; getline(&line, &len, fp) != -1;) {
        sscanf(line, "MemTotal: %lu kB", &memtotal);
        sscanf(line, "MemAvailable: %lu kB", &memavailable);
    }
    free(line);

    const long used = memtotal - memavailable;

    static long last_used[1 + mem_samples];

    static bool first_run = true;
    if (first_run) {
        for (unsigned i = 0; i < mem_samples; i++) {
            last_used[i] = used;
        }
        first_run = false;
    }

    long accum = used;
    for (unsigned i=0; i<mem_samples; i++) {
        accum += last_used[i];
    }
    accum /= mem_samples + 1;

    last_used[mem_samples] = used;
    memmove(last_used, last_used + 1, sizeof last_used[0] * mem_samples);

    double percentage = used * 100.0 / memtotal;
    printf("mem: %.0f%%", percentage);

    if (percentage > 80 || used - accum > 1024 * 512) {
        print_red();
    } else if (percentage < 40) {
        print_gray();
    }

    fclose(fp);
}

static void print_time(time_t offset = 0) {
    time_t now;
    tm result;

    time(&now);
    char buf[sizeof "week 43 Fri 2015-10-30 12:44:52"];
    if (offset) {
        now += offset;
        strftime(buf, sizeof buf, "%H:%M", localtime_r(&now, &result));
        fputs(buf, stdout);
        print_gray();
    } else {
        strftime(buf, sizeof buf, "week %V %a %F", localtime_r(&now, &result));
        fputs(buf, stdout);
        print_gray();
        print_sep();
        strftime(buf, sizeof buf, "%T", localtime_r(&now, &result));
        fputs(buf, stdout);
    }
}

static void print_volume(PulseClient &client)
{
    client.Populate();
    const Sink *device = client.GetDefaultSink();
    if (!device) {
        printf("couldn't find default sink");
        print_red();
        return;
    }

    printf("vol: %3d%%", device->Volume());

    if (device->Muted()) {
        print_gray();
    } else {
        print_green();
    }
}

static std::vector<std::string> getPartitions()
{
    FILE *file = setmntent("/proc/mounts", "r");
    if (file == NULL) {
        fprintf(stderr, "Failed to open /proc/mounts");
        return {};
    }

    std::vector<std::string> partitions;
    mntent *ent = nullptr;
    while ((ent = getmntent(file))) {
        if (std::string(ent->mnt_type) != "ext4") {
            continue;
        }
        partitions.push_back(ent->mnt_dir);
    }
    endmntent(file);

    return partitions;
}

int main()
{
    UdevConnection udevConnection;

    const std::vector<std::string> mountPoints = getPartitions();

    sd_bus_slot *slot = nullptr;
    sd_bus *bus = nullptr;
    int dbus_fd = -1;

#ifdef ENABLE_NOTIFICATIONS
    int ret = sd_bus_default_user(&bus);
    if (ret < 0) {
        fprintf(stderr, "Failed to connect to system bus: %s\n", strerror(-ret));
        bus = nullptr;
    } else if (register_notification_service(bus, &slot)) {
        dbus_fd = sd_bus_get_fd(bus);
    } else {
        fprintf(stderr, "Not using notifications\n");
    }
#endif
    s_cpu_count = get_nprocs();

    PulseClient client("status");

    struct sigaction sa = {};
    sa.sa_handler = [](int) {
        fputs("received SIGPIPE, exiting\n", stderr);
        exit(1);
    };
    sigaction(SIGPIPE, &sa, nullptr);

    // Can't capture here, hence global static
    sa.sa_handler = [](int) {
        g_running = false;
    };
    sigaction(SIGINT, &sa, nullptr);

    printf("{ \"version\": 1 }\n[\n");

    while (g_running) {
        printf(" [ { \"full_text\": \"");

#ifdef ENABLE_NOTIFICATIONS
        if (!g_notifications.empty()) {
            print_notification(&g_notifications.front());
            print_sep();
        }
#endif

        if (udevConnection.power.valid) {
            print_battery(&udevConnection);
            print_sep();
        }

        for (const std::string &partition : mountPoints) {
            print_disk_info(partition.c_str());
            print_sep();
        }

        for (const std::string &dev : udevConnection.ethernetInterfaces) {
            print_net_usage(dev);
            print_sep();
        }

        for (const std::string &dev : udevConnection.wlanInterfaces) {
            print_net_usage(dev);
            print_sep();
        }

        for (const std::string &dev : udevConnection.wlanInterfaces) {
            print_wifi_strength(dev);
            print_sep();
        }

        print_load();
        print_sep();
        print_mem();
        print_sep();
        print_cpu();
        print_sep();
        print_volume(client);
        print_sep();
        print_time();

        printf("\" } ],\n");
        fflush(stdout);

        fd_set fdset;
        if (udevConnection.udevAvailable || dbus_fd >= 0) {
            FD_ZERO(&fdset);
            FD_SET(udevConnection.udevSocketFd, &fdset);

            if (dbus_fd >= 0) {
                FD_SET(dbus_fd, &fdset);
            }

            timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 1000000; // 1s
            const bool success = (select(std::max(udevConnection.udevSocketFd, dbus_fd) + 1, &fdset, 0, 0, &timeout) != -1) && (errno == 0);
            if (success && FD_ISSET(udevConnection.udevSocketFd, &fdset)) {
                udevConnection.update();
            }

#ifdef ENABLE_NOTIFICATIONS
            if (dbus_fd >= 0 && FD_ISSET(dbus_fd, &fdset)) {
                fprintf(stderr, "processing dbus\n");
                process_bus(bus);
            }
#endif
        } else {
            fprintf(stderr, "udev and dbus unavailable!\n");

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

#ifdef ENABLE_NOTIFICATIONS
        if (!g_notifications.empty()) {
            if (g_notifications.front().timeout-- <= 0) {
                g_notifications.erase(g_notifications.begin());
            }
        }
#endif
    }

    if (slot) {
        sd_bus_slot_unref(slot);
    }
    if (bus) {
        sd_bus_unref(bus);
    }
}
