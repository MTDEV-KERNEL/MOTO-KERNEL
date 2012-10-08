#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,0)
#include <linux/netdevice.h>
#include <linux/module.h>
#include <asm/uaccess.h>
#endif
#include <linux/etherdevice.h>
#include <asm/byteorder.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/udp.h>
#include <linux/in.h>

#include "gdm_wimax.h"
#include "hci.h"
#include "wm_ioctl.h"
#include "netlink_k.h"

#define gdm_wimax_send(n, d, l)	\
	n->phy_dev->send_func(n->phy_dev->priv_dev, d, l, NULL, NULL)
#define gdm_wimax_send_with_cb(n, d, l, c, b)	\
	n->phy_dev->send_func(n->phy_dev->priv_dev, d, l, c, b)
#define gdm_wimax_rcv_with_cb(n, c, b)	\
	n->phy_dev->rcv_func(n->phy_dev->priv_dev, c, b)

static struct {
	int ref_cnt;
	struct sock *sock;
} wm_event;

static u8 gdm_wimax_macaddr[6] = {0x00, 0x0a, 0x3b, 0xf0, 0x01, 0x30};

static void gdm_wimax_ind_fsm_update(struct net_device *dev, fsm_t *fsm);
static void gdm_wimax_ind_if_updown(struct net_device *dev, int if_up);

#if defined(DEBUG_SDU)
static void printk_hex(u8 *buf, u32 size)
{
	int i;

	for (i = 0; i < size; i++) {
		if (i && i % 16 == 0)
			printk("\n%02x ", *buf++);
		else
			printk("%02x ", *buf++);
	}

	printk("\n");
}

static const char *get_protocol_name(u16 protocol)
{
	static char buf[32];
	const char *name = "-";

	switch (protocol) {
		case ETH_P_ARP:
			name = "ARP";
			break;
		case ETH_P_IP:
			name = "IP";
			break;
		case ETH_P_IPV6:
			name = "IPv6";
			break;
	}

	sprintf(buf, "0x%04x(%s)", protocol, name);
	return buf;
}

static const char *get_ip_protocol_name(u8 ip_protocol)
{
	static char buf[32];
	const char *name = "-";

	switch (ip_protocol) {
		case IPPROTO_TCP:
			name = "TCP";
			break;
		case IPPROTO_UDP:
			name = "UDP";
			break;
		case IPPROTO_ICMP:
			name = "ICMP";
			break;
	}

	sprintf(buf, "%u(%s)", ip_protocol, name);
	return buf;
}

static const char *get_port_name(u16 port)
{
	static char buf[32];
	const char *name = "-";

	switch (port) {
		case 67:
			name = "DHCP-Server";
			break;
		case 68:
			name = "DHCP-Client";
			break;
		case 69:
			name = "TFTP";
			break;
	}

	sprintf(buf, "%u(%s)", port, name);
	return buf;
}

