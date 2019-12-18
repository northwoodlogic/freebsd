#!/bin/sh

set -x
set -e

DESTDIR=$(pwd)/armv6-mini
SRCCONF=$(pwd)/src.conf
MINI_IMG=$(pwd)/armv6-mini.img
export TARGET=arm
export TARGET_ARCH=armv6
export KERNCONF=RPI_MINI
export DESTDIR

PM=$(sysctl -n hw.ncpu)

# Need to be in top level source directory.
HERE=$(pwd)
cd ../../

sed -e "s^@mfs_image@^${MINI_IMG}.uzip^g" \
	< sys/arm/conf/RPI_MINI.in \
	> sys/arm/conf/RPI_MINI

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
	echo "/dev/mmcsd0s1 /cfg msdosfs ro,failok 0 0" >> etc/fstab

	# defaults for the appliance image. These can be overridded by
	# the normal /etc/rc.conf config file.
	cat <<- EOT > etc/defaults/vendor.conf
	hostname="pbot"
	root_rw_mount="NO"
	entropy_file="NO"
	hostid_enable="NO"
	sshd_enable="YES"
	ifconfig_ue0_ipv6="auto_linklocal accept_rtadv"
	EOT

	# Trigger rc.initcfg
	mkdir cfg
	touch etc/initcfg

	# trigger 'rc.initdiskless' and copy everything in /etc by default
	touch etc/diskless
	mkdir -p conf/base/etc
	tar -cf - -C etc/ . | tar -xf - -C conf/base/etc/
)
makefs -B little -t ffs ${MINI_IMG} ${DESTDIR}
mkuzip -o ${MINI_IMG}.uzip ${MINI_IMG}

unset NO_CLEAN
make SRCCONF=${SRCCONF} -j${PM} buildkernel
DESTDIR=${DESTDIR}.kernel make SRCCONF=${SRCCONF} installkernel

# Now build FIT image containing kernel and two device trees. This requires
# u-boot-tools be installed on the host build machine.
cd "${HERE}"
mkimage -f rpi-mini.its rpi-mini.itb
