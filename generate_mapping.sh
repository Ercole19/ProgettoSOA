#!/bin/bash

HEADER_FILE="header/syscall_mapping.h"
UNISTD_H=$(ls /usr/include/x86_64-linux-gnu/asm/unistd_64.h /usr/include/asm/unistd_64.h 2>/dev/null | head -n 1)

if [ -z "$UNISTD_H" ]; then
    echo "Errore: unistd_64.h non trovato!"
    exit 1
fi

echo "Generazione di $HEADER_FILE usando $UNISTD_H..."

MAX_ID=$(awk '/^#define __NR_/ && $3 ~ /^[0-9]+/ {print $3}' "$UNISTD_H" | sort -nr | head -n 1)
ARRAY_SIZE=$((MAX_ID + 1))

cat <<EOF > "$HEADER_FILE"
#pragma once

#define MAX_SYSCALLS $ARRAY_SIZE

static const char __attribute__((unused)) *syscall_sym_names[MAX_SYSCALLS] = {
EOF

awk '/^#define __NR_/ && $3 ~ /^[0-9]+/ {
    name=$2; 
    sub("__NR_", "", name);
    print "    [" $3 "] = \"__x64_sys_" name "\",";
}' "$UNISTD_H" >> "$HEADER_FILE"

echo "};" >> "$HEADER_FILE"
