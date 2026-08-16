#!/bin/sh
# Installed as Emus/<platform>/NETPLAY.pak/launch.sh.  The matching ROM lives
# under a hidden Roms directory, so it is eligible for recent.txt without
# adding a fake console to NextUI's Games list.

: "${SDCARD_PATH:=/mnt/SDCARD}"
: "${PLATFORM:=tg5040}"

NP="$SDCARD_PATH/Tools/$PLATFORM/Netplay.pak"
exec "$NP/launch.sh" --quick
