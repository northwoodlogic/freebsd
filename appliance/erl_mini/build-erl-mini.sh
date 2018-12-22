#!/bin/sh

set -x
set -e

DESTDIR=$(pwd)/mips64-mini
SRCCONF=$(pwd)/src.conf
MINI_IMG=$(pwd)/mips64-mini.img
export TARGET=mips
export TARGET_ARCH=mips64
export KERNCONF=ERL_MINI
export DESTDIR

PM=$(sysctl -n hw.ncpu)

# Need to be in top level source directory.
cd ../../

sed -e "s^@mfs_image@^${MINI_IMG}.uzip^g" \
	< sys/mips/conf/ERL_MINI.in \
	> sys/mips/conf/ERL_MINI

if [ -e ${DESTDIR} ] ; then
	chflags -R noschg ${DESTDIR}
	rm -Rf ${DESTDIR}
fi

if [ -e ${MINI_IMG} ] ; then
	rm ${MINI_IMG}
fi

make SRCCONF=${SRCCONF} -j${PM} buildworld
echo "DONE BUILDING WORLD"

make SRCCONF=${SRCCONF} installworld
echo "DONE INSTALLING WORLD"

make SRCCONF=${SRCCONF} distribution
echo "DONE DISTRIBUTE WORLD"

(
	cd ${DESTDIR}
	# don't want static archives
	rm usr/lib/*.a

	# uzip rootfs is ro, no need to fsck
	echo "/dev/md0.uzip / ufs ro 0 0" > etc/fstab

	cat << EOT > etc/rc.conf
root_rw_mount="NO"
entropy_file="NO"
hostid_enable="NO"
hostname="edge"
EOT

	# Dummy rc.local placeholder
	cat << EOT > etc/rc.local
#!/bin/sh
echo "Hello"
EOT
	chmod a+x etc/rc.local

	# trigger 'rc.initdiskless' and copy everything in /etc by default
	touch etc/diskless
	mkdir -p conf/base/etc
	tar -cf - -C etc/ . | tar -xf - -C conf/base/etc/

)
makefs -B big -t ffs ${MINI_IMG} ${DESTDIR}
mkuzip -o ${MINI_IMG}.uzip ${MINI_IMG}

unset NO_CLEAN
make SRCCONF=${SRCCONF} -j${PM} buildkernel
DESTDIR=${DESTDIR}.kernel make SRCCONF=${SRCCONF} installkernel

#
# Self contained 'appliance' kernel+rootfs is located here:
#   ${DESTDIR}.kernel/boot/kernel/kernel
#
# Kernel is approximately 36MB
#
# Octeon ubnt_e100# dhcp
# Interface 0 has 3 ports (RGMII)
# BOOTP broadcast 1
# octeth0: Up 1000 Mbps Full duplex (port  0)
# DHCP client bound to address 10.0.0.33
#
# Octeon ubnt_e100# setenv serverip 10.0.0.31
#
# Octeon ubnt_e100# tftp $loadaddr kernel
# Using octeth0 device
# TFTP from server 10.0.0.31; our IP address is 10.0.0.33
# Filename 'kernel'.
# Load address: 0x9f00000
# Loading: #################################################################
#          #################################################################
#          #################################################################
#          #################################################################
#          ######
# done
# Bytes transferred = 37974056 (2437028 hex), 5579 Kbytes/sec
#
# Octeon ubnt_e100# bootoctlinux $loadaddr coremask=0x3

