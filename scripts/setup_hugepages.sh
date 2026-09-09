#!/bin/bash
set -e

HUGEPAGES=${HUGEPAGES:-1024}

sudo sysctl -w vm.nr_hugepages="$HUGEPAGES"

echo "Configured $HUGEPAGES hugepages."
cat /proc/meminfo | grep Huge
