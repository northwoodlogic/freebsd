
#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/counter.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/rmlock.h>
#include <sys/rwlock.h>
#include <sys/socket.h>
#include <sys/sockopt.h>
#include <sys/queue.h>

#include <net/if.h>
#include <net/if_var.h>
#include <net/netisr.h>
#include <net/pfil.h>
#include <net/vnet.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/ip_fw.h>
#include <netinet/ip6.h>
//#include <netinet/icmp6.h>
#include <netinet6/in6_var.h>
#include <netinet6/ip6_var.h>
#include <netinet6/ip_fw_6rd.h>

#include <netpfil/ipfw/ip_fw_private.h>

#include "6rd.h"

VNET_DEFINE(uint16_t, sixrd_eid) = 0;

#define	_6RD_STATS	(sizeof(struct ipfw_6rd_stats) / sizeof(uint64_t))

#define	_6RD_STAT_ADD(c, f, v)		\
    counter_u64_add((c)->stats[		\
	offsetof(struct ipfw_6rd_stats, f) / sizeof(uint64_t)], (v))

#define	_6RD_STAT_INC(c, f)	_6RD_STAT_ADD(c, f, 1)

#define	_6RD_STAT_FETCH(c, f)		\
    counter_u64_fetch((c)->stats[	\
	offsetof(struct ipfw_6rd_stats, f) / sizeof(uint64_t)])

#define	_6RD_LOOKUP(chain, cmd)	\
    (struct sixrd_cfg *)SRV_OBJECT((chain), (cmd)->arg1)

struct sixrd_cfg {
	struct named_object	no;
	char			name[64];   /* Instance name */

	struct in6_addr	dprefix6;	/* Delegated prefix (computed) */
	struct in6_addr	prefix6;	/* ISP prefix */
	struct in_addr	relay4;		/* ISP border relay */
	struct in_addr	ce4;		/* CE IPv4 address */
	uint8_t		dprefixlen;
	uint8_t		prefixlen;
	uint8_t		masklen;
	uint8_t		unused[5];
	
	counter_u64_t		stats[_6RD_STATS]; /* Statistics counters */
};

static int
ipfw_6rd(struct ip_fw_chain *chain, struct ip_fw_args *args,
    ipfw_insn *cmd, int *done);


static int sixrd_create(struct ip_fw_chain *ch,
	ip_fw3_opheader *op3, struct sockopt_data *sd);

static int sixrd_destroy(struct ip_fw_chain *ch,
	ip_fw3_opheader *op3, struct sockopt_data *sd);

static int sixrd_config(struct ip_fw_chain *ch,
	ip_fw3_opheader *op3, struct sockopt_data *sd);

static int sixrd_list(struct ip_fw_chain *ch,
	ip_fw3_opheader *op3, struct sockopt_data *sd);

static int sixrd_stats(struct ip_fw_chain *ch,
	ip_fw3_opheader *op, struct sockopt_data *sd);

static int sixrd_reset_stats(struct ip_fw_chain *ch,
	ip_fw3_opheader *op, struct sockopt_data *sd);


static int sixrd_classify(ipfw_insn *cmd, uint16_t *puidx, uint8_t *ptype);
static void sixrd_update_arg1(ipfw_insn *cmd, uint16_t idx);
static int sixrd_findbyname(struct ip_fw_chain *ch,
	struct tid_info *ti, struct named_object **pno);

static struct named_object * sixrd_findbykidx(
	struct ip_fw_chain *ch, uint16_t idx);
static int sixrd_manage_sets(struct ip_fw_chain *ch,
	uint16_t set, uint8_t new_set, enum ipfw_sets_cmd cmd);

static struct ipfw_sopt_handler	scodes[] = {
	{ IP_FW_6RD_CREATE,	0,	HDIR_BOTH,	sixrd_create },
	{ IP_FW_6RD_DESTROY,	0,	HDIR_SET,	sixrd_destroy },
	{ IP_FW_6RD_CONFIG,	0,	HDIR_BOTH,	sixrd_config },
	{ IP_FW_6RD_LIST,	0,	HDIR_GET,	sixrd_list },
	{ IP_FW_6RD_STATS,	0,	HDIR_GET,	sixrd_stats },
	{ IP_FW_6RD_RESET_STATS,0,	HDIR_SET,	sixrd_reset_stats },
};