static void dump_eth_packet(const char *title, u8 *data, int len)
{
	struct iphdr *ih = NULL;
	struct udphdr *uh = NULL;
	u16 protocol = 0;
	u8 ip_protocol = 0;
	u16 port = 0;

	protocol = (data[12]<<8) | data[13];
	ih = (struct iphdr *) (data+ETH_HLEN);

	if (protocol == ETH_P_IP) {
		uh = (struct udphdr *) ((char *)ih + sizeof(struct iphdr));
		ip_protocol = ih->protocol;
		port = ntohs(uh->dest);
	}
	else if (protocol == ETH_P_IPV6) {
		struct ipv6hdr *i6h = (struct ipv6hdr *) data;
		uh = (struct udphdr *) ((char *)i6h + sizeof(struct ipv6hdr));
		ip_protocol = i6h->nexthdr;
		port = ntohs(uh->dest);
	}

	printk("[%s] len=%d, %s, %s, %s\n",
		title, len,
		get_protocol_name(protocol),
		get_ip_protocol_name(ip_protocol),
		get_port_name(port));

	#if 1
	if (!(data[0] == 0xff && data[1] == 0xff)) {
		if (protocol == ETH_P_IP) {
			printk("     src=%u.%u.%u.%u\n", NIPQUAD(ih->saddr));
		}
		else if (protocol == ETH_P_IPV6) {
			#ifdef NIP6
			printk("     src=%x:%x:%x:%x:%x:%x:%x:%x\n", NIP6(ih->saddr));
			#else
			printk("     src=%pI6\n", &ih->saddr);
			#endif
		}
	}
	#endif

	#if (DUMP_PACKET & DUMP_SDU_ALL)
	printk_hex(data, len);
	#else
		#if (DUMP_PACKET & DUMP_SDU_ARP)
		if (protocol == ETH_P_ARP)
			printk_hex(data, len);
		#endif
		#if (DUMP_PACKET & DUMP_SDU_IP)
		if (protocol == ETH_P_IP || protocol == ETH_P_IPV6)
			printk_hex(data, len);
		#else
			#if (DUMP_PACKET & DUMP_SDU_IP_TCP)
			if (ip_protocol == IPPROTO_TCP)
				printk_hex(data, len);
			#endif
			#if (DUMP_PACKET & DUMP_SDU_IP_UDP)
			if (ip_protocol == IPPROTO_UDP)
				printk_hex(data, len);
			#endif
			#if (DUMP_PACKET & DUMP_SDU_IP_ICMP)
			if (ip_protocol == IPPROTO_ICMP)
				printk_hex(data, len);
			#endif
		#endif
	#endif
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,4,23)
struct net_device *alloc_netdev(int sizeof_priv, const char *mask,
				       void (*setup)(struct net_device *))
{
	struct net_device *dev;
	int alloc_size;

	/* ensure 32-byte alignment of the private area */
	alloc_size = sizeof (*dev) + sizeof_priv + 31;

	dev = (struct net_device *) kmalloc (alloc_size, GFP_KERNEL);
	if (dev == NULL) {
		printk(KERN_ERR "alloc_dev: Unable to allocate device memory.\n");
		return NULL;
	}

	memset(dev, 0, alloc_size);

	if (sizeof_priv)
		netdev_priv(dev) = (void *) (((long)(dev + 1) + 31) & ~31);

	setup(dev);
	strcpy(dev->name, mask);

	return dev;
}

#define free_netdev(dev)		kfree(dev)
#endif

static inline int gdm_wimax_header(struct sk_buff **pskb)
{
	u16 buf[HCI_HEADER_SIZE / sizeof(u16)];
	struct sk_buff *skb = *pskb;
	int ret = 0;

	if (unlikely(skb_headroom(skb) < HCI_HEADER_SIZE)) {
		struct sk_buff *skb2;

		skb2 = skb_realloc_headroom(skb, HCI_HEADER_SIZE);
		if (skb2 == NULL)
			return -ENOMEM;
		if (skb->sk)
			skb_set_owner_w(skb2, skb->sk);
		kfree_skb(skb);
		skb = skb2;
	}

	skb_push(skb, HCI_HEADER_SIZE);
	buf[0] = H2B(WIMAX_TX_SDU);
	buf[1] = H2B(skb->len - HCI_HEADER_SIZE);
	memcpy(skb->data, buf, HCI_HEADER_SIZE);

	*pskb = skb;
	return ret;
}

static void gdm_wimax_event_rcv(struct net_device *dev, u16 type, void *msg, int len)
{
	struct nic *nic = netdev_priv(dev);

	#if defined(DEBUG_HCI)
	u8 *buf = (u8 *) msg;
	u16 hci_cmd =  (buf[0]<<8) | buf[1];
	u16 hci_len = (buf[2]<<8) | buf[3];
	printk("H=>D: 0x%04x(%d)\n", hci_cmd, hci_len);
	#endif

	gdm_wimax_send(nic, msg, len);
}

static int gdm_wimax_event_init(void)
{
	if (wm_event.ref_cnt == 0)
		wm_event.sock = netlink_init(NETLINK_WIMAX, gdm_wimax_event_rcv);

	if (wm_event.sock) {
		wm_event.ref_cnt++;
		return 0;
	}

	printk(KERN_ERR "Creating WiMax Event netlink is failed\n");
	return -1;
}

static void gdm_wimax_event_exit(void)
{
	if (wm_event.sock && --wm_event.ref_cnt == 0) {
		netlink_exit(wm_event.sock);
		wm_event.sock = NULL;
	}
}

static int gdm_wimax_event_send(struct net_device *dev, char *buf, int size)
{
	int idx;

	#if defined(DEBUG_HCI)
	u16 hci_cmd =  ((u8)buf[0]<<8) | (u8)buf[1];
	u16 hci_len = ((u8)buf[2]<<8) | (u8)buf[3];
	printk("D=>H: 0x%04x(%d)\n", hci_cmd, hci_len);
	#endif

	sscanf(dev->name, "wm%d", &idx);
	return netlink_send(wm_event.sock, idx, 0, buf, size);
}

