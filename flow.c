/*
 * demo rte_flow default rule
 * Copyright(c) 2019 Microsoft Corporation
 * All rights reserved.
 *
 * Creates queues each to receive a different UDP port
 *
 * Usage:
 *    udp-demo EAL_args -- [OPTIONS] PORTS
 */

#include <getopt.h>                      // for getopt_long, no_argument
#include <inttypes.h>                    // for PRIu64, PRId64
#include <signal.h>                      // for signal, SIGINT, SIGTERM
#include <stdbool.h>                     // for bool, false, true
#include <stdint.h>                      // for UINT16_MAX
#include <stdio.h>                       // for printf, NULL, fflush, fprintf
#include <stdlib.h>                      // for EXIT_FAILURE, calloc, exit
#include <sys/time.h>                    // for CLOCK_MONOTONIC
#include <time.h>                        // for clock_gettime

#include <rte_common.h>                  // for rte_exit, rte_align32pow2
#include <rte_ethdev.h>                  // for rte_eth_dev_info, ETH_MQ_RX_...
#include <rte_ether.h>                   // for rte_ether_format_addr, RTE_E...
#include <rte_flow.h>                    // for RTE_FLOW_ACTION_TYPE_END
#include <rte_lcore.h>                   // for rte_lcore_count, rte_get_mas...
#include <rte_mbuf.h>                    // for rte_pktmbuf_free, rte_pktmbu...
#include <rte_timer.h>                   // for rte_timer_init, rte_timer_ma...
#include <rte_eal.h>                     // for rte_eal_cleanup, rte_eal_init
#include <rte_errno.h>                   // for rte_strerror, per_lcore__rte...
#include <rte_mempool.h>                 // for rte_mempool_calc_obj_size

#include "rte_flow_dump.h"
#include "pkt_dump.h"

static unsigned int num_ports;
static uint16_t *udp_ports;

#define MEMPOOL_CACHE 256
#define RX_DESC_DEFAULT 256
#define TX_DESC_DEFAULT 512
#define MAX_PKT_BURST 32
#define MAX_RX_QUEUE 64

#define STAT_INTERVAL 10
#define PKTMBUF_POOL_RESERVED 128

static bool flow_dump = false;
static unsigned int details;
static uint32_t ticks_us;
static struct rte_timer stat_timer;

struct lcore_conf {
	uint16_t n_rx;
	struct rx_queue {
		uint16_t port_id;
		uint16_t queue_id;
		uint64_t rx_packets;
	} rx_queue_list[MAX_RX_QUEUE];
} __rte_cache_aligned;
static struct lcore_conf lcore_conf[RTE_MAX_LCORE];

static struct rte_mempool *mb_pool;

static struct rte_eth_conf port_conf = {
	.rxmode = {
		.mq_mode	= ETH_MQ_RX_NONE,
		.max_rx_pkt_len = RTE_ETHER_MAX_LEN,
		.offloads	= DEV_RX_OFFLOAD_CHECKSUM,
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
};

static volatile bool running = true;

static void port_config(uint16_t portid, uint16_t ntxq, uint16_t nrxq)
{
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf txq_conf;
	struct rte_eth_rxconf rxq_conf;
	uint16_t nb_rxd = RX_DESC_DEFAULT;
	uint16_t nb_txd = TX_DESC_DEFAULT;
	uint16_t q;
	int r;

	r = rte_eth_dev_info_get(portid, &dev_info);
	if (r < 0)
		rte_exit(EXIT_FAILURE,
			 "Could not get device information for port %u\n",
			 portid);

	if (ntxq > dev_info.max_tx_queues)
		rte_exit(EXIT_FAILURE, "Not enough transmit queues %u > %u\n",
			 ntxq, dev_info.max_tx_queues);

	if (nrxq > dev_info.max_rx_queues)
		rte_exit(EXIT_FAILURE, "Not enough receive queues %u > %u\n",
			 nrxq, dev_info.max_rx_queues);

	printf("Configure %u Tx and %u Rx queues\n", ntxq, nrxq);

	r = rte_eth_dev_configure(portid, nrxq, ntxq, &port_conf);
	if (r < 0)
		rte_exit(EXIT_FAILURE,
			 "Cannot configure device: err=%d port=%u\n", r,
			 portid);

	r = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd, &nb_txd);
	if (r < 0)
		rte_exit(
			EXIT_FAILURE,
			"Cannot adjust number of descriptors: err=%d, port=%d\n",
			r, portid);

