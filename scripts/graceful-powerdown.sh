#!/bin/bash

IFACE=eth0
DELAY=2
BROADCAST_TPL="/etc/iboot-agent/broadcast.msg"

GRN='\033[0;32m'
RED='\033[0;31m'
YEL='\033[0;33m'
BLU='\033[0;34m'
CYA='\033[0;36m'
MAG='\033[0;35m'
NC='\033[0m' # No Color


## CHECK IF ROOT IF NOT EXIT

if [[ $EUID -ne 0 ]]; then
    printf "${RED}ERROR: must run as root${NC}\n" >&2
    exit 1
fi


# CHECK IF WE ARE IN A SSH SESSION

if [[ "$SSH_CONNECTION" = "" ]]; then
    printf "CURRENT SESSION: ${GRN}[LOCAL]${NC}\n"
else
    REMOTE_IP=$(echo "$SSH_CONNECTION" | awk '{ print $1 }')
    REMOTE_PORT=$(echo "$SSH_CONNECTION" | awk '{ print $2 }')
    LOCAL_IP=$(echo "$SSH_CONNECTION" | awk '{ print $3 }')
    printf "${CYA}CURRENT SESSION: ${NC}${GRN}[REMOTE]${NC}\n"
    printf "${RED}[$REMOTE_IP]${NC}  ====>>>  ${GRN}[$LOCAL_IP]${NC}\n"
fi


## SEND A TTY MESSAGE TO ALL LOGGED-IN USERS

if [[ -r "$BROADCAST_TPL" ]]; then
    sed -e "s/<HOSTNAME>/$(hostname)/" \
        -e "s/<TIME>/$(date '+%Y-%m-%d %H:%M:%S')/" \
        -e "s/<DELAY_SEC>/${DELAY}s/" \
        "$BROADCAST_TPL" | wall
else
    printf "${RED}WARNING: template not found: ${BROADCAST_TPL}${NC}\n" >&2
    echo "System shutting down in ${DELAY}s" | wall
fi

echo "will shutdown in ${DELAY}s"

setsid bash -c "sleep $DELAY; /usr/sbin/shutdown -h now" > /tmp/sysdown.log 2>&1 < /dev/null &

exit 0