static struct opcode_obj_rewrite opcodes[] = {
	{
		.opcode	= O_EXTERNAL_INSTANCE,
		.etlv = IPFW_TLV_EACTION,
		.classifier = sixrd_classify,
		.update = sixrd_update_arg1,
		.find_byname = sixrd_findbyname,
		.find_bykidx = sixrd_findbykidx,
		.manage_sets = sixrd_manage_sets,
	},
};

int
sixrd_init(struct ip_fw_chain *ch, int first)
{
	V_sixrd_eid = ipfw_add_eaction(ch, ipfw_6rd, "6rd");
	if (V_sixrd_eid == 0)
		return (ENXIO);

	IPFW_ADD_SOPT_HANDLER(first, scodes);
	IPFW_ADD_OBJ_REWRITER(first, opcodes);
	return (0);
}

void
sixrd_uninit(struct ip_fw_chain *ch, int last)
{
	if (V_sixrd_debug != 0)
		printf("%s: Foo bar\n", __func__);

	IPFW_DEL_OBJ_REWRITER(last, opcodes);
	IPFW_DEL_SOPT_HANDLER(last, scodes);
	ipfw_del_eaction(ch, V_sixrd_eid);

	V_sixrd_eid = 0;
}

static struct sixrd_cfg *
sixrd_find(struct namedobj_instance *ni, const char *name, uint8_t set)
{
	struct sixrd_cfg *cfg;
	cfg = (struct sixrd_cfg *)ipfw_objhash_lookup_name_type(ni, set,
	    IPFW_TLV_6RD_NAME, name);

	return (cfg);
}

/*
 * Creates new 6rd instance.
 * Data layout (v0)(current):
 * Request: [ ipfw_obj_lheader ipfw_6rd_cfg ]
 *
 * Returns 0 on success
 */
static int
sixrd_create(struct ip_fw_chain *ch, ip_fw3_opheader *op3,
    struct sockopt_data *sd)
{
	ipfw_obj_lheader *olh;
	ipfw_6rd_cfg *uc;
	struct sixrd_cfg *cfg;
	struct namedobj_instance *ni;

	if (sd->valsize != sizeof(*olh) + sizeof(*uc))
		return (EINVAL);

	olh = (ipfw_obj_lheader *)sd->kbuf;
	uc = (ipfw_6rd_cfg *)(olh + 1);

	if (ipfw_check_object_name_generic(uc->name) != 0)
		return (EINVAL);

	if (uc->set >= IPFW_MAX_SETS)
		return (EINVAL);

	// TODO: Check incoming ipfw_sixrd_cfg args. This is the struct that
	// is set up in userspace by the ipfw utility defined in ip_fw_6rd.h


	ni = CHAIN_TO_SRV(ch);
	IPFW_UH_RLOCK(ch);
	if (sixrd_find(ni, uc->name, uc->set) != NULL) {
		IPFW_UH_RUNLOCK(ch);
		return (EEXIST);
	}
	IPFW_UH_RUNLOCK(ch);

	cfg = malloc(sizeof(struct sixrd_cfg), M_IPFW, M_WAITOK | M_ZERO);
	COUNTER_ARRAY_ALLOC(cfg->stats, _6RD_STATS, M_WAITOK);

	strlcpy(cfg->name, uc->name, sizeof(cfg->name));
	cfg->no.name = cfg->name;
	cfg->no.etlv = IPFW_TLV_6RD_NAME;
	cfg->no.set = uc->set;

	cfg->dprefix6	= uc->dprefix6;
	cfg->prefix6	= uc->prefix6;
	cfg->relay4	= uc->relay4;
	cfg->ce4 	= uc->ce4;
	cfg->dprefixlen	= uc->dprefixlen;
	cfg->prefixlen	= uc->prefixlen;
	cfg->masklen	= uc->masklen;

	IPFW_UH_WLOCK(ch);

