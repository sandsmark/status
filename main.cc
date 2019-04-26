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
#include <libudev.h>

#include "pulse.h"

// TODO automatically get the correct interface
#define WLAN_INTERFACE "wlan0"

// number of samples to average
// setting it to 1 disables averaging
const unsigned net_samples = 5;
static_assert(net_samples > 1, "net_samples must be greater than 0");

const unsigned mem_samples = 5;
static_assert(mem_samples > 1, "mem_samples must be greater than 0");

inline void print_sep() {
    printf("\""
           "  },"
           "  {   \"full_text\": \"");
}

inline void print_gray()
{
    printf("\", \"color\": \"#aaaaaa");
}

inline void print_red()
{
    printf("\", \"color\": \"#ff9999");
}

inline void print_yellow()
{
    printf("\", \"color\": \"#ffff00");
}

inline void print_green()
{
    printf("\", \"color\": \"#00ff00");
}

static void print_disk_info(const char *path) {
    struct statvfs buf;
    if (statvfs(path, &buf) == -1) {
        fprintf(stderr, "error running statvfs on %s: %s\n", path, strerror(errno));
        return;
    }
    double gb_free = (double)buf.f_bavail * (double)buf.f_bsize / 1000000000.0;
    printf("%s: %.1fGB free", path, gb_free);
    if (gb_free < 1) {
        print_red();
    } else if (gb_free < 5) {
        print_yellow();
    } else {
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

static bool g_udevAvailable = false;

static void print_battery(udev_device *udevDevice)
{
    bool chargerOnline = false;
    bool batteryCharging = false;
    unsigned long percentage = 0;
    bool udevValuesValid = false;

    static bool preUdevConnected = false;

    if (g_udevAvailable) {
        static int udevChargerOnline = -1;
        static int udevBatteryCharging = -1;
        static int udevPercentage = -1;

        if (udevDevice) {
            //printf("action: %s\n", udev_device_get_action(udevDevice));
            //printf("sysname: %s\n", udev_device_get_sysname(udevDevice));
            //{
            //    udev_list_entry *entry = udev_device_get_properties_list_entry(udevDevice);
            //    while (entry) {
            //        const char *name = udev_list_entry_get_name(entry);
            //        const char *value = udev_list_entry_get_value(entry);
            //        printf("property name: %s value %s\n", name, value);
            //        entry = udev_list_entry_get_next(entry);
            //    }
            //}

            const char *deviceName = udev_device_get_sysname(udevDevice);
            if (strcmp(deviceName, "BAT0") == 0) {
                const char *chargingStatus = udev_device_get_property_value(udevDevice, "POWER_SUPPLY_STATUS");
                if (strcmp(chargingStatus, "Charging") == 0) {
                    udevBatteryCharging = 1;
                } else if (strcmp(chargingStatus, "Discharging") == 0) {
                    udevBatteryCharging = 0;
                } else {
                    udevBatteryCharging = udevChargerOnline;
                    fprintf(stderr, "unknown status %s\n", chargingStatus);
                }
                if (preUdevConnected) {
                    udevBatteryCharging = !udevBatteryCharging;
                }

                udevPercentage = atoi(udev_device_get_property_value(udevDevice, "POWER_SUPPLY_CAPACITY"));
            } else if (strcmp(deviceName, "AC") == 0) {
                const char *online = udev_device_get_property_value(udevDevice, "POWER_SUPPLY_ONLINE");
                if (strcmp(online, "1") == 0) {
                    udevChargerOnline = 1;
                } else if (strcmp(online, "0") == 0) {
                    udevChargerOnline = 0;
                } else {
                    fprintf(stderr, "unknown charger online %s\n", online);
                }
            }
        }

        if (udevChargerOnline != -1 && udevBatteryCharging != -1 && udevPercentage != -1) {
            chargerOnline = udevChargerOnline;
            batteryCharging = udevBatteryCharging; // udev is inverted?
            percentage = udevPercentage;
            udevValuesValid = true;
        }
    }
    bool isCharging = batteryCharging;

    if (!udevValuesValid) {
        //fprintf(stderr, "udev invalid\n");
        FILE *file = fopen("/sys/class/power_supply/BAT0/energy_now", "r");
        if (!file) {
            printf("failed to open file for battery");
            return;
        }
        unsigned long energy_now;
        fscanf(file, "%lu", &energy_now);
        fclose(file);
        file = fopen("/sys/class/power_supply/BAT0/energy_full", "r");
        unsigned long energy_full;
        fscanf(file, "%lu", &energy_full);
        fclose(file);

        percentage = energy_now * 100 / energy_full;

        file = fopen("/sys/class/power_supply/BAT0/status", "rw");
        char *line = nullptr;
        size_t len;
        getline(&line, &len, file);
        fclose(file);
        if (strcmp(line, "Charging\n") == 0) {
            isCharging = true;
            preUdevConnected = true;
        }
        free(line);
    }

    //if (percentage > 97) {
    //    return;
    //}
    //}

    if (chargerOnline && percentage > 97) {
        return;
    }

    if (isCharging) {
        printf("charging: %lu%%", percentage);
        print_gray();
    } else {
        static unsigned long last_percentage = 100;
        if (last_percentage < 100 && percentage < last_percentage && percentage < 5) {
            do_suspend();
        }
        last_percentage = percentage;

        printf("bat: %lu%%", percentage);
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

    print_sep();
}

static unsigned s_cpu_high_seconds = 0;

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

    // Show feedback if CPU is pegged
    if (percent > 20) {
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
static void print_wifi_strength() {
    {
        FILE *fp = fopen("/sys/class/net/" WLAN_INTERFACE "/carrier", "r");
        if (!fp) {
            printf("Unable to get carrier status for wifi");
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

    for (size_t len = 0; getline(&ln, &len, fp) != -1;) {
        if (sscanf(ln, " " WLAN_INTERFACE ": %*u %d. %*f %*d %*u %*u %*u %*u %*u %*u",
                   &strength) == 1) {
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

    print_sep();

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
    time(&now);
    char buf[sizeof "week 43 Fri 2015-10-30 12:44:52"];
    if (offset) {
        now += offset;
        strftime(buf, sizeof buf, "%H:%M", localtime(&now));
        fputs(buf, stdout);
        print_gray();
    } else {
        strftime(buf, sizeof buf, "week %V %a %F", localtime(&now));
        fputs(buf, stdout);
        print_gray();
        print_sep();
        strftime(buf, sizeof buf, "%T", localtime(&now));
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

static bool g_running = true;

int main() {
    int udevSocketFd = -1;
    udev *udevConnection = udev_new();
    udev_monitor *udevMonitor = nullptr;
    if (udevConnection) {
        udevMonitor = udev_monitor_new_from_netlink(udevConnection, "udev");
    } else {
        fprintf(stderr, "Failed to connect to udev\n");
    }

    if (udevMonitor) {
        udev_monitor_filter_add_match_subsystem_devtype(udevMonitor, "power_supply", 0);
        udev_monitor_enable_receiving(udevMonitor);
        udevSocketFd = udev_monitor_get_fd(udevMonitor);
        g_udevAvailable = true;
    } else {
        fprintf(stderr, "Failed to create udev monitor\n");
    }

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

    udev_device *udevDevice = nullptr;
    while (g_running) {
        printf(" [ { \"full_text\": \"");
        print_battery(udevDevice);
        print_disk_info("/");
        print_sep();
        print_disk_info("/");
        print_net_usage("enp0s31f6");
        print_sep();
        if (print_net_usage(WLAN_INTERFACE)) {
            print_sep();
        }
        print_wifi_strength();
        print_sep();
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

        if (udevDevice) {
            udev_device_unref(udevDevice);
            udevDevice = nullptr;
        }

        if (g_udevAvailable) {
            fd_set fdset;
            FD_ZERO(&fdset);
            FD_SET(udevSocketFd, &fdset);

            timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 1000000; // 1s

            if (select(udevSocketFd + 1, &fdset, 0, 0, &timeout) == -1) {
                if (errno != EINTR) {
                    perror("select failed");
                    g_udevAvailable = false;
                }
                fprintf(stderr, "select error: %s %d\n", strerror(errno), errno);
            }

            if (errno == 0 && FD_ISSET(udevSocketFd, &fdset)) {
                // "consume" the event, so we don't get pinged with the same event again
                udevDevice = udev_monitor_receive_device(udevMonitor);
                if (udevDevice) {
                } else {
                    perror("failed to read udev device with event from udevMonitor");
                    fprintf(stderr, "error: %s %d\n", strerror(errno), errno);
                    g_udevAvailable = false;
                }
            }
        } else { // fallback if udev is unavailable
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

    if (udevMonitor) {
        udev_monitor_unref(udevMonitor);
    }
    if (udevConnection) {
        udev_unref(udevConnection);
    }
}
