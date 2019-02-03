#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/rwlock.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

#include <net/if.h>
#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/ip_var.h>
#include <netinet/ip_fw.h>

#include <netpfil/ipfw/ip_fw_private.h>

#include "ip_fw_6rd.h"

VNET_DEFINE(int, sixrd_debug) = 0;
SYSCTL_DECL(_net_inet_ip_fw);
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, 6rd_debug, CTLFLAG_VNET | CTLFLAG_RW,
    &VNET_NAME(sixrd_debug), 0, "Debug level for 6rd module");

static int
vnet_ipfw_6rd_init(const void *arg __unused)
{
	struct ip_fw_chain *ch;
	int first, error;

	ch = &V_layer3_chain;
	first = IS_DEFAULT_VNET(curvnet) ? 1: 0;
	error = sixrd_init(ch, first);
	if (error != 0)
		return (error);
	return (0);
}

static int
vnet_ipfw_6rd_uninit(const void *arg __unused)
{
	struct ip_fw_chain *ch;
	int last;
	
	ch = &V_layer3_chain;
	last = IS_DEFAULT_VNET(curvnet) ? 1: 0;
	sixrd_uninit(ch, last);
	return (0);
}

static int
ipfw_6rd_modevent(module_t mod, int type, void *unused)
{
	switch (type) {
	case MOD_LOAD:
	case MOD_UNLOAD:
		break;
	default:
		return (EOPNOTSUPP);
	}
	return (0);
}

static moduledata_t ipfw_6rd_mod = {
	"ipfw_6rd",
	ipfw_6rd_modevent,
	0
};

/* Define startup order. */
#define	IPFW_6RD_SI_SUB_FIREWALL	SI_SUB_PROTO_IFATTACHDOMAIN
#define	IPFW_6RD_MODEVENT_ORDER	(SI_ORDER_ANY - 128) /* after ipfw */
#define	IPFW_6RD_MODULE_ORDER		(IPFW_6RD_MODEVENT_ORDER + 1)
#define	IPFW_6RD_VNET_ORDER		(IPFW_6RD_MODEVENT_ORDER + 2)

DECLARE_MODULE(ipfw_6rd, ipfw_6rd_mod,
	IPFW_6RD_SI_SUB_FIREWALL, SI_ORDER_ANY);
MODULE_DEPEND(ipfw_6rd, ipfw, 3, 3, 3);
MODULE_VERSION(ipfw_6rd, 1);

VNET_SYSINIT(vnet_ipfw_6rd_init, IPFW_6RD_SI_SUB_FIREWALL,
    IPFW_6RD_VNET_ORDER, vnet_ipfw_6rd_init, NULL);
VNET_SYSUNINIT(vnet_ipfw_6rd_uninit, IPFW_6RD_SI_SUB_FIREWALL,
    IPFW_6RD_VNET_ORDER, vnet_ipfw_6rd_uninit, NULL);