	txq_conf = dev_info.default_txconf;
	rxq_conf = dev_info.default_rxconf;

	txq_conf.offloads = port_conf.txmode.offloads;
	rxq_conf.offloads = port_conf.rxmode.offloads;

	for (q = 0; q < ntxq; q++) {
		r = rte_eth_tx_queue_setup(portid, q, nb_txd, 0, &txq_conf);
		if (r < 0)
			rte_exit(EXIT_FAILURE,
				 "rte_eth_tx_queue_setup failed %d\n", r);
	}

	for (q = 0; q < nrxq; q++) {
		r = rte_eth_rx_queue_setup(portid, q, nb_rxd, 0, &rxq_conf,
					   mb_pool);
		if (r < 0)
			rte_exit(EXIT_FAILURE,
				 "rte_eth_rx_queue_setup failed %d\n", r);
	}

	r = rte_eth_dev_start(portid);
	if (r < 0)
		rte_exit(EXIT_FAILURE, "Start failed: err=%d\n", r);
}

/* Mask to match any source but a specific destination port */
static const struct rte_flow_item_udp udp_dst_port_mask = {
	.hdr.dst_port = RTE_BE16(0xffff),
};

static const struct rte_flow_item_ipv4 ipv4_any_addr = {
	.hdr = {
		.src_addr = 0,
		.dst_addr = 0,
	}
};

static const struct rte_flow_item_eth eth_proto_mask = {
	.type = RTE_BE16(0xffff),
};

static const struct rte_flow_item_eth eth_proto_ipv4 = {
	.type = RTE_BE16(RTE_ETHER_TYPE_IPV4),
};

/* Configure a queue to match a particular UDP port */
static void flow_configure(uint16_t portid, uint16_t id, uint16_t q,
			   uint16_t udp_port)
{
	struct rte_flow_attr attr = {
		.group = 0,
		.priority = 1,
		.ingress = 1,
	};
	struct rte_flow_item_udp udp_flow = {
		.hdr.dst_port = RTE_BE16(udp_port),
	};
	struct rte_flow_item patterns[] = {
		{
			.type = RTE_FLOW_ITEM_TYPE_ETH,
			.mask = &eth_proto_mask,
			.spec = &eth_proto_ipv4,
		},
		{
			.type = RTE_FLOW_ITEM_TYPE_IPV4,
			.mask = &ipv4_any_addr,
			.spec = &ipv4_any_addr,
		},
		{
			.type = RTE_FLOW_ITEM_TYPE_UDP,
			.spec = &udp_flow,
			.last = NULL, /* not a range */
			.mask = &udp_dst_port_mask,
		},
		{
			.type = RTE_FLOW_ITEM_TYPE_END,
		},
	};
	struct rte_flow_action_queue queue_action = {
		.index = q,
	};
	struct rte_flow_action actions[] = {
		{
			.type = RTE_FLOW_ACTION_TYPE_QUEUE,
			.conf = &queue_action,
		},
		{ .type = RTE_FLOW_ACTION_TYPE_END },
	};
	struct rte_flow_error err;

	printf("Creating flow %u [%u] with queue %u\n", id, udp_port, q);

	if (flow_dump)
		rte_flow_dump(stdout, &attr, patterns, actions);

	if (rte_flow_create(portid, &attr, patterns, actions, &err) == NULL)
		rte_exit(EXIT_FAILURE,
			 "flow create failed: %s\n error type %u %s\n",
			 rte_strerror(rte_errno), err.type, err.message);
}

