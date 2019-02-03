
#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/socket.h>

#include "ipfw2.h"

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>

#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip_fw.h>
#include <netinet6/ip_fw_6rd.h>
#include <arpa/inet.h>

#define	sixrd_check_name	table_check_name

static struct _s_x sixrd_cmds[] = {
	{ "create",	TOK_CREATE	},
	{ "destroy",	TOK_DESTROY	},
	{ "list",	TOK_LIST	},
	{ "show",	TOK_LIST	},
	{ "stats",	TOK_STATS	},
	{ NULL, 0 }
};

static struct _s_x sixrd_statscmds[] = {
	{ "reset",	TOK_RESET	},
	{ NULL, 0 }
};

static struct _s_x sixrd_newcmds[] = {
	{ "prefix",	TOK_6RD_PREFIX	},
	{ "relay",	TOK_6RD_RELAY	},
	{ "masklen",	TOK_6RD_MASKLEN	},
	{ "ip4src",	TOK_6RD_CE4	},
	{ NULL, 0 }
};

typedef int (sixrd_cb_t)(ipfw_6rd_cfg *i, const char *name, uint8_t set);
static int sixrd_foreach(sixrd_cb_t *f, const char *name, uint8_t set,
    int sort);

static void sixrd_fill_ntlv(ipfw_obj_ntlv *ntlv, const char *name, uint8_t set);
static void sixrd_destroy(const char *name, uint8_t set);
static int sixrd_get_stats(const char *name, uint8_t set, struct ipfw_6rd_stats *stats);
static void sixrd_stats(const char *name, uint8_t set);
static void sixrd_reset_stats(const char *name, uint8_t set);
static int sixrd_show_cb(ipfw_6rd_cfg *cfg, const char *name, uint8_t set);
static int sixrd_destroy_cb(ipfw_6rd_cfg *cfg, const char *name, uint8_t set);
static void sixrd_create(const char *name, uint8_t set, int ac, char *av[]);
static void sixrd_parse_ip6(const char *arg, struct in6_addr *addr, uint8_t *plen);
static void sixrd_parse_ip4(const char *arg, struct in_addr *addr);
static void sixrd_parse_len(const char *arg, uint8_t *len);

static void
sixrd_parse_ip6(const char *arg, struct in6_addr *addr, uint8_t *plen)
{
	char *p, *l;

	p = strdup(arg);
	if (p == NULL)
		err(EX_OSERR, NULL);

	if ((l = strchr(p, '/')) != NULL)
		*l++ = '\0';
	else
		errx(EX_USAGE, "Bad prefix length: %s", arg);

	if (inet_pton(AF_INET6, p, addr) != 1)
		errx(EX_USAGE, "Bad prefix: %s", p);

	*plen = (uint8_t)strtol(l, &l, 10);
	if (*plen > 64)
		errx(EX_USAGE, "Bad prefix length: %s", arg);

	free(p);
}

static void
sixrd_parse_ip4(const char *arg, struct in_addr *addr)
{
	if (inet_pton(AF_INET, arg, addr) != 1)
		errx(EX_USAGE, "Bad ip4 addr: %s", arg);
}

static void
sixrd_parse_len(const char *arg, uint8_t *len)
{
	*len = (uint8_t)strtol(arg, NULL, 10);
	if (*len >= 32)
		errx(EX_USAGE, "Bad ip4 mask length: %s", arg);
}

/*
 * Note: there's a lot of duplication here. A lot of the functions in each file are
 * the same only named differently
 */
static void
sixrd_fill_ntlv(ipfw_obj_ntlv *ntlv, const char *name, uint8_t set)
{
	ntlv->head.type = IPFW_TLV_EACTION_NAME(1); /* it doesn't matter */
	ntlv->head.length = sizeof(ipfw_obj_ntlv);
	ntlv->idx = 1;
	ntlv->set = set;
	strlcpy(ntlv->name, name, sizeof(ntlv->name));
}

static void
sixrd_destroy(const char *name, uint8_t set)
{
	ipfw_obj_header oh;
	memset(&oh, 0, sizeof(oh));
	sixrd_fill_ntlv(&oh.ntlv, name, set);
	if (do_set3(IP_FW_6RD_DESTROY, &oh.opheader, sizeof(oh)) != 0)
		err(EX_OSERR, "failed to destroy 6rd instance %s", name);
}

