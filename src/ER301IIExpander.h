#pragma once
//
// Wire contract for delivering monome II / i2c messages to the ER-301 module
// through Rack's expander message buffers.
//
// ═══ THIS FILE IS DUPLICATED ═══
//   er-301-vcv-stolmine/src/ER301IIExpander.h
//   monome-rack-stolmine/src/common/core/ER301IIExpander.h
// They must stay byte-identical. The two plugins are separate dylibs with no
// shared build, so there is no way to enforce that at compile time -- hence the
// magic/version/size header below, which turns a mismatch into a silent no-op
// instead of a crash. Bump ER301_II_VERSION on ANY layout change.
//
// Rack's expander API is the only sanctioned cross-plugin channel: the sender
// checks the receiver's Model before touching its buffers, so nothing here
// requires linking the two plugins together. The ER-301 (receiver) allocates
// both buffers on both sides, so it can sit to either side of the leader.
//
// Plain C layout only -- no std:: types, no virtuals, no bitfields. Every field
// is fixed-width and naturally aligned, so the struct has identical layout
// under both plugins' compilers without packing pragmas.

#include <stdint.h>

// The receiving plugin's slugs, for rack::plugin::getModel().
#define ER301_II_PLUGIN_SLUG "ER-301"
#define ER301_II_MODEL_SLUG "ER301"

// 'I301' little-endian. Guards against a neighbour that allocated an expander
// buffer for some entirely different protocol.
#define ER301_II_MAGIC 0x31303349u
#define ER301_II_VERSION 1u

// Matches I2C_MAX_MSG_SIZE in the ER-301 firmware's hal/i2c.h.
#define ER301_II_MAX_DATA 8
// One Teletype script line can emit several II writes; 32 is the same bound
// the firmware's own outbound queue uses.
#define ER301_II_MAX_FRAMES 32

typedef struct
{
  uint8_t address; // 7-bit follower address, e.g. 0x31 for SC
  uint8_t length;  // 1..ER301_II_MAX_DATA
  uint8_t data[ER301_II_MAX_DATA];
} ER301IIFrame; // 10 bytes, no padding

typedef struct
{
  uint32_t magic;   // ER301_II_MAGIC
  uint32_t version; // ER301_II_VERSION
  uint32_t size;    // sizeof(ER301IIExpanderMessage)
  uint32_t count;   // frames in use, 0..ER301_II_MAX_FRAMES

  ER301IIFrame frames[ER301_II_MAX_FRAMES];
} ER301IIExpanderMessage;

// Protocol:
//
//  - The ER-301 allocates one of these as producerMessage AND consumerMessage
//    on both leftExpander and rightExpander, zero-initialised with the header
//    filled in.
//  - A leader checks its neighbour's Model, casts that neighbour's
//    producerMessage on the side facing itself, validates magic/version/size,
//    writes frames + count, then calls requestMessageFlip() on that expander.
//  - The ER-301 reads consumerMessage in process(), pushes each frame into the
//    i2c follower queue, then sets count = 0 so the same block is not consumed
//    twice if the leader stops writing.
//
// Message passing has one sample of latency, which is immaterial against the
// ER-301's 128-sample engine frame.