static void tx_complete(void *arg)
{
	struct nic *nic = arg;

	if (netif_queue_stopped(nic->netdev))
		netif_wake_queue(nic->netdev);
}

int gdm_wimax_send_tx(struct sk_buff *skb, struct net_device *dev)
{
	int ret = 0;
	struct nic *nic = netdev_priv(dev);

	ret = gdm_wimax_send_with_cb(nic, skb->data, skb->len, tx_complete, nic);
	if (ret == -ENOSPC) {
		netif_stop_queue(dev);
		ret = 0;
	}

	if (ret) {
		skb_pull(skb, HCI_HEADER_SIZE);
		return ret;
	}
	
	nic->stats.tx_packets++;
	nic->stats.tx_bytes += skb->len - HCI_HEADER_SIZE;
	kfree_skb(skb);
	return ret;
}

static int gdm_wimax_tx(struct sk_buff *skb, struct net_device *dev)
{
	int ret = 0;
	struct nic *nic = netdev_priv(dev);
	fsm_t *fsm = (fsm_t *) nic->sdk_data[SIOC_DATA_FSM].buf;

	#if defined(DEBUG_SDU)
	dump_eth_packet("TX", skb->data, skb->len);
	#endif

	ret = gdm_wimax_header(&skb);
	if (ret < 0) {
		skb_pull(skb, HCI_HEADER_SIZE);
		return ret;
	}

	if (!fsm)
		printk(KERN_ERR "ASSERTION ERROR: fsm is NULL!!\n");
	else if (fsm->m_status != M_CONNECTED) {
		printk(KERN_EMERG "ASSERTION ERROR: Device is NOT ready. status=%d\n",
			fsm->m_status);
		kfree_skb(skb);
		return 0;
	}

#if defined(CONFIG_GDM_QOS)
	ret = gdm_qos_send_hci_pkt(skb, dev);
#else
	ret = gdm_wimax_send_tx(skb, dev);
#endif
	return ret;
}

static int gdm_wimax_set_config(struct net_device *dev, struct ifmap *map)
{
	if (dev->flags & IFF_UP)
		return -EBUSY;

	return 0;
}
 
static void __gdm_wimax_set_mac_addr(struct net_device *dev, char *mac_addr)
{
	u16 hci_pkt_buf[32 / sizeof(u16)];
	u8 *pkt = (u8 *) &hci_pkt_buf[0];
	struct nic *nic = netdev_priv(dev);
	
	/* Since dev is registered as a ethernet device,
	 * ether_setup has made dev->addr_len to be ETH_ALEN
	 */
	memcpy(dev->dev_addr, mac_addr, dev->addr_len);

	/* Let lower layer know of this change by sending SetInformation(MAC Address) */
	hci_pkt_buf[0] = H2B(WIMAX_SET_INFO);	// cmd_evt
	hci_pkt_buf[1] = H2B(8);			// size
	pkt[4] = 0; /* T */
	pkt[5] = 6; /* L */
	memcpy(pkt + 6, mac_addr, dev->addr_len); /* V */

	gdm_wimax_send(nic, pkt, HCI_HEADER_SIZE + 8);
}

/* A driver function */
static int gdm_wimax_set_mac_addr(struct net_device *dev, void *p)
{
	struct sockaddr *addr = p;

	if (netif_running(dev)) return -EBUSY;

	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	__gdm_wimax_set_mac_addr(dev, addr->sa_data);

	return 0;
}

static struct net_device_stats *gdm_wimax_stats(struct net_device *dev)
{
	struct nic *nic = netdev_priv(dev);

	return &nic->stats;
}

static int gdm_wimax_open(struct net_device *dev)
{
	struct nic *nic = netdev_priv(dev);
	fsm_t *fsm = (fsm_t *) nic->sdk_data[SIOC_DATA_FSM].buf;

	netif_start_queue(dev);

	if (fsm && fsm->m_status != M_INIT)
		gdm_wimax_ind_if_updown(dev, 1);
	return 0;
}