static int
sixrd_get_stats(const char *name, uint8_t set, struct ipfw_6rd_stats *stats)
{
	ipfw_obj_header *oh;
	ipfw_obj_ctlv *oc;
	size_t sz;

	sz = sizeof(*oh) + sizeof(*oc) + sizeof(*stats);
	oh = calloc(1, sz);
	sixrd_fill_ntlv(&oh->ntlv, name, set);
	if (do_get3(IP_FW_6RD_STATS, &oh->opheader, &sz) == 0) {
		oc = (ipfw_obj_ctlv *)(oh + 1);
		memcpy(stats, oc + 1, sizeof(*stats));
		free(oh);
		return (0);
	}
	free(oh);
	return (-1);
}

static void
sixrd_stats(const char *name, uint8_t set)
{
	struct ipfw_6rd_stats stats;

	if (sixrd_get_stats(name, set, &stats) != 0)
		err(EX_OSERR, "Error retrieving stats");

	if (co.use_set != 0 || set != 0)
		printf("set %u ", set);
	printf("6rd %s\n", name);
	printf("\t%ju packets translated (internal to external)\n",
	    (uintmax_t)stats.in2ex);
	printf("\t%ju packets translated (external to internal)\n",
	    (uintmax_t)stats.ex2in);
	printf("\t%ju packets dropped due to some error\n",
	    (uintmax_t)stats.dropped);
}

static void
sixrd_reset_stats(const char *name, uint8_t set)
{
	ipfw_obj_header oh;

	memset(&oh, 0, sizeof(oh));
	sixrd_fill_ntlv(&oh.ntlv, name, set);
	if (do_set3(IP_FW_6RD_RESET_STATS, &oh.opheader, sizeof(oh)) != 0)
		err(EX_OSERR, "failed to reset stats for instance %s", name);
}

static int
sixrd_show_cb(ipfw_6rd_cfg *cfg, const char *name, uint8_t set)
{
	char abuf[INET6_ADDRSTRLEN];

	if (name != NULL && strcmp(cfg->name, name) != 0)
		return (ESRCH);

	if (co.use_set != 0 && cfg->set != set)
		return (ESRCH);

	if (co.use_set != 0 || cfg->set != 0)
		printf("set %u ", cfg->set);

	inet_ntop(AF_INET6, &cfg->prefix6, abuf, sizeof(abuf));
	printf("6rd %s prefix %s/%u ", cfg->name, abuf, cfg->prefixlen);
	inet_ntop(AF_INET, &cfg->relay4, abuf, sizeof(abuf));
	printf("relay %s ", abuf);
	inet_ntop(AF_INET, &cfg->ce4, abuf, sizeof(abuf));
	printf("ip4src %s ", abuf);
	printf("masklen %u ", cfg->masklen);

	// TODO: Print computed delegated prefix.
	return (0);
}

static int
sixrd_destroy_cb(ipfw_6rd_cfg *cfg, const char *name, uint8_t set)
{
	if (co.use_set != 0 && cfg->set != set)
		return (ESRCH);
	sixrd_destroy(cfg->name, cfg->set);
	return (0);
}

static int
sixrd_foreach(sixrd_cb_t *f, const char *name, uint8_t set, int sort)
{
	ipfw_obj_lheader *olh;
	ipfw_6rd_cfg *cfg;
	size_t sz;
	int i, error;

	/* Start with reasonable default */
	sz = sizeof(*olh) + 16 * sizeof(*cfg);
	for (;;) {
		if ((olh = calloc(1, sz)) == NULL)
			return (ENOMEM);

		olh->size = sz;
		if (do_get3(IP_FW_6RD_LIST, &olh->opheader, &sz) != 0) {
			sz = olh->size;
			free(olh);
			if (errno != ENOMEM)
				return (errno);
			continue;
		}
#if 0
		if (sort != 0)
			qsort(olh + 1, olh->count, olh->objsize, sixrd_name_cmp);
#endif
		cfg = (ipfw_6rd_cfg *)(olh + 1);
		for (i = 0; i < olh->count; i++) {
			error = f(cfg, name, set);
			cfg = (ipfw_6rd_cfg *)((caddr_t)cfg + olh->objsize);
		}
		free(olh);
		break;
	}
	return (0);
}

