#!/bin/bash
# This script expects to be invoked from the base fio directory.
set -eu

main() {
    [ "${CI_TARGET_BUILD}" = android ] && return 0

    echo "Running long running tests..."
    export PYTHONUNBUFFERED="TRUE"
    if [[ "${CI_TARGET_ARCH}" == "arm64" ]]; then
        python3 t/run-fio-tests.py --skip 6 1007 1008 --debug -p 1010:"--skip 15 16 17 18 19 20"
    else
        python3 t/run-fio-tests.py --skip 6 1007 1008 --debug
    fi
    make -C doc html
}

main