static int gdm_wimax_close(struct net_device *dev)
{ 
	struct nic *nic = netdev_priv(dev);
	fsm_t *fsm = (fsm_t *) nic->sdk_data[SIOC_DATA_FSM].buf;

	netif_stop_queue(dev);

	if (fsm && fsm->m_status != M_INIT)
		gdm_wimax_ind_if_updown(dev, 0);
	return 0;
}

static void kdelete(void **buf)
{
	if (buf && *buf) {
		kfree(*buf);
		*buf = NULL;
	}
}

static int gdm_wimax_ioctl_get_data(data_t *dst, data_t *src)
{
	int size;

	size = dst->size < src->size ? dst->size : src->size;
	
	dst->size = size;
	if (src->size) {
		if (!dst->buf)
			return -EINVAL;
		if (copy_to_user(dst->buf, src->buf, size))
			return -EFAULT;
	}
	return 0;
}

static int gdm_wimax_ioctl_set_data(data_t *dst, data_t *src)
{
	if (!src->size) {
		dst->size = 0;
		return 0;
	}

	if (!src->buf)
		return -EINVAL;

	if (!(dst->buf && dst->size == src->size)) {
		kdelete(&dst->buf);
		dst->buf = kmalloc(src->size, GFP_KERNEL);
		if (dst->buf == NULL)
			return -ENOMEM;
	}

	if (copy_from_user(dst->buf, src->buf, src->size)) {
		kdelete(&dst->buf);
		return -EFAULT;
	}
	dst->size = src->size;
	return 0;
}

static void gdm_wimax_cleanup_ioctl(struct net_device *dev)
{
	struct nic *nic = netdev_priv(dev);
	int i;

	for (i = 0; i < SIOC_DATA_MAX; i++)
		kdelete(&nic->sdk_data[i].buf);
}

static void gdm_update_fsm(struct net_device *dev, fsm_t *new_fsm)
{
	struct nic *nic = netdev_priv(dev);
	fsm_t *cur_fsm = (fsm_t *) nic->sdk_data[SIOC_DATA_FSM].buf;

	if (!cur_fsm)
		return;

	if (cur_fsm->m_status != new_fsm->m_status ||
		cur_fsm->c_status != new_fsm->c_status) {
		if (new_fsm->m_status == M_CONNECTED)
			netif_carrier_on(dev);
		else if (cur_fsm->m_status == M_CONNECTED) {
			netif_carrier_off(dev);
			#if defined(CONFIG_GDM_QOS)
			gdm_qos_release_list(nic);
			#endif
		}
		gdm_wimax_ind_fsm_update(dev, new_fsm);
	}
}

static int gdm_wimax_ioctl(struct net_device *dev, struct ifreq *ifr, int cmd)
{
	wm_req_t *req = (wm_req_t *) ifr;
	struct nic *nic = netdev_priv(dev);
	int ret;

	if (cmd != SIOCWMIOCTL)
		return -EOPNOTSUPP;

	switch(req->cmd) {
		case SIOCG_DATA:
		case SIOCS_DATA:
			if (req->data_id >= SIOC_DATA_MAX) {
				printk(KERN_ERR "%s error: data-index(%d) is invalid!!\n",
					__FUNCTION__, req->data_id);
				return -EOPNOTSUPP;
			}
			if (req->cmd == SIOCG_DATA) {
				if ((ret = gdm_wimax_ioctl_get_data(&req->data,
					&nic->sdk_data[req->data_id])) < 0)
					return ret;
			}
			else if (req->cmd == SIOCS_DATA) {
				if (req->data_id == SIOC_DATA_FSM) {
					/*NOTE: gdm_update_fsm should be called
					before gdm_wimax_ioctl_set_data is called*/
					gdm_update_fsm(dev, (fsm_t *) req->data.buf);
				}
				if ((ret = gdm_wimax_ioctl_set_data(&nic->sdk_data[req->data_id],
					&req->data)) < 0)
					return ret;
			}
			break;
		default :
			printk(KERN_ERR "%s: %x unknown ioctl\n", __FUNCTION__, cmd);
			return -EOPNOTSUPP;
	}

	return 0;
}