	if (sixrd_find(ni, uc->name, uc->set) != NULL) {
		IPFW_UH_WUNLOCK(ch);
		COUNTER_ARRAY_FREE(cfg->stats, _6RD_STATS);
		free(cfg, M_IPFW);
		return (EEXIST);
	}

	if (ipfw_objhash_alloc_idx(CHAIN_TO_SRV(ch), &cfg->no.kidx) != 0) {
		IPFW_UH_WUNLOCK(ch);
		COUNTER_ARRAY_FREE(cfg->stats, _6RD_STATS);
		free(cfg, M_IPFW);
		return (ENOSPC);
	}

	ipfw_objhash_add(CHAIN_TO_SRV(ch), &cfg->no);
	SRV_OBJECT(ch, cfg->no.kidx) = cfg;
	IPFW_UH_WUNLOCK(ch);
	return (0);
}

static int
sixrd_destroy(struct ip_fw_chain *ch, ip_fw3_opheader *op3,
    struct sockopt_data *sd)
{
	ipfw_obj_header *oh;
	struct sixrd_cfg *cfg;

	if (sd->valsize != sizeof(*oh))
		return (EINVAL);

	oh = (ipfw_obj_header *)sd->kbuf;
	if (ipfw_check_object_name_generic(oh->ntlv.name) != 0)
		return (EINVAL);

	IPFW_UH_WLOCK(ch);
	cfg = sixrd_find(CHAIN_TO_SRV(ch), oh->ntlv.name, oh->ntlv.set);
	if (cfg == NULL) {
		IPFW_UH_WUNLOCK(ch);
		return (ESRCH);
	}
	if (cfg->no.refcnt > 0) {
		IPFW_UH_WUNLOCK(ch);
		return (EBUSY);
	}

	SRV_OBJECT(ch, cfg->no.kidx) = NULL;
	ipfw_objhash_del(CHAIN_TO_SRV(ch), &cfg->no);
	ipfw_objhash_free_idx(CHAIN_TO_SRV(ch), cfg->no.kidx);
	IPFW_UH_WUNLOCK(ch);

	COUNTER_ARRAY_FREE(cfg->stats, _6RD_STATS);
	free(cfg, M_IPFW);
	return (0);
}

/*
 * Get or change 6rd instance config.
 * Request: [ ipfw_obj_header [ ipfw_6rd_cfg ] ]
 */
static int
sixrd_config(struct ip_fw_chain *chain, ip_fw3_opheader *op,
    struct sockopt_data *sd)
{
	return (EOPNOTSUPP);
}

struct sixrd_dump_arg {
	struct ip_fw_chain *ch;
	struct sockopt_data *sd;
};

static void
sixrd_export_config(struct ip_fw_chain *ch, struct sixrd_cfg *cfg,
    ipfw_6rd_cfg *uc)
{

	uc->dprefix6	= cfg->dprefix6;
	uc->prefix6	= cfg->prefix6;
	uc->relay4	= cfg->relay4;
	uc->ce4 	= cfg->ce4;
	uc->dprefixlen	= cfg->dprefixlen;
	uc->prefixlen	= cfg->prefixlen;
	uc->masklen	= cfg->masklen;

	uc->set = cfg->no.set;
	strlcpy(uc->name, cfg->no.name, sizeof(uc->name));
}

static int
export_config_cb(struct namedobj_instance *ni, struct named_object *no,
    void *arg)
{
	struct sixrd_dump_arg *da = (struct sixrd_dump_arg *)arg;
	ipfw_6rd_cfg *uc;

	uc = (ipfw_6rd_cfg *)ipfw_get_sopt_space(da->sd, sizeof(*uc));
	sixrd_export_config(da->ch, (struct sixrd_cfg *)no, uc);
	return (0);
}

/*
 * Lists all 6RD instances currently available in kernel.
 * Data layout (v0)(current):
 * Request: [ ipfw_obj_lheader ]
 * Reply: [ ipfw_obj_lheader ipfw_6rd_cfg x N ]
 *
 * Returns 0 on success
 */
static int
sixrd_list(struct ip_fw_chain *ch, ip_fw3_opheader *op3,
    struct sockopt_data *sd)
{
	ipfw_obj_lheader *olh;
	struct sixrd_dump_arg da;

