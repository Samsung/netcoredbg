#!/bin/sh

SDB=${SDB:-sdb}

GBSROOT=$HOME/GBS-ROOT
TOOLS_ABS_PATH=/home/owner/share/tmp/sdk_tools

SCRIPTDIR=$(dirname $(readlink -f $0))

# Detect target arch

if $SDB shell cat /proc/cpuinfo | grep -q ARMv7; then ARCH=armv7l; else ARCH=i686; fi

# The following command assumes that GBS build was performed on a clean system (or in Docker),
# which means only one such file exists.
RPMFILE=$(find $GBSROOT/local/repos/ -type f -name netcoredbg-[0-9]\*$ARCH.rpm -print -quit)

# Repackage RPM file as TGZ

if [ ! -f "$RPMFILE" ]; then exit 1; fi
PKGNAME=`rpm -q --qf "%{n}" -p $RPMFILE`
PKGVERSION=`rpm -q --qf "%{v}" -p $RPMFILE`
PKGARCH=`rpm -q --qf "%{arch}" -p $RPMFILE`
TARGZNAME=$PKGNAME-$PKGVERSION-$PKGARCH.tar.gz
if [ -d $SCRIPTDIR/unpacked ]; then rm -rf $SCRIPTDIR/unpacked; fi
mkdir $SCRIPTDIR/unpacked && cd $SCRIPTDIR/unpacked
rpm2cpio "$RPMFILE" | cpio -idmv
touch .$TOOLS_ABS_PATH/$PKGNAME/version-$PKGVERSION
tar cfz ../$TARGZNAME --owner=root --group=root -C .$TOOLS_ABS_PATH .
cd ..

# Upload TGZ to target and unpack

REMOTETESTDIR=$TOOLS_ABS_PATH/netcoredbg-tests

$SDB root on
$SDB shell rm -rf $TOOLS_ABS_PATH/netcoredbg
$SDB shell mkdir -p $TOOLS_ABS_PATH/on-demand
$SDB push $TARGZNAME $TOOLS_ABS_PATH/on-demand
$SDB shell "cd $TOOLS_ABS_PATH && tar xf $TOOLS_ABS_PATH/on-demand/$(basename $TARGZNAME)"
$SDB shell rm -rf $REMOTETESTDIR
$SDB shell mkdir $REMOTETESTDIR

# Upload all test dlls and pdbs

find $SCRIPTDIR -name '*Test.runtimeconfig.json' | while read fname; do
  base=$(echo $fname | rev | cut -f 3- -d '.' | rev)
  $SDB push ${base}.dll ${base}.pdb $REMOTETESTDIR
done

$SDB shell "echo -e '#!/bin/sh\nexec /lib/ld-linux.so.3 /usr/share/dotnet/corerun --clr-path /usr/share/dotnet/shared/Microsoft.NETCore.App/2.0.0 \$@' > $REMOTETESTDIR/dotnet"
$SDB shell chmod +x $REMOTETESTDIR/dotnet

# Run tests

DEBUGGER=$TOOLS_ABS_PATH/netcoredbg/netcoredbg

# sdb always allocates a terminal for a shell command, but we do not want MI commands to be echoed.
# So we need to delay stdin by 1 second and hope that `stty raw -echo` turns the echo off during the delay.
TESTDIR=$REMOTETESTDIR PIPE="(sleep 1; cat) | $SDB shell stty raw -echo\\; export PATH=\$PATH:$TOOLS_ABS_PATH/netcoredbg-tests\\; $DEBUGGER --log=file --interpreter=mi" dotnet test $SCRIPTDIR --logger "trx;LogFileName=$SCRIPTDIR/Results.trx"
