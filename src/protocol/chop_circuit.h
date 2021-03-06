#ifndef CHOP_CIRCUIT_H
#define CHOP_CIRCUIT_H

#include "crypt.h"
#include "rng.h"

#include "chop_config.h"

class chop_circuit_t : public circuit_t
{
public:
  transmit_queue tx_queue;
  reassembly_queue recv_queue;
  unordered_set<chop_conn_t *> downstreams;
  gcm_encryptor *send_crypt;
  ecb_encryptor *send_hdr_crypt;
  gcm_decryptor *recv_crypt;
  ecb_decryptor *recv_hdr_crypt;
  chop_config_t *config;

  uint32_t circuit_id;
  uint32_t last_acked;
  uint32_t dead_cycles;
  bool received_fin : 1;
  bool sent_fin : 1;
  bool upstream_eof : 1;

  // set when block 0 is received
  bool initialized : 1; 


  CIRCUIT_DECLARE_METHODS(chop);
  
  DISALLOW_COPY_AND_ASSIGN(chop_circuit_t);


  // Shortcut some unnecessary conversions for callers within this file.
  void add_downstream(chop_conn_t *conn);
  void drop_downstream(chop_conn_t *conn);


  int find_best_to_retransmit(chop_conn_t*, evbuffer*);

  int send_special(opcode_t f, struct evbuffer *payload);
  int send_targeted(chop_conn_t *conn);
  int send_targeted(chop_conn_t *conn, size_t blocksize);
  int send_targeted(chop_conn_t *conn, size_t d, size_t p, opcode_t f,
                    struct evbuffer *payload);
  int maybe_send_SACK();

  chop_conn_t *pick_connection(size_t desired, size_t minimum,
                               size_t *blocksize);

  int recv_block(uint32_t seqno, uint32_t ackno, opcode_t op, evbuffer *payload);
  int process_queue();
  void check_for_eof();

  uint32_t axe_interval() {
    // This function must always return a number which is larger than
    // the maximum possible number that *our peer's* flush_interval()
    // could have returned; otherwise, we might axe the connection when
    // it was just that there was nothing to say for a while.
    // For simplicity's sake, right now we hardwire this to be 30 minutes.
    return 30 * 60 * 1000;
  }
  uint32_t flush_interval() {
    // 10*60*1000 lies between 2^19 and 2^20.
    uint32_t shift = std::max(1u, std::min(19u, dead_cycles));
    uint32_t xv = std::max(1u, std::min(10u * 60 * 1000, 1u << shift));
    uint32_t rval;
    rval = rng_range_geom(20 * 60 * 1000, xv) + 100;
    if (rval > 3000)
      rval = rval % 3000;
    return rval;
  }
};


#endif
