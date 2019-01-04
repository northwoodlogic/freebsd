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

	# /cfg is best effort, if this is marked noauto then init system will
	# umount /cfg if it happens be mounted by the rc.initcfg script???
	echo "/dev/da0s1 /cfg msdosfs ro,failok 0 0" >> etc/fstab

	# defaults for the appliance image. These can be overridded by
	# the normal /etc/rc.conf config file.
	cat <<- EOT > etc/defaults/vendor.conf
	hostname="edge"
	root_rw_mount="NO"
	entropy_file="NO"
	hostid_enable="NO"
	sshd_enable="YES"
	ifconfig_gi1_ipv6="auto_linklocal accept_rtadv"
	EOT

	# Write out an interface map. This allows the config shell to use a
	# consistent interface naming scheme and identifies physical
	# interfaces in the system.
	cat <<- EOT > etc/iface.map
	gi1:octe0
	gi2:octe1
	gi3:octe2
	EOT


	# Dummy rc.local placeholder
	cat <<- EOT > etc/rc.local
	#!/bin/sh
	echo "Hello"
	EOT

	chmod a+x etc/rc.local

	# Trigger rc.initcfg
	mkdir cfg
	touch etc/initcfg

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