static void gdm_wimax_prepare_device(struct net_device *dev)
{
	struct nic *nic = netdev_priv(dev);
	u16 buf[32 / sizeof(u16)];
	hci_t *hci = (hci_t *) buf;
	u16 len = 0;
	u32 val = 0;

	#define BIT_MULTI_CS	0
	#define BIT_WIMAX		1
	#define BIT_QOS			2
	#define BIT_AGGREGATION	3
	
	/* GetInformation mac address */
	len = 0;
	hci->cmd_evt = H2B(WIMAX_GET_INFO);
	hci->data[len++] = TLV_T(T_MAC_ADDRESS);
	hci->length = H2B(len);
	gdm_wimax_send(nic, hci, HCI_HEADER_SIZE+len);

	val = (1<<BIT_WIMAX) | (1<<BIT_MULTI_CS);
	#if defined(CONFIG_GDM_QOS)
	val |= (1<<BIT_QOS);
	#endif
	#if defined(CONFIG_AGGREGATE_RECV_PKT)
	val |= (1<<BIT_AGGREGATION);
	#endif

	/* Set capability */
	len = 0;
	hci->cmd_evt = H2B(WIMAX_SET_INFO);
	hci->data[len++] = TLV_T(T_CAPABILITY);
	hci->data[len++] = TLV_L(T_CAPABILITY);
	val = DH2B(val);
	memcpy(&hci->data[len], &val, TLV_L(T_CAPABILITY));
	len += TLV_L(T_CAPABILITY);
	hci->length = H2B(len);
	gdm_wimax_send(nic, hci, HCI_HEADER_SIZE+len);

	printk("GDM WiMax Set CAPABILITY: 0x%08X\n", DB2H(val));
}

static int gdm_wimax_hci_get_tlv(u8 *buf, u8 *T, u16 *L, u8 **V)
{
	#define __U82U16(b)	((u16)((u8*)(b))[0] | ((u16)((u8*)(b))[1] << 8))
	int next_pos;

	*T = buf[0];
	if (buf[1] == 0x82) {
		*L = B2H(__U82U16(&buf[2]));
		next_pos = 1/*type*/+3/*len*/;
	}
	else {
		*L = buf[1];
		next_pos = 1/*type*/+1/*len*/;
	}
	*V = &buf[next_pos];

	next_pos += *L/*length of val*/;
	return next_pos;
}

static int gdm_wimax_get_prepared_info(struct net_device *dev, char *buf, int len)
{
	u8 T, *V;
	u16 L;
	u16 cmd_evt, cmd_len;
	int pos = HCI_HEADER_SIZE;

	cmd_evt = B2H(*(u16 *)&buf[0]);
	cmd_len = B2H(*(u16 *)&buf[2]);

	if (len < cmd_len + HCI_HEADER_SIZE) {
		printk(KERN_ERR "%s: invalid length [%d/%d]\n", __FUNCTION__,
			cmd_len + HCI_HEADER_SIZE, len);
		return -1;
	}

	if (cmd_evt == WIMAX_GET_INFO_RESULT) {
		if (cmd_len < 2) {
			printk(KERN_ERR "%s: len is too short [%x/%d]\n",
				__FUNCTION__, cmd_evt, len);
			return -1;
		}

		pos += gdm_wimax_hci_get_tlv(&buf[pos], &T, &L, &V);
		if (T == TLV_T(T_MAC_ADDRESS)) {
			if (L != dev->addr_len) {
				printk(KERN_ERR "%s Invalid inofrmation result T/L"
					" [%x/%d] \n", __FUNCTION__, T, L);
				return -1;
			}
			printk("MAC change [%02x:%02x:%02x:%02x:%02x:%02x]"
				"->[%02x:%02x:%02x:%02x:%02x:%02x]\n",
				dev->dev_addr[0], dev->dev_addr[1], dev->dev_addr[2],
				dev->dev_addr[3], dev->dev_addr[4], dev->dev_addr[5],
				V[0], V[1], V[2], V[3], V[4], V[5] );
			memcpy(dev->dev_addr, V, dev->addr_len);
			return 1;
		}
	}

	gdm_wimax_event_send(dev, buf, len);
	return 0;
}

static void gdm_wimax_netif_rx(struct net_device *dev, char *buf, int len)
{
	struct nic *nic = netdev_priv(dev);
	struct sk_buff *skb;

	#if defined(DEBUG_SDU)
	dump_eth_packet("RX", buf, len);
	#endif

	skb = dev_alloc_skb(len + 2);
	if (!skb) {
		printk(KERN_ERR "%s: dev_alloc_skb failed!\n", __FUNCTION__);
		return;
	}
	skb_reserve(skb, 2);

	nic->stats.rx_packets++;
	nic->stats.rx_bytes += len;
	
	memcpy(skb_put(skb, len), buf, len);

	skb->dev = dev;
	skb->protocol = eth_type_trans(skb, dev); /* what will happen? */

	if (netif_rx(skb) == NET_RX_DROP)
		printk(KERN_ERR "%s skb dropped\n", __FUNCTION__);
}

