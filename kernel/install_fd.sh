#!/bin/sh

TRANSFER=/etc/transfer.ref
DEVICE_NAME=SVECFDELAY
DRIVER_NAME=fmc-fine-delay

# symlink to true GOLDEN="fmc/golden-svec-20130114-1000.bin"
GOLDEN="fmc/svec_golden.bin"

# symlink to true FDELAY="fmc/svec-fine-delay-20121218-1549.bin"
FDELAY="fmc/svec_top.bin"

OUTPUT=":"
RUN=""

while getopts hvnc:D:d:t: o
do	case $o in
	v)	OUTPUT="echo" ;;		# verbose
	n)	RUN=":" ;;			# dry run
	D)	DEVICE_NAME="$OPTARG" ;;
	d)	DRIVER_NAME="$OPTARG" ;;
	c)	CRATECONFIG="$OPTARG" ;;
	t)	TRANSFER="$OPTARG" ;;
	[h?])	echo >&2 "usage: $0 [-?hvn] [-D device_name] [-d driver_name] [-t transfer]"
		exit ;;
	esac
done

$OUTPUT "$DRIVER_NAME install"
echo Installing zio and fmc drivers ...
/sbin/insmod zio.ko && /sbin/insmod fmc.ko 

echo Installing fine delay driver...
/sbin/insmod $DRIVER_NAME.ko gateware=$FDELAY timer_ms=10

INSMOD_ARGS=`awk -f ./svec.awk FMC-SVEC $TRANSFER |
	sed 's!\$GOLDEN!'$GOLDEN'!g'`
echo "svec: insmod with $INSMOD_ARGS"
/sbin/insmod svec.ko $INSMOD_ARGS

echo "$DRIVER_NAME: making device nodes"
awk -f ./fd-luns.awk $DEVICE_NAME $TRANSFER | sh