	/* Check minimum header size */
	if (sd->valsize < sizeof(ipfw_obj_lheader))
		return (EINVAL);

	olh = (ipfw_obj_lheader *)ipfw_get_sopt_header(sd, sizeof(*olh));

	IPFW_UH_RLOCK(ch);
	olh->count = ipfw_objhash_count_type(CHAIN_TO_SRV(ch),
	    IPFW_TLV_6RD_NAME);
	olh->objsize = sizeof(ipfw_6rd_cfg);
	olh->size = sizeof(*olh) + olh->count * olh->objsize;

	if (sd->valsize < olh->size) {
		IPFW_UH_RUNLOCK(ch);
		return (ENOMEM);
	}
	memset(&da, 0, sizeof(da));
	da.ch = ch;
	da.sd = sd;
	ipfw_objhash_foreach_type(CHAIN_TO_SRV(ch), export_config_cb,
	    &da, IPFW_TLV_6RD_NAME);
	IPFW_UH_RUNLOCK(ch);

	return (0);
}

#define	__COPY_STAT_FIELD(_cfg, _stats, _field)	\
	(_stats)->_field = _6RD_STAT_FETCH(_cfg, _field)
static void
export_stats(struct ip_fw_chain *ch, struct sixrd_cfg *cfg,
    struct ipfw_6rd_stats *stats)
{

	__COPY_STAT_FIELD(cfg, stats, in2ex);
	__COPY_STAT_FIELD(cfg, stats, ex2in);
	__COPY_STAT_FIELD(cfg, stats, dropped);
}

/*
 * Get 6RD statistics.
 * Data layout (v0)(current):
 * Request: [ ipfw_obj_header ]
 * Reply: [ ipfw_obj_header ipfw_obj_ctlv [ uint64_t x N ]]
 *
 * Returns 0 on success
 */
static int
sixrd_stats(struct ip_fw_chain *ch, ip_fw3_opheader *op,
    struct sockopt_data *sd)
{
	struct ipfw_6rd_stats stats;
	struct sixrd_cfg *cfg;
	ipfw_obj_header *oh;
	ipfw_obj_ctlv *ctlv;
	size_t sz;

	sz = sizeof(ipfw_obj_header) + sizeof(ipfw_obj_ctlv) + sizeof(stats);
	if (sd->valsize % sizeof(uint64_t))
		return (EINVAL);
	if (sd->valsize < sz)
		return (ENOMEM);
	oh = (ipfw_obj_header *)ipfw_get_sopt_header(sd, sz);
	if (oh == NULL)
		return (EINVAL);
	if (ipfw_check_object_name_generic(oh->ntlv.name) != 0 ||
	    oh->ntlv.set >= IPFW_MAX_SETS)
		return (EINVAL);
	memset(&stats, 0, sizeof(stats));

	IPFW_UH_RLOCK(ch);
	cfg = sixrd_find(CHAIN_TO_SRV(ch), oh->ntlv.name, oh->ntlv.set);
	if (cfg == NULL) {
		IPFW_UH_RUNLOCK(ch);
		return (ESRCH);
	}
	export_stats(ch, cfg, &stats);
	IPFW_UH_RUNLOCK(ch);

	ctlv = (ipfw_obj_ctlv *)(oh + 1);
	memset(ctlv, 0, sizeof(*ctlv));
	ctlv->head.type = IPFW_TLV_COUNTERS;
	ctlv->head.length = sz - sizeof(ipfw_obj_header);
	ctlv->count = sizeof(stats) / sizeof(uint64_t);
	ctlv->objsize = sizeof(uint64_t);
	ctlv->version = 1;
	memcpy(ctlv + 1, &stats, sizeof(stats));
	return (0);
}

/*
 * Reset 6RD statistics.
 * Data layout (v0)(current):
 * Request: [ ipfw_obj_header ]
 *
 * Returns 0 on success
 */
static int
sixrd_reset_stats(struct ip_fw_chain *ch, ip_fw3_opheader *op,
    struct sockopt_data *sd)
{
	struct sixrd_cfg *cfg;
	ipfw_obj_header *oh;