static uint64_t time_monotonic(void)
{
	static uint64_t start;
	struct timespec ts;
	uint64_t ns;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	ns = ts.tv_sec * NS_PER_S + ts.tv_nsec;

	if (start)
		return ns - start;
	start = ns;
	return 0;
}

static void dump_rx_pkt(uint16_t portid, uint16_t queueid,
			struct rte_mbuf *pkts[], uint16_t n)
{
	static unsigned int pktno;
	uint64_t us = time_monotonic() / 1000;
	uint16_t i;

	for (i = 0; i < n; i++) {
		printf("[%u:%u] ", portid, queueid);
		if (details > 1)
			printf("%-6u %" PRId64 ".%06" PRId64, ++pktno,
			       us / US_PER_S, us % US_PER_S);

		pkt_print(pkts[i]);
	}
	fflush(stdout);
}

static void show_stats(struct rte_timer *tm __rte_unused, void *arg)
{
	unsigned long nrxq = (unsigned long)arg;
	unsigned int q, i, lcore_id;
	struct rte_eth_stats stats;

	rte_eth_stats_get(0, &stats);

	printf("%" PRIu64 "/%" PRIu64, stats.ipackets, stats.ibytes);

	for (q = 0; q < nrxq; q++) {
		RTE_LCORE_FOREACH(lcore_id) {
			struct lcore_conf *conf = &lcore_conf[lcore_id];

			for (i = 0; i < conf->n_rx; i++) {
				struct rx_queue *rxq = &conf->rx_queue_list[i];

				if (rxq->queue_id == q) {
					printf(" %u@%u:%" PRIu64, q, lcore_id,
					       rxq->rx_packets);
					break;
				}
			}
		}
	}
	printf("\n");
	fflush(stdout);
}

static unsigned int rx_poll(struct rx_queue *rxq)
{
	uint16_t portid = rxq->port_id;
	uint16_t queueid = rxq->queue_id;
	struct rte_mbuf *pkts[MAX_PKT_BURST];
	unsigned int i, n;

	n = rte_eth_rx_burst(portid, queueid, pkts, MAX_PKT_BURST);
	if (n == 0)
		return 0;

	rxq->rx_packets += n;
	if (details)
		dump_rx_pkt(portid, queueid, pkts, n);

	for (i = 0; i < n; i++)
		rte_pktmbuf_free(pkts[i]);

	return n;
}

static int rx_thread(void *arg __rte_unused)
{
	unsigned int core_id = rte_lcore_id();
	struct lcore_conf *c = &lcore_conf[core_id];

	/* no need for rx thread if no queues  */
	if (c->n_rx == 0)
		return 0;

	while (running) {
		unsigned int i, total = 0;

		rte_timer_manage();

		for (i = 0; i < c->n_rx; i++)
			total += rx_poll(&c->rx_queue_list[i]);
	}

	return 0;
}

static void print_mac(uint16_t portid)
{
	struct rte_ether_addr eth_addr;
	char buf[RTE_ETHER_ADDR_FMT_SIZE];

	rte_eth_macaddr_get(portid, &eth_addr);
	rte_ether_format_addr(buf, sizeof(buf), &eth_addr);

	printf("Initialized port %u: MAC: %s\n", portid, buf);
}

static void usage(const char *argv0)
{
	printf("Usage: %s [EAL options] -- [OPTIONS] PORT(s) ...\n"
	       "  -f,--flow      flow dump\n"
	       "  -v,--details   print packet details\n",
	       argv0);
	exit(1);
}

static const struct option longopts[] = { { "flow", no_argument, 0, 'f' },
					  { 0 } };

static void parse_args(int argc, char **argv)
{
	unsigned int i;
	int opt;

	while ((opt = getopt_long(argc, argv, "fv", longopts, NULL)) != EOF) {
		switch (opt) {
		case 'f':
			flow_dump = true;
			break;
		case 'v':
			++details;
			break;
		default:
			fprintf(stderr, "Unknown option\n");
			usage(argv[0]);
		}
	}

	/* Additional arguments are UDP ports  */
	num_ports = argc - optind;
	udp_ports = calloc(sizeof(uint16_t), num_ports);
	for (i = 0; i < num_ports; i++) {
		unsigned long l;

		l = strtoul(argv[optind + i], NULL, 0);
		if (l == 0 || l > UINT16_MAX)
			rte_exit(EXIT_FAILURE, "Invalid port: %s\n",
				 argv[optind + i]);

		udp_ports[i] = l;
	}
}

