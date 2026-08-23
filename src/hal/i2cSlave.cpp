// VCV implementation of the ER-301's I2C follower HAL.
//
// The engine polls I2c_popMessage() from mods/teletype/Dispatcher::process(),
// an od::Task registered at priority INT_MAX-1, so it drains this queue
// completely at the top of every audio frame. Messages are produced by the VCV
// side (an adjacent II leader module) via VCV_i2cPushMessage().
//
// The queue mirrors the hardware driver in arch/am335x/hal/i2cSlave.c: a
// 64-deep single-producer/single-consumer ring, non-blocking in both
// directions, dropping the NEWEST message when full. Dropping the newest
// rather than overwriting the oldest matters -- the leader has no backpressure
// channel, so discarding already-queued messages would corrupt a committed
// sequence mid-flight.
//
// In VCV both ends currently run on Rack's engine thread (Pump_callback is
// driven from ER301Module::process), so the atomics are not strictly required
// today. They cost nothing measurable and keep this correct if the producer
// ever moves to another thread -- which it would if II delivery were driven
// from the UI thread instead.

#include <hal/i2c.h>
#include <hal/timing.h>
#include <hal/log.h>

#include <atomic>
#include <cstring>

// Must be a power of two: the ring indices are free-running and wrapped by
// masking, so unsigned overflow of the counters stays correct.
static const uint32_t QUEUE_SIZE = 64;
static const uint32_t QUEUE_MASK = QUEUE_SIZE - 1;

static I2cMessage queue[QUEUE_SIZE];
static std::atomic<uint32_t> qFront{0}; // next slot to read  (consumer owns)
static std::atomic<uint32_t> qBack{0};  // next slot to write (producer owns)

static std::atomic<bool> slaveOpen{false};
static std::atomic<uint32_t> ownAddress{0};
static bool initialized = false;

static void queueReset()
{
  qFront.store(0, std::memory_order_relaxed);
  qBack.store(0, std::memory_order_relaxed);
}

// ─── VCV-side producer ───

// Deliver one I2C frame to the engine. Returns false if the follower is
// closed, the address does not match, or the queue is full.
//
// The address byte never reaches the engine -- I2cMessage carries only the
// payload -- so address filtering has to happen here, below I2c_popMessage.
bool VCV_i2cPushMessage(uint8_t address, const uint8_t *data, uint8_t length)
{
  if (!slaveOpen.load(std::memory_order_acquire))
    return false;

  if (address != (uint8_t)ownAddress.load(std::memory_order_relaxed))
    return false;

  if (data == nullptr || length == 0)
    return false;

  // The hardware receive path drops bytes past I2C_MAX_MSG_SIZE but still
  // delivers the message, so truncate rather than discard.
  if (length > I2C_MAX_MSG_SIZE)
    length = I2C_MAX_MSG_SIZE;

  uint32_t b = qBack.load(std::memory_order_relaxed);
  uint32_t f = qFront.load(std::memory_order_acquire);
  if ((uint32_t)(b - f) >= QUEUE_SIZE)
    return false; // full: drop the newest

  I2cMessage *msg = &queue[b & QUEUE_MASK];
  // Stamped on arrival, from the same clock the engine uses. Dispatcher turns
  // (timestamp - mLastTimestamp) into a sample offset within the current frame
  // and renders TR/CV edges there, so stamping a whole batch with one value
  // would collapse every event onto sample 0 and quantise II timing to the
  // frame boundary.
  msg->timestamp = ticks();
  msg->length = length;
  memcpy(msg->data, data, length);

  qBack.store(b + 1, std::memory_order_release);
  return true;
}

bool VCV_i2cIsSlaveOpen(void)
{
  return slaveOpen.load(std::memory_order_acquire);
}

uint8_t VCV_i2cGetOwnAddress(void)
{
  return (uint8_t)ownAddress.load(std::memory_order_relaxed);
}

extern "C"
{

  void I2c_init()
  {
    queueReset();
    initialized = true;
  }

  void I2c_deinit()
  {
    slaveOpen.store(false, std::memory_order_release);
    initialized = false;
  }

  bool I2c_openSlave(uint32_t address)
  {
    if (!initialized)
    {
      logError("I2c_openSlave: i2c not initialized.");
      return false;
    }

    // Hardware calls initQ() here, so opening the follower flushes anything
    // queued against a previously-open address.
    queueReset();
    ownAddress.store(address, std::memory_order_relaxed);
    slaveOpen.store(true, std::memory_order_release);
    logInfo("I2c_openSlave: listening on 0x%02x.", (unsigned)address);
    return true;
  }

  void I2c_closeSlave()
  {
    slaveOpen.store(false, std::memory_order_release);
  }

  bool I2c_popMessage(I2cMessage *msg)
  {
    if (msg == nullptr)
      return false;

    uint32_t f = qFront.load(std::memory_order_relaxed);
    uint32_t b = qBack.load(std::memory_order_acquire);
    if (f == b)
      return false;

    *msg = queue[f & QUEUE_MASK];
    qFront.store(f + 1, std::memory_order_release);
    return true;
  }
}
