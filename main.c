/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2014 Intel Corporation
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/queue.h>
#include <stdbool.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_memory.h>
#include <rte_launch.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_lcore.h>
#include <rte_debug.h>
#include <rte_ring.h>

#include "timer.h"

#define POOL_NAME "msg_pool"
#define NODE_NUM 2
#define DATA_NUM (1 << 20)
#define DATA_BATCH 32
#define RING_SZ (1 << 10)

//#define DBG_ON 

#ifdef DBG_ON
#define LOG_DBG(format, ...) fprintf(stdout, format, ##__VA_ARGS__)
#else
#define LOG_DBG(format, ...) {}
#endif

#define LOG_INFO(format, ...) fprintf(stdout, format, ##__VA_ARGS__)

bool force_quit = false;

struct Node{
  struct rte_ring *_ring_in, *_ring_out;
  unsigned int node_id;
};

struct RecvParam{
  struct rte_ring *main_out;
  unsigned int  data_max;
};

struct Node nodes_list[NODE_NUM];
struct rte_ring *main_in, *main_out;
int* raw_data[DATA_NUM];
void *input_buffer[DATA_BATCH];
void *output_buffer[DATA_BATCH];

/* timer */
TIMER_INIT();

static int
lcore_recv(__attribute__((unused)) void *arg)
{
  struct RecvParam *recv_conf = (struct RecvParam *)arg;
  unsigned int rcv_tot = 0, temp_rcv = 0;
  int *data[DATA_BATCH];

  while(rcv_tot < recv_conf->data_max){
     temp_rcv = rte_ring_dequeue_bulk(recv_conf->main_out, (void**)data, DATA_BATCH, NULL);
     if(temp_rcv > 0){
        rcv_tot += temp_rcv;
     }
  }
  
  TIMER_DOWN();

  //LOG_INFO("Receiver dequeue %u msgs\n", rcv_tot);
  LOG_INFO("MP : Speed = %.3lf Mpps\n", (double)rcv_tot / ((double)dura / 1000000000) / (1000*1000));
  return 0;
}

static int
lcore_calculate(__attribute__((unused)) void *arg)
{
  unsigned lcore_id;
  struct Node *m_node = NULL;
  int *data[DATA_BATCH];
  struct rte_ring *in_r = NULL;
  struct rte_ring *out_r = NULL;
  unsigned int rcv_nb, snd_nb, to_read, did;

	lcore_id = rte_lcore_id();
	//LOG_INFO("Core#%u is ready\n", lcore_id);
  m_node = &(nodes_list[lcore_id - 1]);
  in_r = m_node->_ring_in;
  out_r = m_node->_ring_out;
  
  if (in_r == NULL || out_r == NULL){
    rte_exit(EXIT_FAILURE, "Ring config error\n");
  }

  while(!force_quit){
     //dequeue 
     rcv_nb = rte_ring_dequeue_bulk(in_r, (void**)data, DATA_BATCH, &to_read);
     if(rcv_nb > 0){
        //if(lcore_id == 1){ 
	      //  LOG_INFO("Core#%u dequeue %d msg (%u remain), do computing\n", lcore_id, rcv_nb, to_read);
        //}

        //dequeue and do compute
        for(did = 0; did < rcv_nb; ++did){
          *(data[did]) += 1;
        }
	      LOG_DBG("Core#%u DONE\n", lcore_id);

        //try enqueue
        snd_nb = 0;
        while ((rcv_nb - snd_nb) != 0 ){
          snd_nb += rte_ring_enqueue_bulk(out_r, (void**)(data + snd_nb), rcv_nb - snd_nb, NULL);
        }
	      //LOG_DBG("Core#%u enqueue %d msg\n", lcore_id, snd_nb);
     } 
  }
	//LOG_INFO("Core#%u is quit\n", lcore_id);

	return 0;
}

