#ifndef _NETINET6_IP_FW_6RD_H
#define _NETINET6_IP_FW_6RD_H

struct ipfw_6rd_stats {
	uint64_t	in2ex;		/* Int->Ext packets translated */
	uint64_t	ex2in;		/* Ext->Int packets translated */
	uint64_t	dropped;	/* dropped due to some errors */
	uint64_t	reserved[5];
};

typedef struct _ipfw_6rd_cfg {
	char		name[64];	/* instance name */
	struct in6_addr dprefix6;	/* Delegated Prefix (computed) */
	struct in6_addr	prefix6;	/* ISP prefix */
	struct in_addr	relay4;		/* ISP IPv4 border relay */
	struct in_addr	ce4;		/* CE IPv4 edge address */
	uint32_t	spare1;
	uint8_t		dprefixlen;	/* Delegated Prefix len (computed) */
	uint8_t		prefixlen;	/* ISP prefix length */
	uint8_t		masklen;	/* ISP IPv4 mask length */
	uint8_t		set;		/* Named instance set [0..31] */
} ipfw_6rd_cfg;

#endif