	if (sd->valsize != sizeof(*oh))
		return (EINVAL);
	oh = (ipfw_obj_header *)sd->kbuf;
	if (ipfw_check_object_name_generic(oh->ntlv.name) != 0 ||
	    oh->ntlv.set >= IPFW_MAX_SETS)
		return (EINVAL);

	IPFW_UH_WLOCK(ch);
	cfg = sixrd_find(CHAIN_TO_SRV(ch), oh->ntlv.name, oh->ntlv.set);
	if (cfg == NULL) {
		IPFW_UH_WUNLOCK(ch);
		return (ESRCH);
	}
	COUNTER_ARRAY_ZERO(cfg->stats, _6RD_STATS);
	IPFW_UH_WUNLOCK(ch);
	return (0);
}

// OBJ opcodes..
//
static int
sixrd_classify(ipfw_insn *cmd, uint16_t *puidx, uint8_t *ptype)
{
	ipfw_insn *icmd;
	icmd = cmd - 1;
	if (icmd->opcode != O_EXTERNAL_ACTION ||
	    icmd->arg1 != V_sixrd_eid)
		return (1);

	*puidx = cmd->arg1;
	*ptype = 0;
	return (0);
}

static void
sixrd_update_arg1(ipfw_insn *cmd, uint16_t idx)
{
	cmd->arg1 = idx;
}

static int
sixrd_findbyname(struct ip_fw_chain *ch, struct tid_info *ti,
    struct named_object **pno)
{
	int err;
	err = ipfw_objhash_find_type(CHAIN_TO_SRV(ch), ti,
	    IPFW_TLV_6RD_NAME, pno);
	return (err);
}

static struct named_object *
sixrd_findbykidx(struct ip_fw_chain *ch, uint16_t idx)
{
	struct namedobj_instance *ni;
	struct named_object *no;

	IPFW_UH_WLOCK_ASSERT(ch);
	ni = CHAIN_TO_SRV(ch);
	no = ipfw_objhash_lookup_kidx(ni, idx);
	KASSERT(no != NULL, ("6RD with index %d not found", idx));
	return (no);
}

static int
sixrd_manage_sets(struct ip_fw_chain *ch, uint16_t set, uint8_t new_set,
    enum ipfw_sets_cmd cmd)
{
	return (ipfw_obj_manage_sets(CHAIN_TO_SRV(ch), IPFW_TLV_6RD_NAME,
	    set, new_set, cmd));
}


/*
 * ipfw external action handler. This is where the packets flow into the 6rd module.
 */
static int
ipfw_6rd(struct ip_fw_chain *chain, struct ip_fw_args *args,
    ipfw_insn *cmd, int *done)
{
//	struct ip6_hdr *ip6;
	struct sixrd_cfg *cfg;
	ipfw_insn *icmd;
	int ret;

	*done = 0; /* try next rule if not matched */
	ret = IP_FW_DENY;
	icmd = cmd + 1;
	if (cmd->opcode != O_EXTERNAL_ACTION ||
		cmd->arg1 != V_sixrd_eid ||
		icmd->opcode != O_EXTERNAL_INSTANCE ||
		(cfg = _6RD_LOOKUP(chain, icmd)) == NULL)
		return (ret);

	/*
	 * We need act as router, so when forwarding is disabled -
	 * do nothing.
	 */
	//if (V_ip6_forwarding == 0 || args->f_id.addr_type != 6)
	// if (V_ip6_forwarding == 0 || V_ip_forwarding == 0)
	//	return (ret);
	

	// code for processing the packet goes here...
	return (ret);
}

//These settings are for century link with a made up ip4 src address that's on my lan
//
//ipfw -f flush
//
//ipfw     6rd SIXRD create prefix 2602::/24 masklen 0 relay 205.171.2.64 ip4src x.x.x.x
//ipfw add 6rd SIXRD ip from <IPv6 delegated prefix> to any in
//ipfw add 6rd SIXRD ip from any to <IPv4 CE addr> in
//
// prefixlen + (32 - masklen) --> delegated prefix length
// 	24 + (32 - 0) --> 56
//
// How may /64 subnets do I have?
// 	64 - 56 --> 8 --> 2^8 == 256

