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
#include <rte_kni.h>
#include <rte_log.h>
#include <rte_malloc.h>


#define MAX_PKT_BURST 32
#define MBUF_POOL_NAME "lb_mbuf_pool"
#define NB_MBUF (8192*16)
#define RTE_TEST_RX_DESC_DEFAULT 128
#define RTE_TEST_TX_DESC_DEFAULT 512
#define RING_SIZE 128
#define CACHE_SIZE 32 
#define MAX_PACKET_SZ           2048



#define KNI_MAX_KTHREAD 32
#define KNI_ENET_HEADER_SIZE    14
#define KNI_ENET_FCS_SIZE       4

#define KNI_US_PER_SECOND       1000000
#define KNI_SECOND_PER_DAY      86400


/* Macros for printing using RTE_LOG */
#define RTE_LOGTYPE_APP RTE_LOGTYPE_USER1


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

struct kni_port_params {
         uint8_t port_id;/* Port ID */
         unsigned lcore_rx; /* lcore ID for RX */
         unsigned lcore_tx; /* lcore ID for TX */
         uint32_t nb_lcore_k; /* Number of lcores for KNI multi kernel threads */
         uint32_t nb_kni; /* Number of KNI devices to be created */
         unsigned lcore_k[KNI_MAX_KTHREAD]; /* lcore ID list for kthreads */
         struct rte_kni *kni[KNI_MAX_KTHREAD]; /* KNI context pointers */
} __rte_cache_aligned;

struct kni_port_params *kni_port_params_array[2];

static void load_balancer(void);
void initialize(void);
static void init_kni(unsigned int);
static void kni_alloc(uint8_t);
static int kni_change_mtu(uint8_t port_id, unsigned new_mtu);
static int kni_config_network_interface(uint8_t port_id, uint8_t if_up);
static int kni_free_kni(uint8_t port_id); 
//static rte_atomic32_t kni_stop = RTE_ATOMIC32_INIT(0);



static int load_balancer_lcore(__attribute__((unused)) void *dummy){
  load_balancer();
  return 0;
}

static int kni_free_kni(uint8_t port_id){
  uint8_t i;
  struct kni_port_params **p = kni_port_params_array;

  if (port_id >= RTE_MAX_ETHPORTS || !p[port_id])
          return -1;

  for (i = 0; i < p[port_id]->nb_kni; i++) {
          rte_kni_release(p[port_id]->kni[i]);
          p[port_id]->kni[i] = NULL;
  }
  rte_eth_dev_stop(port_id);

  return 0;

} 

/* Callback for request of changing MTU */
static int 
kni_change_mtu(uint8_t port_id, unsigned new_mtu)
{
        int ret;
        struct rte_eth_conf conf;

        if (port_id >= rte_eth_dev_count()) {
                RTE_LOG(ERR, APP, "Invalid port id %d\n", port_id);
                return -EINVAL;
        }

        RTE_LOG(INFO, APP, "Change MTU of port %d to %u\n", port_id, new_mtu);

        /* Stop specific port */
        rte_eth_dev_stop(port_id);

        memcpy(&conf, &port_conf, sizeof(conf));
        /* Set new MTU */
        if (new_mtu > ETHER_MAX_LEN)
                conf.rxmode.jumbo_frame = 1;
        else
                conf.rxmode.jumbo_frame = 0;

        /* mtu + length of header + length of FCS = max pkt length */
        conf.rxmode.max_rx_pkt_len = new_mtu + KNI_ENET_HEADER_SIZE +
                                                        KNI_ENET_FCS_SIZE;
        ret = rte_eth_dev_configure(port_id, 1, 1, &conf);
        if (ret < 0) {
                RTE_LOG(ERR, APP, "Fail to reconfigure port %d\n", port_id);
                return ret;
        }
        //fixme
        printf("port_id in kni_change_mtu: %u\n", port_id);
        RTE_LOG(INFO, APP, "port_id in kni_change_mtu: %u\n", port_id);

        /* Restart specific port */
        ret = rte_eth_dev_start(port_id);
        if (ret < 0) {
                RTE_LOG(ERR, APP, "Fail to restart port %d\n", port_id);
                return ret;
        }

        return 0;
}

/* Callback for request of configuring network interface up/down */
static int 
kni_config_network_interface(uint8_t port_id, uint8_t if_up)
{
        int ret = 0;

        if (port_id >= rte_eth_dev_count() || port_id >= RTE_MAX_ETHPORTS) {
                RTE_LOG(ERR, APP, "Invalid port id %d\n", port_id);
                return -EINVAL;
        }

        RTE_LOG(INFO, APP, "Configure network interface of %d %s\n",
                                        port_id, if_up ? "up" : "down");
//fixme
printf("port_id in kni_config_network_interface: %u\n", port_id);

        if (if_up != 0) { /* Configure network interface up */
                rte_eth_dev_stop(port_id);
                ret = rte_eth_dev_start(port_id);
        } else /* Configure network interface down */
                rte_eth_dev_stop(port_id);

        if (ret < 0)
                RTE_LOG(ERR, APP, "Failed to start port %d\n", port_id);

        return ret;
}



