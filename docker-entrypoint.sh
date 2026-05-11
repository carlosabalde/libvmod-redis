#!/usr/bin/env bash

set -euo pipefail
IFS=$'\n\t'

mkdir -p /mnt/host

# Beware 'privileged: true' is required for this.
bindfs \
    --force-user=$(id -u dev) \
    --force-group=$(id -g dev) \
    --create-for-user=$HOST_UID \
    --create-for-group=$HOST_GID \
    --chown-ignore \
    --chgrp-ignore \
    /mnt/host.raw \
    /mnt/host

exec sleep infinity