static void signal_handler(int signum)
{
	printf("\n\nSignal %d received, preparing to exit...\n", signum);

	running = false;
}

static void assign_queues(uint16_t portid, uint16_t nrxq)
{
	static int lcore = -1;
	uint16_t q;

	for (q = 0; q < nrxq; q++) {
		struct lcore_conf *c;
		struct rx_queue *rxq;

		lcore = rte_get_next_lcore(lcore, 0, 1);
		c = &lcore_conf[lcore];
		if (c->n_rx >= MAX_RX_QUEUE)
			rte_exit(EXIT_FAILURE,
				 "Too many rx queue already on core %u\n",
				 lcore);

		rxq = c->rx_queue_list + c->n_rx++;
		rxq->port_id = portid;
		rxq->queue_id = q;
	}
}

int main(int argc, char **argv)
{
	unsigned int i, num_mbufs, obj_size;
	unsigned int ntxq, nrxq, nport, port;
	int r;

	r = rte_eal_init(argc, argv);
	if (r < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");

	parse_args(argc - r, argv + r);

	ntxq = rte_lcore_count();
	nrxq = num_ports + 1;

	ticks_us = rte_get_tsc_hz() / US_PER_S;

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	nport = rte_eth_dev_count_avail();
	if (nport == 0)
		rte_exit(EXIT_FAILURE, "No ports present\n");

	num_mbufs =
		nport * (rte_align32pow2(nrxq * RX_DESC_DEFAULT * 3) +
			 ntxq * (TX_DESC_DEFAULT + MAX_PKT_BURST) +
			 rte_lcore_count() * (MEMPOOL_CACHE + MAX_PKT_BURST) +
			 PKTMBUF_POOL_RESERVED);

	/* rte_pktmbuf_pool_create is optimum with 2^q - 1 */
	num_mbufs = rte_align32pow2(num_mbufs + 1) - 1;

	mb_pool = rte_pktmbuf_pool_create("mb_pool", num_mbufs, MEMPOOL_CACHE,
					  0, RTE_MBUF_DEFAULT_BUF_SIZE, 0);
	if (!mb_pool)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

	obj_size =
		rte_mempool_calc_obj_size(RTE_MBUF_DEFAULT_BUF_SIZE, 0, NULL);
	printf("mbuf pool %u of %u bytes = %uMb\n", num_mbufs, obj_size,
	       (num_mbufs * obj_size) / (1024 * 1024));

	rte_timer_subsystem_init();

	RTE_ETH_FOREACH_DEV(port) {
		port_config(port, ntxq, nrxq);
		print_mac(port);

		/* q is the queue, starts after default queue */
		for (i = 0; i + 1 < nrxq; i++)
			flow_configure(port, i, i + 1, udp_ports[i]);

		assign_queues(port, nrxq);
	}

	if (!details) {
		rte_timer_init(&stat_timer);
		rte_timer_reset(&stat_timer, STAT_INTERVAL * rte_get_timer_hz(),
				PERIODICAL, rte_get_master_lcore(), show_stats,
				(void *)(unsigned long)nrxq);

		printf("\n%-14s: %8s/%-10s | per-queue\n", "Time", "Packets",
		       "Bytes");
	}

	r = rte_eal_mp_remote_launch(rx_thread, NULL, CALL_MASTER);
	if (r < 0)
		rte_exit(EXIT_FAILURE, "cannot launch cores");

	RTE_ETH_FOREACH_DEV(port)
	{
		rte_eth_dev_stop(port);
		rte_eth_dev_close(port);
	}

	rte_mempool_free(mb_pool);
	rte_eal_cleanup();
	return 0;
}
