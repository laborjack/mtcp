/* for io_module_func def'ns */
#include "io_module.h"
#ifndef DISABLE_NETMAP
/* for mtcp related def'ns */
#include "mtcp.h"
/* for errno */
#include <errno.h>
/* for logging */
#include "debug.h"
/* for num_devices_* */
#include "config.h"
/* for netmap definitions */
#define NETMAP_WITH_LIBS
#include "netmap_user.h"
/* for poll */
#include <sys/poll.h>
/*----------------------------------------------------------------------------*/
#define MAX_PKT_BURST			64
#define ETHERNET_FRAME_SIZE		1514
#define MAX_IFNAMELEN			(IF_NAMESIZE + 10)
/*----------------------------------------------------------------------------*/

struct netmap_private_context {
	struct nm_desc *local_nmd[MAX_DEVICES];
	unsigned char snd_pktbuf[MAX_DEVICES][ETHERNET_FRAME_SIZE];
	unsigned char *rcv_pktbuf[MAX_PKT_BURST];
	uint8_t rcv_pkt_len[MAX_PKT_BURST];
	uint16_t snd_pkt_size[MAX_DEVICES];
} __attribute__((aligned(__WORDSIZE)));
/*----------------------------------------------------------------------------*/
void
netmap_init_handle(struct mtcp_thread_context *ctxt)
{
	struct netmap_private_context *npc;
	char ifname[MAX_IFNAMELEN];
	char nifname[MAX_IFNAMELEN];
	int j;

	/* create and initialize private I/O module context */
	ctxt->io_private_context = calloc(1, sizeof(struct netmap_private_context));
	if (ctxt->io_private_context == NULL) {
		TRACE_ERROR("Failed to initialize ctxt->io_private_context: "
			    "Can't allocate memory\n");
		exit(EXIT_FAILURE);
	}
	
	npc = (struct netmap_private_context *)ctxt->io_private_context;

	/* initialize per-thread netmap interfaces  */
	for (j = 0; j < num_devices_attached; j++) {
		if (if_indextoname(devices_attached[j], ifname) == NULL) {
			TRACE_ERROR("Failed to initialize interface %s with ifidx: %d - "
				    "error string: %s\n",
				    ifname, devices_attached[j], strerror(errno));
			exit(EXIT_FAILURE);
		}
		
		if (unlikely(CONFIG.num_cores == 1))
			sprintf(nifname, "netmap:%s", ifname);
		else
			sprintf(nifname, "netmap:%s-%d", ifname, ctxt->cpu);
		
		TRACE_INFO("Opening %s with j: %d\n", nifname, j);

		struct nmreq base_nmd;
		memset(&base_nmd, 0, sizeof(base_nmd));

		npc->local_nmd[j] = nm_open(nifname, &base_nmd, 0, NULL);
		if (npc->local_nmd[j] == NULL) {
			TRACE_ERROR("Unable to open %s: %s\n",
				    nifname, strerror(errno));
			exit(EXIT_FAILURE);
		}
	}
}
/*----------------------------------------------------------------------------*/
int
netmap_link_devices(struct mtcp_thread_context *ctxt)
{
	/* linking takes place during mtcp_init() */
	
	return 0;
}
/*----------------------------------------------------------------------------*/
void
netmap_release_pkt(struct mtcp_thread_context *ctxt, int ifidx, unsigned char *pkt_data, int len)
{
	/* 
	 * do nothing over here - memory reclamation
	 * will take place in dpdk_recv_pkts 
	 */
}
/*----------------------------------------------------------------------------*/
int
netmap_send_pkts(struct mtcp_thread_context *ctxt, int nif)
{
	int pkt_size, idx;
	struct netmap_private_context *npc;
	mtcp_manager_t mtcp;

	npc = (struct netmap_private_context *)ctxt->io_private_context;
	idx = nif;
	pkt_size = npc->snd_pkt_size[idx];
	mtcp = ctxt->mtcp_manager;

	/* assert-type statement */
	if (pkt_size == 0) return 0;

#ifdef NETSTAT
	mtcp->nstat.tx_packets[nif]++;
	mtcp->nstat.tx_bytes[nif] += pkt_size + 24;
#endif

	if (nm_inject(npc->local_nmd[idx], npc->snd_pktbuf[idx], pkt_size) == 0) {
		TRACE_DBG("Failed to send pkt of size %d on interface: %d\n",
			  pkt_size, idx);
	}

	npc->snd_pkt_size[idx] = 0;

	return 1;
}
/*----------------------------------------------------------------------------*/
uint8_t *
netmap_get_wptr(struct mtcp_thread_context *ctxt, int nif, uint16_t pktsize)
{
	struct netmap_private_context *npc;
	int idx = nif;

	npc = (struct netmap_private_context *)ctxt->io_private_context;
	if (npc->snd_pkt_size[idx] != 0)
		netmap_send_pkts(ctxt, nif);

	npc->snd_pkt_size[idx] = pktsize;
	
	return (uint8_t *)npc->snd_pktbuf[idx];
}
/*----------------------------------------------------------------------------*/
int32_t
netmap_recv_pkts(struct mtcp_thread_context *ctxt, int ifidx)
{
	struct netmap_private_context *npc;
	struct nm_desc *d;
	npc = (struct netmap_private_context *)ctxt->io_private_context;
	d = npc->local_nmd[ifidx];

	int p = 0;
	int c, got = 0, ri = d->cur_rx_ring;
	int n = d->last_rx_ring - d->first_rx_ring + 1;
	int cnt = MAX_PKT_BURST;



	for (c = 0; c < n && cnt != got; c++) {
		/* compute current ring to use */
		struct netmap_ring *ring;
		
		ri = d->cur_rx_ring + c;
		if (ri > d->last_rx_ring)
			ri = d->first_rx_ring;
		ring = NETMAP_RXRING(d->nifp, ri);
		for ( ; !nm_ring_empty(ring) && cnt != got; got++) {
			u_int i = ring->cur;
			u_int idx = ring->slot[i].buf_idx;
			npc->rcv_pktbuf[p] = (u_char *)NETMAP_BUF(ring, idx);
			npc->rcv_pkt_len[p] = ring->slot[i].len;
			p++;
			ring->head = ring->cur = nm_ring_next(ring, i);
		}
	}
	d->cur_rx_ring = ri;

	return p;
}
/*----------------------------------------------------------------------------*/
uint8_t *
netmap_get_rptr(struct mtcp_thread_context *ctxt, int ifidx, int index, uint16_t *len)
{
	struct netmap_private_context *npc;
	npc = (struct netmap_private_context *)ctxt->io_private_context;

	*len = npc->rcv_pkt_len[index];
	return (unsigned char *)npc->rcv_pktbuf[index];
}
/*----------------------------------------------------------------------------*/
int32_t
netmap_select(struct mtcp_thread_context *ctxt)
{
	static __thread int flag = 0;
	struct netmap_private_context *npc;

	npc = (struct netmap_private_context *)ctxt->io_private_context;
	struct pollfd pfd = { .fd = npc->local_nmd[0]->fd, .events = POLLIN };

	do { 
		int i = poll(&pfd, 1, 1000);
		if (i > 0 && !(pfd.revents & POLLERR)) {
			flag = 1;
			break;
		}
	} while (flag == 0);
	
	if (pfd.revents & POLLERR) {
		TRACE_ERROR("Poll failed! (cpu: %d\n, err: %d)\n",
			    ctxt->cpu, errno);
		exit(EXIT_FAILURE);
	}

	return 0;
}
/*----------------------------------------------------------------------------*/
void
netmap_destroy_handle(struct mtcp_thread_context *ctxt)
{
}
/*----------------------------------------------------------------------------*/
void
netmap_load_module(void)
{
	/* not needed - all initializations done in netmap_init_handle() */
}
/*----------------------------------------------------------------------------*/
io_module_func netmap_module_func = {
	.load_module		   = netmap_load_module,
	.init_handle		   = netmap_init_handle,
	.link_devices		   = netmap_link_devices,
	.release_pkt		   = netmap_release_pkt,
	.send_pkts		   = netmap_send_pkts,
	.get_wptr   		   = netmap_get_wptr,
	.recv_pkts		   = netmap_recv_pkts,
	.get_rptr	   	   = netmap_get_rptr,
	.select			   = netmap_select,
	.destroy_handle		   = netmap_destroy_handle
};
/*----------------------------------------------------------------------------*/
#else
io_module_func netmap_module_func = {
	.load_module		   = NULL,
	.init_handle		   = NULL,
	.link_devices		   = NULL,
	.release_pkt		   = NULL,
	.send_pkts		   = NULL,
	.get_wptr   		   = NULL,
	.recv_pkts		   = NULL,
	.get_rptr	   	   = NULL,
	.select			   = NULL,
	.destroy_handle		   = NULL
};
/*----------------------------------------------------------------------------*/
#endif /* !DISABLE_NETMAP */
