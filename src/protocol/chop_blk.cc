/* Copyright 2011, 2012, 2013 SRI International
 * See LICENSE for other credits and copying information
 */


#include "chop_blk.h"
#include "crypt.h"
#include "connections.h"
#include <event2/buffer.h>
#include <iomanip>
#include <limits>


/* The chopper is the core StegoTorus protocol implementation.
   For its design, see doc/chopper.txt.  Note that it is still
   being implemented, and may change incompatibly.  */

using std::numeric_limits;

namespace chop_blk
{

const char *
opname(unsigned int o, char fallbackbuf[4])
{
  switch (o) {
  case op_XXX: return "XXX";
  case op_DAT: return "DAT";
  case op_FIN: return "FIN";
  case op_RST: return "RST";
  case op_SACK: return "SACK";
  default:
    if (o > op_LAST)
      return "^^^";

    if (o < op_STEG0)
      xsnprintf(fallbackbuf, sizeof *fallbackbuf, "R%02x", o);
    else
      xsnprintf(fallbackbuf, sizeof *fallbackbuf, "S%02x", o - op_STEG0);
    return fallbackbuf;
  }
}



void
debug_ack_contents(evbuffer *payload, std::ostream& os)
{

  std::ios init(NULL);
  init.copyfmt(os);


  size_t len = evbuffer_get_length(payload);
  os << "length " << len << "; ";

  if (len < 4) {
    os << "too short";
    return;
  }

  uint8_t *buf = evbuffer_pullup(payload, len);
  uint32_t hsn = ((uint32_t(buf[0]) << 24) | (uint32_t(buf[1]) << 16) |
                  (uint32_t(buf[2]) <<  8) | (uint32_t(buf[3])));

  log_assert(buf);
  os << "through " << hsn;

  if (len == 4)
    return;

  size_t i;
  for (i = 0; i < 32; i++) {
    if (i + 4 >= len)
      break;

    uint8_t c = buf[i+4];
    for (int j = 0; j < 8; j++)
      if (c & (1 << j))
        os << ", " << (hsn + 1 + i*8 + j);
  }

  if (i + 4 == len)
    return;

  os << "; trailing junk: " << std::hex << std::setw(2) << std::setfill('0');

  for (i += 4; i < len; i++)
    os << (unsigned int)buf[i];

  os.copyfmt(init);

}




// Note: this function must take exactly the same amount of time to
// execute regardless of its inputs.
header::header(const uint8_t *ciphr, ecb_decryptor &dc, uint32_t window) 
  : s(0), a(0), d(0), p(0), f(op_XXX)
{
  uint8_t clear[16];

  dc.decrypt(clear, ciphr);

  uint32_t s_ = ((uint32_t(clear[0]) << 16) | (uint32_t(clear[1]) <<  8) |
                 (uint32_t(clear[2])));

  uint32_t a_  = ((uint32_t(clear[3]) << 16) | (uint32_t(clear[4]) <<  8) |
                 (uint32_t(clear[5]) ));

  uint16_t d_ = ((uint16_t(clear[6]) <<  8) | (uint16_t(clear[7])));

  uint16_t p_ = ((uint16_t(clear[8]) <<  8) | (uint16_t(clear[9])));

  uint8_t f_  = clear[10];

  bool checkOK = !(clear[11] | clear[12] | clear[13] | clear[14] | clear[15]);

  uint32_t delta = s_ - window;
  bool deltaOK = !(delta & ~uint32_t(0xFF));
  bool fOK = ((f_ >= op_RESERVED0) && (f_ < op_STEG0));
  bool ok = (checkOK | deltaOK | fOK);

  if (ok) {
    s = s_;
    a = a_;
    d = d_;
    p = p_;
    f = opcode_t(f_);
  } else {
    s = 0;
    a = 0;
    d = 0;
    p = 0;
    f = op_XXX;
  }
}



void
header::encode(uint8_t *ciphr, ecb_encryptor &ec) const
{
  uint8_t clear[16];

  clear[ 0] = (s >> 16) & 0xFF;
  clear[ 1] = (s >>  8) & 0xFF;
  clear[ 2] = (s      ) & 0xFF;

  clear[ 3] = (a >> 16) & 0xFF;
  clear[ 4] = (a >>  8) & 0xFF;
  clear[ 5] = (a      ) & 0xFF;

  clear[ 6] = (d >>  8) & 0xFF;
  clear[ 7] = (d      ) & 0xFF;

  clear[ 8] = (p >>  8) & 0xFF;
  clear[ 9] = (p      ) & 0xFF;

  clear[10] = uint8_t(f);

  clear[11] = 0;
  clear[12] = 0;
  clear[13] = 0;
  clear[14] = 0;
  clear[15] = 0;

  ec.encrypt(ciphr, clear);
}



bool
header::prepare_retransmit(uint32_t ackno, uint16_t new_plen)
{
  //it seems prudent to update the ackno ...
  a = ackno;
  p = new_plen;
  return true;
}



ack_payload::ack_payload(evbuffer *wire, uint32_t hfloor)
  : hsn_(-1), maxusedbyte(0)
{
  memset(window, 0, sizeof window);

  uint8_t hsnwire[4];
  if (evbuffer_remove(wire, hsnwire, 4) != 4) {
    // invalid payload
    evbuffer_free(wire);
    log_warn("invalid payload");
    return;
  }
  hsn_ = (uint32_t(hsnwire[0]) << 24 |
          uint32_t(hsnwire[1]) << 16 |
          uint32_t(hsnwire[2]) <<  8 |
          uint32_t(hsnwire[3]));


  maxusedbyte = evbuffer_remove(wire, window, sizeof window);

  // there shouldn't be any _more_ data than that, the hsn should
  // be in the range [hfloor-1, hfloor+256), and the first bit of the
  // window should be zero.

  if (evbuffer_get_length(wire) > 0 || (hfloor >= 1 && hsn_ < hfloor - 1 && (hfloor - hsn_ > 128)) || (hsn_ >= hfloor + 256)) {
    log_warn("invalid ack_payload: %u %u %u", hsn_, hfloor, block_received(hsn_ + 1));
    hsn_ = -1; // invalidate
  }
  else if ((hfloor >= 1 && hsn_ < hfloor-1) ||  block_received(hsn_ + 1)) {
    log_debug("invalid ack_payload (ack block reordering?): %u %u %u", hsn_, hfloor, block_received(hsn_ + 1));
    hsn_ = -1; // invalidate
  }

  evbuffer_free(wire);
}



evbuffer *
ack_payload::serialize() const
{
  log_assert(valid());
  evbuffer *wire = evbuffer_new();
  evbuffer_iovec v;

  if (evbuffer_reserve_space(wire, 4 + maxusedbyte, &v, 1) != 1 ||
      v.iov_len < 4 + maxusedbyte) {
    evbuffer_free(wire);
    return 0;
  }

  uint8_t *p = (uint8_t *)v.iov_base;
  p[0] = (hsn_ & 0xFF000000U) >> 24;
  p[1] = (hsn_ & 0x00FF0000U) >> 16;
  p[2] = (hsn_ & 0x0000FF00U) >>  8;
  p[3] = (hsn_ & 0x000000FFU);

  if (maxusedbyte > 0)
    memcpy(&p[4], window, maxusedbyte);

  v.iov_len = 4 + maxusedbyte;
  if (evbuffer_commit_space(wire, &v, 1)) {
    evbuffer_free(wire);
    return 0;
  }
  return wire;
}



transmit_queue::transmit_queue()  : next_to_ack(0), next_to_send(0)
{
}



transmit_queue::~transmit_queue()
{
  for (int i = 0; i < 256; i++)
    if (cbuf[i].data)
      evbuffer_free(cbuf[i].data);
}



uint32_t
transmit_queue::enqueue(opcode_t f, evbuffer *data, uint16_t padding)
{

  //careful on windows things like ostream and sstream can pull in a wrong definition of time_t
  //fprintf(stderr, "chop_blk: sizeof(time_t) = %d\n", sizeof(time_t));
    
  log_assert(opcode_valid(f));
  log_assert(evbuffer_get_length(data) <= numeric_limits<uint16_t>::max());
  log_assert(!full());

  uint32_t seqno = next_to_send;

  transmit_elt &elt = cbuf[seqno & 0xFF];

  // set ackno to 0 for now... fill it in when transmitting
  elt.hdr = header(seqno, 0, evbuffer_get_length(data), padding, f);
  //fprintf(stderr, "setting %p %p %p %p\n", this, cbuf, &elt, data);
  elt.data = data;
  elt.last_sent = 0;
  next_to_send++;
  return seqno;
}



int
transmit_queue::transmit(uint32_t seqno, uint32_t next_to_recv,
                         evbuffer *output, ecb_encryptor &ec, gcm_encryptor &gc)
{

  // next_to_ack == highest received ackno + 1
  log_assert(seqno >= next_to_ack && seqno < next_to_send);
  transmit_elt &elt = cbuf[seqno & 0xFF];

  // next_to_recv == highest inorder seqno received + 1
  return transmit(elt, next_to_recv, output, ec, gc);
}




int
transmit_queue::transmit(transmit_elt &elt, uint32_t next_to_recv,
                         evbuffer *output, ecb_encryptor &ec, gcm_encryptor &gc)
{

  if (!elt.data){
    //a jit preprocessor hack while the cross compiling dust has not settled.
    #ifdef _WIN32
    log_warn("ERRR: transmit_queue:transmit %u %lu", elt.hdr.seqno(), (long unsigned int)elt.data);
    #else
    log_warn("ERRR: transmit_queue:transmit %u %llu", elt.hdr.seqno(), (long long unsigned int)elt.data);
    #endif
  }

  log_assert(elt.data);

  struct evbuffer *block = evbuffer_new();
  if (!block) {
    log_warn("memory allocation failure");
    return -1;
  }

  size_t d = elt.hdr.dlen();
  size_t p = elt.hdr.plen();
  size_t blocksize = elt.hdr.total_len();
  struct evbuffer_iovec v;

  // next_to_recv == highest inorder seqno received + 1
  elt.hdr.set_ackno(next_to_recv-1);

  if (evbuffer_reserve_space(block, blocksize, &v, 1) != 1 ||
      v.iov_len < blocksize) {
    log_warn("memory allocation failure");
    evbuffer_free(block);
    return -1;
  }

  v.iov_len = blocksize;
  elt.hdr.encode((uint8_t *)v.iov_base, ec);

  uint8_t encodebuf[d + p];
  
  //fprintf(stderr, "sending %p %p %p %p\n", this, cbuf, &elt, elt.data);
  
  if (evbuffer_copyout(elt.data, encodebuf, d) != (ssize_t)d) {
    log_warn("failed to extract data");
    evbuffer_free(block);
    return -1;
  }

  memset(encodebuf + d, 0, p);
  gc.encrypt((uint8_t *)v.iov_base + HEADER_LEN,
             encodebuf, d + p, (uint8_t *)v.iov_base, HEADER_LEN);

  if (evbuffer_commit_space(block, &v, 1)) {
    log_warn("failed to commit block buffer");
    evbuffer_free(block);
    return -1;
  }

  if (evbuffer_add_buffer(output, block)) {
    log_warn("failed to transfer block to output queue");
    evbuffer_free(block);
    return -1;
  }

  elt.last_sent = time(NULL);
  evbuffer_free(block);
  return 0;
}



