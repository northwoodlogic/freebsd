#!/bin/sh

set -x
set -e

HERE=$(pwd)
BOOTDIR=$(pwd)/aboot
DESTDIR=$(pwd)/amd64-mini
SRCCONF=$(pwd)/src.conf
MINI_IMG=$(pwd)/amd64-mini.img
export KERNCONF=AMD64_MINI
export DESTDIR

# 64MB that's reserved in loader.efi
MD_IMAGE_SIZE=67108864

PM=$(sysctl -n hw.ncpu)

# Need to be in top level source directory.
cd ../../

sed -e "s^@mfs_image@^${MINI_IMG}.uzip^g" \
	< sys/amd64/conf/AMD64_MINI.in \
	> sys/amd64/conf/AMD64_MINI

if [ -e ${DESTDIR} ] ; then
	chflags -R noschg ${DESTDIR}
	rm -Rf ${DESTDIR}
fi

if [ -e ${MINI_IMG} ] ; then
	rm ${MINI_IMG}
fi

if [ -e ${BOOTDIR} ] ; then
	chflags -R noschg ${BOOTDIR}
	rm -Rf ${BOOTDIR}
fi

make SRCCONF=${SRCCONF} MD_IMAGE_SIZE=${MD_IMAGE_SIZE} -j${PM} buildworld
echo "DONE BUILDING WORLD"

make SRCCONF=${SRCCONF} installworld
echo "DONE INSTALLING WORLD"

make SRCCONF=${SRCCONF} distribution
echo "DONE DISTRIBUTE WORLD"

(
	cd ${DESTDIR}
	# don't want static archives
	
	if [ -d "usr/lib" ] ; then
		rm -Rf usr/lib/*.a
	fi

	if [ -d "usr/lib32" ] ; then
		rm -Rf usr/lib32/*.a
	fi

	# uzip rootfs is ro, no need to fsck
	echo "/dev/md0.uzip / ufs ro 0 0" > etc/fstab

	# /cfg is best effort, if this is marked noauto then init system will
	# umount /cfg if it happens be mounted by the rc.initcfg script???
	echo "/dev/gpt/esp /cfg msdosfs ro,failok 0 0" >> etc/fstab

	# defaults for the appliance image. These can be overridded by
	# the normal /etc/rc.conf config file.
	cat <<- EOT > etc/defaults/vendor.conf
	hostname="xbot"
	root_rw_mount="NO"
	entropy_file="NO"
	hostid_enable="NO"
	sshd_enable="YES"
	#ifconfig_ue0_ipv6="auto_linklocal accept_rtadv"
	EOT

	# Trigger rc.initcfg
	mkdir cfg
	touch etc/initcfg

	# trigger 'rc.initdiskless' and copy everything in /etc by default
	touch etc/diskless
	mkdir -p conf/base/etc
	tar -cf - -C etc/ . | tar -xf - -C conf/base/etc/
)

mv ${DESTDIR}/boot ${BOOTDIR}
makefs -B little -t ffs ${MINI_IMG} ${DESTDIR}
mkuzip -o ${MINI_IMG}.uzip ${MINI_IMG}

# TODO: change this so the md image isn't linked into the kernel. we
# need loader anyway so let it load the image as a module.
unset NO_CLEAN
make SRCCONF=${SRCCONF} -j${PM} buildkernel
DESTDIR=${DESTDIR}.kernel make SRCCONF=${SRCCONF} installkernel

cd ${HERE}
cp ${BOOTDIR}/loader_simp.efi amd64-mini.efi
rm   -Rf md-boot
mkdir -p md-boot/boot/defaults
mkdir -p md-boot/boot/kernel

cp ${BOOTDIR}/device.hints              md-boot/boot/
cp ${BOOTDIR}/defaults/loader.conf      md-boot/boot/defaults/
cp ${DESTDIR}.kernel/boot/kernel/kernel md-boot/boot/kernel/
echo 'hw.mfi.mrsas_enable="1"' >>       md-boot/boot/device.hints

makefs md-boot.img md-boot
sh ../../sys/tools/embed_mfs.sh amd64-mini.efi md-boot.img