#define _6RD_HAS_PREFIX		0x01
#define _6RD_HAS_RELAY		0x02
#define _6RD_HAS_MASKLEN	0x04
#define _6RD_HAS_CE4		0x08

static void
sixrd_create(const char *name, uint8_t set, int ac, char *av[])
{
	char buf[sizeof(ipfw_obj_lheader) + sizeof(ipfw_6rd_cfg)];
	ipfw_6rd_cfg *cfg;
	ipfw_obj_lheader *olh;
	int tcmd, flags, plen;

	plen = 0;
	memset(buf, 0, sizeof(buf));
	olh = (ipfw_obj_lheader *)buf;
	cfg = (ipfw_6rd_cfg *)(olh + 1);
	cfg->set = set;
	flags = 0;
	while (ac > 0) {
		tcmd = get_token(sixrd_newcmds, *av, "option");
		ac--; av++;

		switch (tcmd) {
		case TOK_6RD_PREFIX:
			NEED1("6rd prefix required");
			sixrd_parse_ip6(*av, &cfg->prefix6, &cfg->prefixlen);
			flags |= _6RD_HAS_PREFIX;
			ac--; av++;
			break;
		case TOK_6RD_RELAY:
			NEED1("6rd relay required");
			sixrd_parse_ip4(*av, &cfg->relay4);
			flags |= _6RD_HAS_RELAY;
			ac--; av++;
			break;
		case TOK_6RD_MASKLEN:
			NEED1("6rd masklen required");
			sixrd_parse_len(*av, &cfg->masklen);
			flags |= _6RD_HAS_MASKLEN;
			ac--; av++;
			break;
		case TOK_6RD_CE4:
			NEED1("6rd ip4src required");
			sixrd_parse_ip4(*av, &cfg->ce4);
			flags |= _6RD_HAS_CE4;
			ac--; av++;
			break;
		}
	}

	/* all args are required */
	if (!(flags & _6RD_HAS_PREFIX))
		errx(EX_USAGE, "prefix required");
	if (!(flags & _6RD_HAS_RELAY))
		errx(EX_USAGE, "relay required");
	if (!(flags & _6RD_HAS_MASKLEN))
		errx(EX_USAGE, "masklen required");
	if (!(flags & _6RD_HAS_CE4))
		errx(EX_USAGE, "ip4src required");

	/* TODO: verify 6rd prefix length and ip4 mask length make sense... */

	olh->count = 1;
	olh->objsize = sizeof(*cfg);
	olh->size = sizeof(buf);
	strlcpy(cfg->name, name, sizeof(cfg->name));
	if (do_set3(IP_FW_6RD_CREATE, &olh->opheader, sizeof(buf)) != 0)
		err(EX_OSERR, "6rd instance creation failed");
}

void
ipfw_6rd_handler(int ac, char *av[])
{
	const char *name;
	int tcmd;
	uint8_t set;

	if (co.use_set != 0)
		set = co.use_set - 1;
	else
		set = 0;
	ac--; av++;

	NEED1("6rd needs instance name");
	name = *av;
	if (sixrd_check_name(name) != 0) {
		if (strcmp(name, "all") == 0) {
			name = NULL;
		} else
			errx(EX_USAGE, "6rd instance name %s is invalid",
			    name);
	}
	ac--; av++;
	NEED1("6rd needs command");

	tcmd = get_token(sixrd_cmds, *av, "6rd command");
	if (name == NULL && tcmd != TOK_DESTROY && tcmd != TOK_LIST)
		errx(EX_USAGE, "6rd instance name required");

	if (name == NULL)
		errx(EX_USAGE, "6rd instance name required");

	switch (tcmd) {
	case TOK_CREATE:
		ac--; av++;
		sixrd_create(name, set, ac, av);
		break;
	case TOK_LIST:
		sixrd_foreach(sixrd_show_cb, name, set, 1);
		break;
	case TOK_DESTROY:
		if (name == NULL)
			sixrd_foreach(sixrd_destroy_cb, NULL, set, 0);
		else
			sixrd_destroy(name, set);
		break;
	case TOK_STATS:
		ac--; av++;
		if (ac == 0) {
			sixrd_stats(name, set);
			break;
		}
		tcmd = get_token(sixrd_statscmds, *av, "stats command");
		if (tcmd == TOK_RESET)
			sixrd_reset_stats(name, set);
	}

}