 int
 transmit_queue::retransmit(uint32_t seqno, uint32_t next_to_recv, uint16_t new_padding,
                            evbuffer *output, ecb_encryptor &ec, gcm_encryptor &gc)
 {
   log_assert(seqno >= next_to_ack && seqno < next_to_send);
   transmit_elt &elt = cbuf[seqno & 0xFF];
   return retransmit(elt, next_to_recv, new_padding, output, ec, gc);
 }





int
transmit_queue::retransmit(transmit_elt &elt,  uint32_t next_to_recv,
                           uint16_t new_padding, evbuffer *output, 
			   ecb_encryptor &ec, gcm_encryptor &gc)
{

  int rval;
  time_t tval;

  if (!elt.data)
    return -1;

  tval = time(NULL);

  // XXXX fix clock skew issue
  if (tval - elt.last_sent < 6) { 

    if (full()) 
      goto skip;

    log_debug("Too soon: not retransmitting: block %u", elt.hdr.seqno());
    return -1;
  }    


  if (!elt.hdr.prepare_retransmit(next_to_ack, new_padding)) {
    //currently this is dead code
    log_warn("block %u retransmitted too many times", elt.hdr.seqno());
    return -1;
  }


 skip:
  rval = transmit(elt, next_to_recv, output, ec, gc);
  return rval;
}




int transmit_queue::process_ack(uint32_t ackno)
{
  // trundle through the transmit queue looking for the
  // transmit_elt with this seqno and toss it.
  // might look to see if we can move the window forward too?
    
  if (ackno >= next_to_send)  {
    log_warn("invalid ack: ackno = %d >= next_to_send = %d... ignoring\n", (int)ackno, (int)next_to_send);
    return -1;
  }

  // next_to_ack == highest received ackno + 1
  for (; next_to_ack <= ackno; next_to_ack++) {
    uint8_t j = next_to_ack & 0xFF;
    if (cbuf[j].data) {
      evbuffer_free(cbuf[j].data);
      cbuf[j].data = 0;
    }
  }
  
  return 0;
}




int
transmit_queue::process_ack(evbuffer *data)
{

  ack_payload ack(data, next_to_ack);

  if (!ack.valid()) {
    // this is okay... no need for warnings
    return 0;  
  }

  uint32_t hsn = ack.hsn();

  if (hsn >= next_to_send)  {
    log_warn("invalid ack: hsn = %d >=  next_to_send = %d... ignoring\n", (int)hsn, (int)next_to_send);
    return -1;
  }

  // next_to_ack == highest received ackno + 1
  for (; next_to_ack <= hsn; next_to_ack++) {
    uint8_t j = next_to_ack & 0xFF;
    if (cbuf[j].data) {
      evbuffer_free(cbuf[j].data);
      cbuf[j].data = 0;
    }
  }

  if (next_to_ack == next_to_send)
    return 0;

  for (uint32_t i = next_to_ack; i < next_to_send; i++) {
    uint8_t j = i & 0xFF;
    if (cbuf[j].data && ack.block_received(i)) {
      evbuffer_free(cbuf[j].data);
      cbuf[j].data = 0;
    }
  } 

  return 0;
}



reassembly_queue::reassembly_queue()
  : next_to_process(0), count(0)
{
  memset(cbuf, 0, sizeof cbuf);
}



reassembly_queue::~reassembly_queue()
{
  if (count == 0) return; // short cut for ideal case
  for (int i = 0; i < 256; i++)
    if (cbuf[i].data)
      evbuffer_free(cbuf[i].data);
}



reassembly_elt
reassembly_queue::remove_next()
{
  reassembly_elt rv = { 0, op_DAT };
  uint8_t front = next_to_process & 0xFF;
  char fallbackbuf[4];

  log_debug("next_to_process=%d data=%p op=%s",
            next_to_process, cbuf[front].data,
            opname(cbuf[front].op, fallbackbuf));

  if (cbuf[front].data) {
    rv = cbuf[front];
    cbuf[front].data = 0;
    cbuf[front].op   = op_DAT;
    next_to_process++;
    count--;
  }
  return rv;
}




bool
reassembly_queue::insert(uint32_t seqno, opcode_t op, evbuffer *data)
{
  // weird but window and seqno are unsigned...
  if (seqno - window() > 255 && window() - seqno > 10) {
    log_warn("block outside receive window %d %d", seqno, window());
    evbuffer_free(data);
    return false;
  }

  uint8_t front = next_to_process & 0xFF;
  uint8_t pos = front + (seqno - window());

  if (cbuf[pos].data) {
    log_info("duplicate block");
    evbuffer_free(data);
    return false;
  }

  cbuf[pos].data = data;
  cbuf[pos].op   = op;
  count++;
  return true;
}




void
reassembly_queue::reset()
{
  log_assert(count == 0);
  for (int i = 0; i < 256; i++) {
    log_assert(!cbuf[i].data);
  }
  next_to_process = 0;
}



evbuffer *
reassembly_queue::gen_ack() const
{
  ack_payload payload(next_to_process == 0 ? 0 : next_to_process - 1);
  uint8_t front = next_to_process & 0xFF;
  uint8_t pos = front;
  do {
    if (cbuf[pos].data)
      payload.set_block_received(next_to_process + (pos - front));
    pos++;
  } while (pos != front);

  return payload.serialize();
}

} // namespace chop_blk

