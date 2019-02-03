#ifndef _IPFW_6RD_H
#define _IPFW_6RD_H

#include "ip_fw_6rd.h"

VNET_DECLARE(uint16_t, sixrd_eid);
#define	V_sixrd_eid		VNET(sixrd_eid)
#define IPFW_TLV_6RD_NAME	IPFW_TLV_EACTION_NAME(V_sixrd_eid) 

#endif