static void 
kni_alloc(uint8_t port_id){
  uint8_t i;  
  struct rte_kni *kni;
  struct rte_kni_conf conf;
  struct kni_port_params **params = kni_port_params_array;

  

  for(i=0; i<params[port_id]->nb_kni; i++){
    memset(&conf, 0 , sizeof(conf));
    snprintf(conf.name, RTE_KNI_NAMESIZE, 
              "vEth%u_%u", port_id, i);
    conf.core_id = params[port_id]->lcore_k[i];
    conf.force_bind = 1;
    conf.group_id = (uint16_t) port_id;
    conf.mbuf_size = MAX_PACKET_SZ;

    if (i == 0) {
       struct rte_kni_ops ops;
       struct rte_eth_dev_info dev_info;

       memset(&dev_info, 0, sizeof(dev_info));
       rte_eth_dev_info_get(port_id, &dev_info);
       conf.addr = dev_info.pci_dev->addr;
       conf.id = dev_info.pci_dev->id;

       memset(&ops, 0, sizeof(ops));
       ops.port_id = port_id;
       ops.change_mtu = kni_change_mtu;
       ops.config_network_if = kni_config_network_interface;

       kni = rte_kni_alloc(lb_pktmbuf_pool, &conf, &ops);
    }
    else{
      kni = rte_kni_alloc(lb_pktmbuf_pool, &conf, NULL);
    }
    if(!kni)
        rte_exit(EXIT_FAILURE, "Fail to create kni for "
                                  "port: %d\n", port_id);

    params[port_id]->kni[i] = kni;
  }

}


static void
init_kni(unsigned int n){
  
  uint8_t port_id = portid; 
  
  memset(&kni_port_params_array, 0, sizeof(kni_port_params_array));
  kni_port_params_array[port_id] =
                        rte_zmalloc("KNI_port_params",
                                    sizeof(struct kni_port_params), RTE_CACHE_LINE_SIZE);

  struct kni_port_params **params = kni_port_params_array;
  //hardcoded initialize
  params[port_id]->port_id = port_id;
  params[port_id]->lcore_rx = (uint8_t) 1;
  params[port_id]->lcore_tx = (uint8_t) 1;
  params[port_id]->nb_kni = 2;
  params[port_id]->nb_lcore_k = 2;
  params[port_id]->lcore_k[0] = 1;
  params[port_id]->lcore_k[1] = 2;
  rte_kni_init(n);
  
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
  
  init_kni(2);
  kni_alloc(portid);
   
  n_port = rte_eth_dev_count();
  printf("- - - - - - number of ports %d\n", n_port);

  //load_balancer();
  rte_eal_mp_remote_launch(load_balancer_lcore, NULL, CALL_MASTER);
  RTE_LCORE_FOREACH_SLAVE(lcore_id) {
        if (rte_eal_wait_lcore(lcore_id) < 0){
            printf("eal_wait<0\n");
            return -1;
        }
    }   
  
  n_port = rte_eth_dev_count();
  printf("- - - - - - number of ports %d\n", n_port);
  
  kni_free_kni(portid);
  //rte_eal_remote_launch(load_balancer, NULL, lcore_id);
  //rte_eth_promiscuous_enable(portid);


  return 0;
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

//  ring_rx1 = rte_ring_create("ring_rx1", RING_SIZE, socket_id, 
//                RING_F_SP_ENQ | RING_F_SC_DEQ);            /* single prod, single cons */
  
//  ring_tx1 = rte_ring_create("ring_tx1", RING_SIZE, socket_id, 
//                RING_F_SP_ENQ | RING_F_SC_DEQ);            /* single prod, single cons */
//  ring_rx2 = rte_ring_create("ring_rx2", RING_SIZE, socket_id, 
//                RING_F_SP_ENQ | RING_F_SC_DEQ);            /* single prod, single cons */
  
//  ring_tx2 = rte_ring_create("ring_tx2", RING_SIZE, socket_id, 
//                RING_F_SP_ENQ | RING_F_SC_DEQ);            /* single prod, single cons */


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

static void
load_balancer(void){
  struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
  uint16_t n_pkt;
  /*
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
  */
  while(1){
    n_pkt = rte_eth_rx_burst((uint8_t) portid, 0, 
              pkts_burst, MAX_PKT_BURST);
    n_pkt = n_pkt;
    //printf("n_pkt: %u\n", (unsigned int) n_pkt);

    /*
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
    */ 
    
  }
}
