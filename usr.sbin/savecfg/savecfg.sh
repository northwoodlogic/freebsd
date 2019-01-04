#!/bin/sh

: ${CFG_DIR="/cfg"}
: ${CFG_CONF="/etc/savecfg.conf"}
: ${CFG_ARCHIVE="startup-config.tar"}

# Save running configuration (files listed in /etc/savecfg.conf)
# to persistent configuration partition mounted at /cfg.

mount -u -o rw,sync,noatime /cfg
if [ $? != 0 ] ; then
	echo "unable to upgrade ${CFG_DIR} mount rw"
	exit 1
fi

tar -cf ${CFG_DIR}/tmp-${CFG_ARCHIVE} -C / -T ${CFG_CONF}
if [ "$?" = "0" ] ; then
	# Maybe keep a rolling history....
	mv ${CFG_DIR}/tmp-${CFG_ARCHIVE} ${CFG_DIR}/${CFG_ARCHIVE}
else
	echo "unable to save configuration archive"
	rm ${CFG_DIR}/tmp-${CFG_ARCHIVE}
fi

sync
mount -u -o ro ${CFG_DIR}