int
main(int argc, char **argv)
{
	int ret;
	unsigned lcore_id;
  const unsigned flags = 0 | RING_F_SP_ENQ | RING_F_SP_ENQ;
  const unsigned ring_size = RING_SZ;
  char * ring_name = NULL;
  
  for(int did = 0; did < DATA_NUM; ++did){
    raw_data[did] = (int*)malloc(sizeof(int));
    *(raw_data[did]) = 1;
  }
  
  /* release worker lock */
  force_quit = false;

	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_panic("Cannot init EAL\n");

  /* init pool and ring */
  main_in = rte_ring_create("main_in", ring_size, rte_socket_id(), flags);
  if (main_in == NULL){
    rte_exit(EXIT_FAILURE, "Main input ring error\n");
  }

  main_out = rte_ring_create("main_out", ring_size, rte_socket_id(), flags);
  if (main_out == NULL){
    rte_exit(EXIT_FAILURE, "Main output ring error\n");
  }
  
  ring_name = (char*)malloc(sizeof(char) * 16);
  for (int nid = NODE_NUM - 1; nid >= 0; --nid) {
    /* config input ring */
    if ( nid != 0 ) {
      sprintf(ring_name, "%d_in", nid); 
      nodes_list[nid]._ring_in = rte_ring_create(ring_name, ring_size, rte_socket_id(), flags);
      if (nodes_list[nid]._ring_in == NULL){
        rte_exit(EXIT_FAILURE, "Node#%d input ring config error\n", nid);  
      }
      memset(ring_name, 0, sizeof(char) * 16); 
    }
    else{
      nodes_list[nid]._ring_in = main_in;
    }

    /* config output ring */
    if ( nid != NODE_NUM -1 ) {
      sprintf(ring_name, "%d_out", nid); 
      nodes_list[nid]._ring_out = nodes_list[nid + 1]._ring_in;
      if (nodes_list[nid]._ring_out == NULL){
        rte_exit(EXIT_FAILURE, "Node#%d output ring config error\n", nid);  
      }
      memset(ring_name, 0, sizeof(char) * 16); 
    }
    else{
      nodes_list[nid]._ring_out = main_out;
    }
  }

	/* call lcore_calculate() on every slave lcore */
  int max_cores_num = NODE_NUM, cur_cores_num = 1;
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
    if(cur_cores_num > max_cores_num){
      LOG_DBG("All cores have been assgined mission\n");
      break;
    }
		rte_eal_remote_launch(lcore_calculate, NULL, lcore_id);
    cur_cores_num += 1;
	}

  /* launch recv core */
  struct RecvParam recv_conf;
  recv_conf.data_max = DATA_NUM;
  recv_conf.main_out = main_out;
  rte_eal_remote_launch(lcore_recv, (void*)(&recv_conf), NODE_NUM + 1);

  /* enqueue raw data */
  int snd_nb = 0, snd_tot = 0; 
  int cur_batch=0;
  int put_cnt = 0;
  
  //start timer
  TIMER_UP();

  for( cur_batch = 0; cur_batch < DATA_NUM; cur_batch += DATA_BATCH)
  {
    /* do enqueue */
    snd_nb = 0;
    do{
      if (rte_ring_count(main_in) < (rte_ring_get_size(main_in) / 2) ){
        snd_nb += rte_ring_enqueue_bulk(main_in, (void**)(raw_data + cur_batch + snd_nb), DATA_BATCH - snd_nb, NULL);
      }
    } while (snd_nb < DATA_BATCH);
    snd_tot += snd_nb;
    put_cnt += 1;
    //LOG_INFO("Main (%d): %d msg sent\n", put_cnt, snd_nb);
  }
  //LOG_INFO("Main : enqueue %d msg totally\n", snd_tot);
  
  /* wait receiver core */ 
  rte_eal_wait_lcore(NODE_NUM + 1);
  force_quit = true;
  
  /* check results */
  int errnb = 0;
  for (int did = 0; did < DATA_NUM; ++did){
    if( *(raw_data[did]) != (1 + NODE_NUM)){
      errnb += 1;
    }
    else{
      *raw_data[did] = 1; 
    }
  }
  LOG_INFO("Error ratio = %d/%d\n", errnb, DATA_NUM);

	rte_eal_mp_wait_lcore();
  
  /* single core test */
  TIMER_UP(); 
  for(int time = 0; time < NODE_NUM; ++time){
    for(int did = 0; did < DATA_NUM; ++did){
      *raw_data[did] += 1;
    }
  }
  TIMER_DOWN();
  LOG_INFO("SP : Speed = %.3lf Mpps\n", (double)(DATA_NUM) / ((double)dura / 1000000000) / (1000*1000));

  /* free data */
  for (int did = 0; did < DATA_NUM; ++did){
    free(raw_data[did]);
  }

	return 0;
}
