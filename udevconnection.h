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

        udev_enumerate_unref(enumerate);

        battery.valid = false;
        if (!battery.udevDevice) {
            fprintf(stderr, "Failed to find battery udev device\n");
            return;
        }
        if (!battery.charger) {
            fprintf(stderr, "Failed to find charger device\n");
            return;
        }
        if (!updateBattery()) {
            fprintf(stderr, "Failed to update battery\n");
            return;
        }
        if (!updateCharger()) {
            fprintf(stderr, "Failed to update charger\n");
            return;
        }
        battery.valid = true;
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

    void printProperties(udev_device *dev)
    {
        fprintf(stderr, "action: %s\n", udev_device_get_action(dev));
        fprintf(stderr, "sysname: %s\n", udev_device_get_sysname(dev));
        udev_list_entry *entry = udev_device_get_properties_list_entry(dev);
        while (entry) {
            const char *name = udev_list_entry_get_name(entry);
            const char *value = udev_list_entry_get_value(entry);
            fprintf(stderr,"property name: %s value %s\n", name, value);
            entry = udev_list_entry_get_next(entry);
        }
    }

    bool update()
    {
        if (!udevAvailable) {
            fprintf(stderr, "udev unavailable\n");
            return false;
        }

        udev_device *dev = udev_monitor_receive_device(udevMonitor);

        const char *deviceName = udev_device_get_sysname(dev);
        if (strcmp(deviceName, "BAT0") == 0) {
            if (dev != battery.udevDevice) {
                if (battery.udevDevice) {
                    udev_device_unref(battery.udevDevice);
                }

                battery.udevDevice = dev;
            }
        } else if (strcmp(deviceName, "AC") == 0) {
            if (dev != battery.charger) {
                if (battery.charger) {
                    udev_device_unref(battery.charger);
                }

                battery.charger = dev;
            }
        } else {
            fprintf(stderr, "Unknown power supply device notification %s\n", deviceName);
        }
        battery.valid = battery.udevDevice && updateBattery() && battery.charger && updateCharger();

        return true;
    }

    bool updateBattery()
    {
        if (!udevAvailable) {
            fprintf(stderr, "Udev unavailable, can't update battery!");
            return false;
        }

        if (!battery.udevDevice) {
            fprintf(stderr, "udev battery device not initialized\n");
            return false;
        }

        const char *chargingStatus = udev_device_get_property_value(battery.udevDevice, "POWER_SUPPLY_STATUS");
        if (strcmp(chargingStatus, "Charging") == 0) {
            battery.batteryCharging = true;
        } else if (strcmp(chargingStatus, "Discharging") == 0) {
            battery.batteryCharging = false;
        } else if (strcmp(chargingStatus, "Not charging") == 0) {
            battery.batteryCharging = false;
        } else if (strcmp(chargingStatus, "Full") == 0) {
            battery.batteryCharging = false;
            battery.percentage = 100;
        } else {
            fprintf(stderr, "unknown status %s\n", chargingStatus);
            printProperties(battery.udevDevice);
        }
        const char *capacity = udev_device_get_property_value(battery.udevDevice, "POWER_SUPPLY_CAPACITY");

        battery.percentage = atoi(capacity);

        if (battery.percentage == -1) {
            fprintf(stderr, "Invalid percentage: %s\n", capacity);
            return false;
        }

        return true;
    }

    bool updateCharger() {
        if (!battery.charger) {
            fprintf(stderr, "charger not available?\n");
            return false;
        }

        const char *online = udev_device_get_property_value(battery.charger, "POWER_SUPPLY_ONLINE");
        if (strcmp(online, "1") == 0) {
            battery.chargerOnline = true;
        } else if (strcmp(online, "0") == 0) {
            battery.chargerOnline = false;
        } else {
            fprintf(stderr, "unknown charger online %s\n", online);
        }

        return true;
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
        int percentage = 0;
        unsigned long last_percentage = 100;
        bool valid = false;
    } battery;
};

