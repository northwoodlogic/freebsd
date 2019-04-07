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

# 64MB
MD_IMAGE_SIZE=67108864

# 80MB
#MD_IMAGE_SIZE=83886080

# The default EFI staging area is 64MB. That's not big enough for us so
# double it to 128MB (even though the config above makes a 64MB laoder).
# Note: The total staging area must be in the first
# 1GB of memory.
EFI_STAGING_SIZE=128

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

make SRCCONF=${SRCCONF} EFI_STAGING_SIZE=${EFI_STAGING_SIZE} MD_IMAGE_SIZE=${MD_IMAGE_SIZE} -j${PM} buildworld
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

	if [ -d "usr/include" ] ; then
		rm -Rf usr/include
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
	sshd_enable="YES"
	zfs_enable="YES"
	powerd_enable="YES"
	rtsold_enable="YES"
	ifconfig_mlxen0_ipv6="auto_linklocal accept_rtadv"

	kld_list="vmm"
	EOT

	cat <<-EOT > etc/sysctl.conf
	vfs.zfs.min_auto_ashift=12
	net.link.tap.up_on_open=1
	EOT

	# Trigger rc.initcfg
	mkdir cfg
	touch etc/initcfg

	# trigger 'rc.initdiskless' and copy everything in /etc by default
	touch etc/diskless
	mkdir -p conf/base/etc
	tar -cf - -C etc/ . | tar -xf - -C conf/base/etc/

	# scratch dir for mounting disks, if applicable
	mkdir data
)

# Each one of the "loaders" will allocate 64MB of space which ends
# up being > 500MB of stuff used up under /boot. All we need is the
# loader_simp.efi, a couple config files, kernel, & md_root
mv ${DESTDIR}/boot ${BOOTDIR}

make SRCCONF=${SRCCONF} -j${PM} buildkernel
DESTDIR=${DESTDIR} make SRCCONF=${SRCCONF} installkernel

cd ${HERE}

cp ${BOOTDIR}/loader_simp.efi amd64-mini.efi
rm   -Rf md-boot
mkdir -p md-boot/boot/defaults
mkdir -p md-boot/boot/kernel

cp ${BOOTDIR}/device.hints              md-boot/boot/
cp ${BOOTDIR}/defaults/loader.conf      md-boot/boot/defaults/

# This needs to happen after the kernel is moved out of DESTDIR
# but before copying the u-zip image into md-root/boot
mv ${DESTDIR}/boot/kernel/kernel	md-boot/boot/kernel/
makefs -B little -t ffs ${MINI_IMG} ${DESTDIR}
mkuzip -o ${MINI_IMG}.uzip ${MINI_IMG}

cp ${MINI_IMG}.uzip			md-boot/boot/

# loader_simp.efi doesn't have a script interpreter. Use commands
# directly in loader.rc. Loader does a sanity check which involves
# checking for two files:
#
#	/boot/kernel/kernel
#	/boot/defaults/loader.conf
#
cat << EOT > md-boot/boot/loader.rc
unload
load             /boot/kernel/kernel
load -t md_image /boot/amd64-mini.img.uzip

set hw.mfi.mrsas_enable="1"
set kern.geom.label.disk_ident.enable="0"
set kern.geom.label.gptid.enable="0"
EOT

makefs md-boot.img md-boot
sh ../../sys/tools/embed_mfs.sh amd64-mini.efi md-boot.img


