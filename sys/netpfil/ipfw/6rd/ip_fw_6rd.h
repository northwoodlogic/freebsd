#ifndef	_IP_FW_6RD_H_
#define	_IP_FW_6RD_H_

VNET_DECLARE(int, sixrd_debug);
#define	V_sixrd_debug		VNET(sixrd_debug)

int	sixrd_init(struct ip_fw_chain *ch, int first);
void	sixrd_uninit(struct ip_fw_chain *ch, int last);

#endif
