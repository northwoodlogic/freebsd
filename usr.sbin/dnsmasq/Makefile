# $FreeBSD$

DNSMASQDIR= ${SRCTOP}/contrib/dnsmasq/src
.PATH: ${DNSMASQDIR}

CONFS=	dnsmasq.conf

PROG=	local-dnsmasq
SRCS=	cache.c rfc1035.c util.c option.c forward.c network.c \
	dnsmasq.c dhcp.c lease.c rfc2131.c netlink.c dbus.c bpf.c \
	helper.c tftp.c log.c conntrack.c dhcp6.c rfc3315.c \
	dhcp-common.c outpacket.c radv.c slaac.c auth.c ipset.c \
	domain.c dnssec.c blockdata.c tables.c loop.c inotify.c \
	poll.c rrfilter.c edns0.c arp.c crypto.c dump.c ubus.c metrics.c

CFLAGS+=	-I${DNSMASQDIR} -DVERSION=\"2.80\"

# dnsmasq assumes that FreeBSD builds want their conf files in
# /usr/local/etc. That's not the case for the base system.
CFLAGS+=	-DCONFFILE=\"/etc/dnsmasq.conf\" -DHAVE_BROKEN_RTC

# dnsmasq abuses function pointer syntax that generate warnings.
WARNS=0

# There is a man page but embedded system appliances won't install them.
MAN=

.include <bsd.prog.mk>
