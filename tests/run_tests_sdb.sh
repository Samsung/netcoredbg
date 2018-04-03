#!/bin/sh

# Before running this script:
#   1. Enable root mode:         sdb root on
#   2. Remount system partition: sdb shell mount -o rw,remount /
#   3. Install netcoredbg rpm:   sdb push netcoredbg-*.rpm /tmp && sdb shell rpm -i --force /tmp/netcoredbg-*.rpm
#   4. Create dotnet symlink:    sdb shell ln -s /usr/share/dotnet/corerun /usr/bin/dotnet

SDB=${SDB:-sdb}

SCRIPTDIR=$(dirname $(readlink -f $0))

# Upload all test dlls and pdbs
find $SCRIPTDIR -name '*Test.runtimeconfig.json' | while read fname; do
  base=$(echo $fname | rev | cut -f 3- -d '.' | rev)
  $SDB push ${base}.dll ${base}.pdb /tmp
  # After push some targets set SMACK access label to System, but the debugger can not access System files.
  # Also, prevent sdb from consuming input.
  cat /dev/null | $SDB shell "chsmack -a '*' /tmp/$(basename ${base}).dll /tmp/$(basename ${base}).pdb"
done

# Run tests

DEBUGGER=/home/owner/share/tmp/sdk_tools/netcoredbg/netcoredbg

# sdb always allocates a terminal for a shell command, but we do not want MI commands to be echoed.
# So we need to delay stdin by 1 second and hope that `stty raw -echo` turns the echo off during the delay.
TESTDIR=/tmp PIPE="(sleep 1; cat) | $SDB shell stty raw -echo\\;$DEBUGGER --interpreter=mi" dotnet test $SCRIPTDIR --logger "trx;LogFileName=$SCRIPTDIR/Results.trx"
