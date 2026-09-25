#!/usr/bin/env bash
set -eo pipefail

CLIENT="./client"
MODULE_NAME="throttle_module"
DEVICE_NODE="/dev/throttle_dev"

if [[ $EUID -ne 0 ]]; then
    echo "Questo script richiede i privilegi di root (usa sudo)." >&2
    exit 1
fi

if [[ ! -x "$CLIENT" ]]; then
    echo "Client non trovato in '$CLIENT'." >&2
    exit 1
fi

MOD_SYS_NAME="${MODULE_NAME//-/_}"

if [[ ! -d "/sys/module/$MOD_SYS_NAME" && ! -d "/sys/module/$MODULE_NAME" ]]; then
    echo "Il modulo '$MODULE_NAME' non risulta caricato."
    exit 0
fi

echo "== Deregistrazione delle syscall agganciate =="
while true; do
    mapfile -t ids < <("$CLIENT" list_syscalls | awk '/^[[:space:]]*-[[:space:]]*[0-9]+/{print $2}')
    if [[ ${#ids[@]} -eq 0 ]]; then
        break
    fi
    for id in "${ids[@]}"; do
        echo "  Deregistro syscall $id..."
        "$CLIENT" del_syscall "$id" || true
    done
done

echo "== Disattivo il monitor =="
"$CLIENT" off || true

echo "== Tentativo di rimozione del modulo '$MODULE_NAME' =="
if rmmod "$MODULE_NAME" 2>/dev/null; then
    echo "Modulo rimosso con successo."
    exit 0
fi

echo "" >&2
echo "rmmod fallito: il modulo è in uso (refcount > 0)." >&2
if [[ -f "/sys/module/$MODULE_NAME/refcnt" ]]; then
    echo "Refcount attuale: $(cat "/sys/module/$MODULE_NAME/refcnt")" >&2
fi

echo "" >&2
echo "Ricerca dei thread bloccati dentro hook_func (/proc/*/task/*/stack)..." >&2

declare -A culprit_pids
for stack in /proc/[0-9]*/task/[0-9]*/stack; do
    if grep -q "$MODULE_NAME" "$stack" 2>/dev/null; then
        pid=$(echo "$stack" | cut -d/ -f3)
        culprit_pids["$pid"]=1
    fi
done

if [[ ${#culprit_pids[@]} -gt 0 ]]; then
    echo "Trovati processi bloccati nel modulo:" >&2
    for p in "${!culprit_pids[@]}"; do
        ps -fp "$p" >&2
    done
    
    echo "" >&2
    read -rp "Vuoi terminare (SIGTERM) questi processi per completare rmmod? [s/N] " confirm
    if [[ "$confirm" =~ ^[sSyY]$ ]]; then
        for p in "${!culprit_pids[@]}"; do
            echo "Termino PID $p..." >&2
            kill -15 "$p" 2>/dev/null || true
        done
        sleep 0.5
        if rmmod "$MODULE_NAME"; then
            echo "Modulo rimosso con successo dopo la terminazione dei processi."
            exit 0
        fi
    fi
else
    echo "Nessun sub-thread con '$MODULE_NAME' nello stack trace." >&2
fi

exit 1