#if defined(CONFIG_AGGREGATE_RECV_PKT)
static void gdm_wimax_transmit_aggr_pkt(struct net_device *dev, char *buf, int len)
{
	#define HCI_PADDING_BYTE	4
	#define HCI_RESERVED_BYTE	4
	hci_t *hci;
	int length;
	
	while (len > 0) {
		hci = (hci_t *) buf;

		if (B2H(hci->cmd_evt) != WIMAX_RX_SDU) {
			printk(KERN_ERR "Wrong cmd_evt(0x%04X)\n", B2H(hci->cmd_evt));
			break;
		}

		length = B2H(hci->length);
		gdm_wimax_netif_rx(dev, hci->data, length);

		if (length & 0x3) 
			length += HCI_PADDING_BYTE - (length & 0x3);/*Add padding size*/

		length += HCI_HEADER_SIZE + HCI_RESERVED_BYTE;
		len -= length;
		buf += length;
	}
}
#endif

static void gdm_wimax_transmit_pkt(struct net_device *dev, char *buf, int len)
{
	#if defined(CONFIG_GDM_QOS)
	struct nic *nic = netdev_priv(dev);
	#endif
	u16 cmd_evt, cmd_len;

	/* This code is added for certain rx packet to be ignored. */
	if (len == 0)
		return;

	cmd_evt = B2H(*(u16 *)&buf[0]);
	cmd_len = B2H(*(u16 *)&buf[2]);

	if (len < cmd_len + HCI_HEADER_SIZE) {
		if (len)
			printk(KERN_ERR "%s: invalid length [%d/%d]\n", __FUNCTION__,
				cmd_len + HCI_HEADER_SIZE, len);
		return;
	}

	switch (cmd_evt) {
		#if defined(CONFIG_AGGREGATE_RECV_PKT)
		case WIMAX_RX_SDU_AGGR:
			gdm_wimax_transmit_aggr_pkt(dev, &buf[HCI_HEADER_SIZE], cmd_len);
			break;
		#endif
		case WIMAX_RX_SDU:
			gdm_wimax_netif_rx(dev, &buf[HCI_HEADER_SIZE], cmd_len);
			break;
		#if defined(CONFIG_GDM_QOS)
		case WIMAX_EVT_MODEM_REPORT:
			gdm_recv_qos_hci_packet(nic, buf, len);
			break;
		#endif
		case WIMAX_SDU_TX_FLOW:
			if (buf[4] == 0) {
				if (!netif_queue_stopped(dev))
					netif_stop_queue(dev);
			}
			else if (buf[4] == 1) {
				if (netif_queue_stopped(dev))
					netif_wake_queue(dev);
			}
			break;
		default:
			gdm_wimax_event_send(dev, buf, len);
			break;
	}
}

static void gdm_wimax_ind_fsm_update(struct net_device *dev, fsm_t *fsm)
{
	u16 buf[32 / sizeof(u16)];
	u8 *hci_pkt_buf = (u8 *)&buf[0];

	/* Indicate updating fsm */
	buf[0] = H2B(WIMAX_FSM_UPDATE);
	buf[1] = H2B(sizeof(fsm_t));
	memcpy(&hci_pkt_buf[HCI_HEADER_SIZE], fsm, sizeof(fsm_t));

	gdm_wimax_event_send(dev, hci_pkt_buf, HCI_HEADER_SIZE+sizeof(fsm_t));
}

static void gdm_wimax_ind_if_updown(struct net_device *dev, int if_up)
{
	u16 buf[32 / sizeof(u16)];
	hci_t *hci = (hci_t *) buf;
	unsigned char up_down;

	up_down = if_up ? WIMAX_IF_UP : WIMAX_IF_DOWN;

	/* Indicate updating fsm */
	hci->cmd_evt = H2B(WIMAX_IF_UPDOWN);
	hci->length = H2B(sizeof(up_down));
	hci->data[0] = up_down;

	gdm_wimax_event_send(dev, (char *)hci, HCI_HEADER_SIZE+sizeof(up_down));
}

