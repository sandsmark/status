status
======

![screenshot](/screenshot.png)

Fairly simple thing to be used with i3bar.

It also automatically suspends my laptop if the battery gets too low (because I
couldn't find anything else that did that nicely, and this already checked my battery).

It also supports displaying notifications, i. e. implements the standard XDG
notification dbus interface used by chromium, firefox, and everything else. Not
enabled by default because I wrote something separate for it:
https://github.com/sandsmark/sandsmark-notificationd

Displays:
 - Battery percentage/charging state (if battery present)
 - Disk space free on all (relevant) partitions)
 - Current network traffic
 - WiFi signal strength (if wlan interface present)
 - Current system load
 - Memory free
 - CPU usage
 - Volume
 - Date (including week number because I always forget that) and time.

Also uses colors to highlight things like quickly rising memory usage, constant
high CPU usage (e. g. when I forgot to stop something running a busyloop) or
low disk space.

Probably not very useful for others, this is mostly for myself.
