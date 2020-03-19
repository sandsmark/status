#pragma once

#include <libudev.h>

#include <algorithm>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>


struct UdevConnection {
    UdevConnection()
    {
        context = udev_new();
        if (!context) {
            fprintf(stderr, "Failed to connect to udev\n");
            return;
        }

        udevMonitor = udev_monitor_new_from_netlink(context, "udev");
        if (!udevMonitor) {
            fprintf(stderr, "Failed to create udev monitor\n");
            return;
        }
        udev_monitor_filter_add_match_subsystem_devtype(udevMonitor, "power_supply", 0);
        udev_monitor_enable_receiving(udevMonitor);
        udevSocketFd = udev_monitor_get_fd(udevMonitor);
        udevAvailable = true;
        fprintf(stderr, "udev initialized\n");

        initBattery();
    }

    void initBattery()
    {
        udev_enumerate* enumerate = udev_enumerate_new(context);

        udev_enumerate_add_match_subsystem(enumerate, "power_supply");
        udev_enumerate_scan_devices(enumerate);

        udev_list_entry *devices = udev_enumerate_get_list_entry(enumerate);
        udev_list_entry *entry = nullptr;

        udev_list_entry_foreach(entry, devices) {
            const char* path = udev_list_entry_get_name(entry);
            if (!path) {
                fprintf(stderr, "Invalid device when listing\n");
                continue;
            }
            udev_device *dev = udev_device_new_from_syspath(context, path);
            if (!dev) {
                fprintf(stderr, "failed getting %s\n", path);
                continue;
            }
            const char *deviceName = udev_device_get_sysname(dev);
            if (strcmp(deviceName, "BAT0") == 0) {
                battery.udevDevice = dev;
            } else if (strcmp(deviceName, "AC") == 0) {
                battery.charger = dev;
            } else {
                fprintf(stderr, "Unknown power supply device %s\n", deviceName);
                udev_device_unref(dev);
            }

        }
        updateBattery();

        udev_enumerate_unref(enumerate);
    }

    ~UdevConnection()
    {
        if (battery.udevDevice) {
            udev_device_unref(battery.udevDevice);
        }
        if (battery.charger) {
            udev_device_unref(battery.charger);
        }
        if (udevMonitor) {
            udev_monitor_unref(udevMonitor);
        }
        if (context) {
            udev_unref(context);
        }
    }

    bool update(fd_set *fdset, const int dbus_fd)
    {
        if (!udevAvailable) {
            fprintf(stderr, "udev unavailable\n");
            return false;
        }

        FD_ZERO(fdset);
        FD_SET(udevSocketFd, fdset);

        if (dbus_fd >= 0) {
            FD_SET(dbus_fd, fdset);
        }

        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 1000000; // 1s

        if (select(std::max(udevSocketFd, dbus_fd) + 1, fdset, 0, 0, &timeout) == -1) {
            if (errno != EINTR) {
                fprintf(stderr, "select failed\n");
                udevAvailable = false;
            }
            fprintf(stderr, "select error: %s %d\n", strerror(errno), errno);
        }

        if (errno == 0 && FD_ISSET(udevSocketFd, fdset)) {
            if (battery.udevDevice) {
                udev_device_unref(battery.udevDevice);
                battery.udevDevice = nullptr;
            }

            // "consume" the event, so we don't get pinged with the same event again
            battery.udevDevice = udev_monitor_receive_device(udevMonitor);
            if (battery.udevDevice) {
                fprintf(stderr, "failed to get battery udev device\n");
            } else {
                fprintf(stderr, "failed to read udev device with event from udevMonitor\n");
                fprintf(stderr, "error: %s %d\n", strerror(errno), errno);
                udevAvailable = false;
            }
        }

        return true;
    }

    bool updateBattery()
    {
        if (!udevAvailable) {
            fprintf(stderr, "Udev unavailable, can't update battery!");
            return false;
        }

        static int udevChargerOnline = -1;
        static int udevBatteryCharging = -1;
        static int udevPercentage = -1;

        if (!battery.udevDevice) {
            fprintf(stderr, "udev battery device not initialized\n");
            return false;
        }

        if (!battery.charger) {
            fprintf(stderr, "udev charger device not initialized\n");
            return false;
        }

        const char *chargingStatus = udev_device_get_property_value(battery.udevDevice, "POWER_SUPPLY_STATUS");
        if (strcmp(chargingStatus, "Charging") == 0) {
            udevBatteryCharging = 1;
        } else if (strcmp(chargingStatus, "Discharging") == 0) {
            udevBatteryCharging = 0;
        } else if (strcmp(chargingStatus, "Not charging") == 0) {
            udevBatteryCharging = 0;
        } else if (strcmp(chargingStatus, "Full") == 0) {
            udevBatteryCharging = 0;
            udevPercentage = 100;
        } else {
            udevBatteryCharging = udevChargerOnline;
            fprintf(stderr, "unknown status %s\n", chargingStatus);
            return false;
        }

        if (battery.preUdevConnected) {
            udevBatteryCharging = !udevBatteryCharging;
        }

        udevPercentage = atoi(udev_device_get_property_value(battery.udevDevice, "POWER_SUPPLY_CAPACITY"));

        const char *online = udev_device_get_property_value(battery.charger, "POWER_SUPPLY_ONLINE");
        if (strcmp(online, "1") == 0) {
            udevChargerOnline = 1;
        } else if (strcmp(online, "0") == 0) {
            udevChargerOnline = 0;
        } else {
            fprintf(stderr, "unknown charger online %s\n", online);
        }

        if (udevChargerOnline != -1 && udevBatteryCharging != -1 && udevPercentage != -1) {
            battery.chargerOnline = udevChargerOnline;
            battery.batteryCharging = udevBatteryCharging; // udev is inverted?
            battery.percentage = udevPercentage;
            return true;
        } else {
            fprintf(stderr, "invalid values, udevChargerOnline: %d, udevBatteryCharging: %d, udevPercentage: %d\n", udevChargerOnline, udevBatteryCharging, udevPercentage);
        }

        return false;
    }

    udev *context = nullptr;
    udev_monitor *udevMonitor = nullptr;

    bool udevAvailable = false;

    int udevSocketFd = -1;

    struct BatteryDevice {
        udev_device *udevDevice = nullptr;
        udev_device *charger = nullptr;

        bool chargerOnline = false;
        bool batteryCharging = false;
        unsigned long percentage = 0;
        bool valid = false;

        bool preUdevConnected = false;
    } battery;

};

