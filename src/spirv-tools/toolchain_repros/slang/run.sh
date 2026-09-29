#!/bin/sh
# Compile each Slang reproducer and report crashes and compile times.
#   SLANGC=/path/to/slangc ./run.sh *.slang
SLANGC=${SLANGC:-slangc}
for f in "$@"; do
    start=$(date +%s)
    "$SLANGC" "$f" -target spirv -incomplete-library -o /tmp/repro_out.spv \
        >/dev/null 2>&1
    rc=$?
    secs=$(( $(date +%s) - start ))
    if [ $rc -gt 128 ]; then
        echo "$f: slangc CRASHED (signal $((rc - 128))), ${secs}s"
    elif [ $rc -ne 0 ]; then
        echo "$f: slangc failed (exit $rc), ${secs}s"
    else
        echo "$f: compiled, ${secs}s"
    fi
done