static void rx_complete(void *arg, void *data, int len)
{
	struct nic *nic = arg;

	gdm_wimax_transmit_pkt(nic->netdev, data, len);
	gdm_wimax_rcv_with_cb(nic, rx_complete, nic);
}

static void prepare_rx_complete(void *arg, void *data, int len)
{
	struct nic *nic = arg;
	int ret;

	ret = gdm_wimax_get_prepared_info(nic->netdev, data, len);
	if (ret == 1)
		gdm_wimax_rcv_with_cb(nic, rx_complete, nic);
	else {
		if (ret < 0)
			printk(KERN_ERR "get_prepared_info failed(%d)\n", ret);
		gdm_wimax_rcv_with_cb(nic, prepare_rx_complete, nic);
		#if 0
		/* Re-prepare WiMax device */
		gdm_wimax_prepare_device(nic->netdev);
		#endif
	}
}

static void start_rx_proc(struct nic *nic)
{
	gdm_wimax_rcv_with_cb(nic, prepare_rx_complete, nic);
}

#ifdef HAVE_NET_DEVICE_OPS
static struct net_device_ops gdm_netdev_ops = {
	.ndo_open				= gdm_wimax_open,
	.ndo_stop				= gdm_wimax_close,
	.ndo_set_config			= gdm_wimax_set_config,
	.ndo_start_xmit			= gdm_wimax_tx,
	.ndo_get_stats			= gdm_wimax_stats,
	.ndo_set_mac_address	= gdm_wimax_set_mac_addr,
	.ndo_do_ioctl			= gdm_wimax_ioctl,
};
#endif

int register_wimax_device(struct phy_dev *phy_dev)
{
	struct nic *nic = NULL;
	struct net_device *dev;
	int ret;

	dev = (struct net_device *)alloc_netdev(sizeof(*nic), "wm%d", ether_setup); 
	
    printk(KERN_INFO "%s\n", __func__);  //SW2-CONN-EC-WiMAX_Log-01+
	if (dev == NULL) {
		printk(KERN_ERR "alloc_etherdev failed\n");
		return -ENOMEM;
	}

	dev->mtu = 1400;
#ifdef HAVE_NET_DEVICE_OPS
	dev->netdev_ops = &gdm_netdev_ops;
#else
	dev->open = gdm_wimax_open;
	dev->stop = gdm_wimax_close;
	dev->set_config = gdm_wimax_set_config;
	dev->hard_start_xmit = gdm_wimax_tx;
	dev->get_stats = gdm_wimax_stats;
	dev->set_mac_address = gdm_wimax_set_mac_addr;
	dev->do_ioctl = gdm_wimax_ioctl;
#endif
	dev->flags &= ~IFF_MULTICAST;
	memcpy(dev->dev_addr, gdm_wimax_macaddr, sizeof(gdm_wimax_macaddr));

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,24)
	SET_MODULE_OWNER(dev);
#endif

	nic = netdev_priv(dev);
	memset(nic, 0, sizeof(*nic));

	nic->netdev = dev;
	nic->phy_dev = phy_dev;
	phy_dev->netdev = dev;

	/* event socket init */
	if ((ret = gdm_wimax_event_init()) < 0) {
		printk(KERN_ERR "Cannot create event.\n");
		goto cleanup;
	}

	ret = register_netdev(dev);
	if (ret)
		goto cleanup;

	netif_carrier_off(dev);

#ifdef CONFIG_GDM_QOS
	gdm_qos_init(nic);
#endif

	start_rx_proc(nic);

	/* Prepare WiMax device */
	gdm_wimax_prepare_device(dev); 

	return 0;

cleanup:
	printk(KERN_ERR "register_netdev failed\n");
	free_netdev(dev);
	return ret;
}

void unregister_wimax_device(struct phy_dev *phy_dev)
{
	struct nic *nic = netdev_priv(phy_dev->netdev);
	fsm_t *fsm = (fsm_t *) nic->sdk_data[SIOC_DATA_FSM].buf;

	if (fsm)
		fsm->m_status = M_INIT;
	unregister_netdev(nic->netdev);

	gdm_wimax_event_exit();

#if defined(CONFIG_GDM_QOS)
	gdm_qos_release_list(nic);
#endif

	gdm_wimax_cleanup_ioctl(phy_dev->netdev);

	free_netdev(nic->netdev);
}
