#!/bin/sh
# Stop the dev-launched game and remove its launchd job. Development only:
# this also kills a game Steam launched.
launchctl bootout "gui/$(id -u)/cai.civ6.dev" 2>/dev/null
pkill -9 -x Civ6_Exe_Child 2>/dev/null
echo "game stopped"
