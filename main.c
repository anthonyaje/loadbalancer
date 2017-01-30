#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <ctype.h>
#include <errno.h>

#include <rte_mbuf.h>
#include <rte_ring.h>
#include <rte_ethdev.h>
#include <rte_eth_ring.h>
#include <rte_mempool.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_per_lcore.h>


#define MAX_PKT_BURST 32
#define MBUF_POOL_NAME "lb_mbuf_pool"
#define NB_MBUF 8192
#define RTE_TEST_RX_DESC_DEFAULT 128
#define RTE_TEST_TX_DESC_DEFAULT 512
#define RING_SIZE 128
#define CACHE_SIZE 32 

static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

struct rte_mempool* lb_pktmbuf_pool; 
struct rte_ring* ring_rx1, *ring_rx2;
struct rte_ring* ring_tx1, *ring_tx2;
int lcore_id;
uint8_t n_port, portid;

static const struct rte_eth_conf port_conf = {
    .rxmode = {
        .split_hdr_size = 0,
        .header_split   = 0, /**< Header Split disabled */
        .hw_ip_checksum = 0, /**< IP checksum offload disabled */
        .hw_vlan_filter = 0, /**< VLAN filtering disabled */
        .jumbo_frame    = 0, /**< Jumbo Frame Support disabled */
        .hw_strip_crc   = 0, /**< CRC stripped by hardware */
    },
    .txmode = {
        .mq_mode = ETH_MQ_TX_NONE,
    },
};

static void load_balancer(void);
void initialize(void);

static int load_balancer_lcore(__attribute__((unused)) void *dummy){
  load_balancer();
  return 0;
}


static void
load_balancer(void){
  struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
  uint16_t n_pkt;
  int ret;
  int new_port1, new_port2;
  int flip = 1;
  unsigned socket_id_1, socket_id_2; 
  uint8_t n_port;

  new_port1 = rte_eth_from_rings("lb_veth1", &ring_rx1, 1, &ring_tx1, 1, 0);
  new_port2 = rte_eth_from_rings("lb_veth2", &ring_rx2, 1, &ring_tx2, 1, 0);

  socket_id_1 = rte_eth_dev_socket_id(new_port1);
  socket_id_2 = rte_eth_dev_socket_id(new_port2);
  
  ret = rte_eth_dev_configure(new_port1, 1, 1, &port_conf);
  if(ret < 0){
    rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
                  ret, (unsigned) new_port1);
  }
  ret = rte_eth_dev_configure(new_port2, 1, 1, &port_conf);
  if(ret < 0){
    rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
                  ret, (unsigned) new_port2);
  }

  rte_eth_rx_queue_setup(new_port1, 0, 32, 0, NULL, lb_pktmbuf_pool);
  rte_eth_tx_queue_setup(new_port1, 0, 32, 0, NULL);

  rte_eth_rx_queue_setup(new_port2, 0, 32, 0, NULL, lb_pktmbuf_pool);
  rte_eth_tx_queue_setup(new_port2, 0, 32, 0, NULL);

  printf(" - - - -  -  socketid %u | newport1 - - - -  - %u \n", socket_id_1, new_port1);
  printf(" - - - -  -  socketid %u | newport2 - - - -  - %u \n", socket_id_2, new_port2);

  n_port = rte_eth_dev_count();
  printf("- - - - - - number of ports %d\n", n_port);

  ret = rte_eth_dev_start(new_port1);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
              ret, (unsigned) new_port1);
  ret = rte_eth_dev_start(new_port2);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
              ret, (unsigned) new_port2);

  while(1){
    n_pkt = rte_eth_rx_burst((uint8_t) portid, 0, 
              pkts_burst, MAX_PKT_BURST);

    if(flip){
      ret = rte_eth_tx_burst(new_port1,  (uint16_t) 0, pkts_burst, n_pkt);
      if (unlikely(ret < n_pkt)) {
          do {
              rte_pktmbuf_free(pkts_burst[ret]);
          } while (++ret < n_pkt);
      }
      flip = 0;
    }
    else{  
      ret = rte_eth_tx_burst(new_port2,  (uint16_t) 0, pkts_burst, n_pkt);
      if (unlikely(ret < n_pkt)) {
          do {
              rte_pktmbuf_free(pkts_burst[ret]);
          } while (++ret < n_pkt);
      }
      flip = 1;
    } 
    
  }

}

void initialize(void){
  int ret;
  //lcore_id = rte_get_next_lcore(-1, 0, 0);
  unsigned socket_id;

  n_port = rte_eth_dev_count();
  if(n_port == 0)
    rte_exit(EXIT_FAILURE, "No Ethernet ports - bye\n");
  else{
    // for simplicity just use the fist port
    portid = 0;
  }
  socket_id = rte_eth_dev_socket_id(portid);
  printf(" - - - -  -  socketid %u | portid - - - -  - %u \n", socket_id, portid);
  
  lb_pktmbuf_pool = rte_pktmbuf_pool_create(MBUF_POOL_NAME, NB_MBUF, 
                            CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, socket_id); 
  if(lb_pktmbuf_pool == NULL)
    rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

  ring_rx1 = rte_ring_create("ring_rx1", RING_SIZE, socket_id, 
                RING_F_SP_ENQ | RING_F_SC_DEQ);            /* single prod, single cons */
  
  ring_tx1 = rte_ring_create("ring_tx1", RING_SIZE, socket_id, 
                RING_F_SP_ENQ | RING_F_SC_DEQ);            /* single prod, single cons */
  ring_rx2 = rte_ring_create("ring_rx2", RING_SIZE, socket_id, 
                RING_F_SP_ENQ | RING_F_SC_DEQ);            /* single prod, single cons */
  
  ring_tx2 = rte_ring_create("ring_tx2", RING_SIZE, socket_id, 
                RING_F_SP_ENQ | RING_F_SC_DEQ);            /* single prod, single cons */


  ret = rte_eth_dev_configure(portid, 1, 1, &port_conf);
  if(ret < 0){
    rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
                  ret, (unsigned) portid);
  }
  
  ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd, rte_eth_dev_socket_id(portid),
                                  NULL, lb_pktmbuf_pool); 
  if(ret < 0){
    rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err%d, port=%u\n", 
                ret, (unsigned) portid);
  }
    
  ret = rte_eth_tx_queue_setup(portid, 0, nb_txd, rte_eth_dev_socket_id(portid), NULL);
  if(ret <0){
    rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
                ret, (unsigned) portid);
  }

  ret = rte_eth_dev_start(portid);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
              ret, (unsigned) portid);
}

int main(int argc, char** argv){
  int ret;

  ret = rte_eal_init(argc, argv);
  if(ret < 0){
    rte_panic("Cannot init EAL\n");
  }
  argc -= ret;
  argv += ret;  

  initialize();  
  
  //load_balancer();
  rte_eal_mp_remote_launch(load_balancer_lcore, NULL, CALL_MASTER);
  RTE_LCORE_FOREACH_SLAVE(lcore_id) {
        if (rte_eal_wait_lcore(lcore_id) < 0){
            printf("eal_wait<0\n");
            return -1;
        }
    }   
  //rte_eal_remote_launch(load_balancer, NULL, lcore_id);
  //rte_eth_promiscuous_enable(portid);


  return 0;
}
