#include "common/align.h"
#include "common/log.h"
#include "common/decaf_assert.h"
#include "gx2_cbpool.h"
#include "gx2_event.h"
#include "gx2_displaylist.h"
#include "gx2_state.h"
#include "gpu/pm4_buffer.h"
#include "gpu/gpu_commandqueue.h"
#include "modules/coreinit/coreinit_core.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <tuple>
#include <vector>

static std::atomic<pm4::Buffer *>
sBufferItemPool;

static bool
sBufferPoolLeased = false;

static uint32_t *
sBufferPoolBase = nullptr;

static uint32_t *
sBufferPoolEnd = nullptr;

static uint32_t *
sBufferPoolHeadPtr = nullptr;

static uint32_t *
sBufferPoolTailPtr = nullptr;

static uint32_t
sBufferPoolSkipped = 0;

static std::mutex
sBufferPoolMutex;

static pm4::Buffer *
sActiveBuffer[coreinit::CoreCount] = { nullptr, nullptr, nullptr };

namespace gx2
{

namespace internal
{

static pm4::Buffer *
allocateCommandBuffer(uint32_t size);

void
initCommandBufferPool(virtual_ptr<uint32_t> base,
                      uint32_t size)
{
   auto core = coreinit::OSGetCoreId();
   decaf_check(gx2::internal::getMainCoreId() == core);

   sBufferPoolBase = base;
   sBufferPoolEnd = sBufferPoolBase + size;
   sBufferPoolHeadPtr = sBufferPoolBase;
   sBufferPoolTailPtr = nullptr;

   sActiveBuffer[core] = allocateCommandBuffer(0x100);
}

static uint32_t *
allocateFromPool(uint32_t wantedSize,
                 uint32_t &allocatedSize)
{
   std::unique_lock<std::mutex> lock(sBufferPoolMutex);

   // Minimum allocation is 0x100 dwords
   wantedSize = std::max(0x100u, wantedSize);

   // Lets make sure we are not trying to make an impossible allocation
   if (wantedSize > sBufferPoolEnd - sBufferPoolBase) {
      decaf_abort("Command buffer allocation greater than entire pool size");
   }

   uint32_t availableSize = 0;

   if (sBufferPoolTailPtr == nullptr) {
      decaf_check(sBufferPoolHeadPtr == sBufferPoolBase);

      availableSize = static_cast<uint32_t>(sBufferPoolEnd - sBufferPoolBase);
      sBufferPoolTailPtr = sBufferPoolHeadPtr;
   } else {
      if (sBufferPoolHeadPtr < sBufferPoolTailPtr) {
         availableSize = static_cast<uint32_t>(sBufferPoolTailPtr - sBufferPoolHeadPtr);

         if (availableSize < wantedSize) {
            return nullptr;
         }
      } else {
         availableSize = static_cast<uint32_t>(sBufferPoolEnd - sBufferPoolHeadPtr);

         if (availableSize < wantedSize) {
            availableSize = static_cast<uint32_t>(sBufferPoolTailPtr - sBufferPoolBase);

            if (availableSize < wantedSize) {
               return nullptr;
            }

            // Lets mark down that we wasted some space so that returnToPool is able
            //  to verify that pool releases are always happening in-order.  Then we
            //  move the head to the base of the pool to allocate from there.
            sBufferPoolSkipped = static_cast<uint32_t>(sBufferPoolEnd - sBufferPoolHeadPtr);
            sBufferPoolHeadPtr = sBufferPoolBase;
         }
      }
   }

   allocatedSize = std::min(0x20000u, availableSize);

   auto allocatedBuffer = sBufferPoolHeadPtr;
   sBufferPoolHeadPtr += allocatedSize;

   return allocatedBuffer;
}

static void
returnToPool(uint32_t *buffer,
             uint32_t usedSize,
             uint32_t originalSize)
{
   std::unique_lock<std::mutex> lock(sBufferPoolMutex);

   decaf_check(originalSize >= usedSize);

   if (originalSize == usedSize) {
      return;
   }

   decaf_check(sBufferPoolHeadPtr == buffer + originalSize);
   sBufferPoolHeadPtr = buffer + usedSize;
}

static void
freeToPool(uint32_t *buffer,
           uint32_t size)
{
   std::unique_lock<std::mutex> lock(sBufferPoolMutex);

   if (sBufferPoolTailPtr + sBufferPoolSkipped == sBufferPoolEnd) {
      sBufferPoolSkipped = 0;
      sBufferPoolTailPtr = sBufferPoolBase;
   }

   decaf_check(sBufferPoolTailPtr == buffer);

   sBufferPoolTailPtr += size;

   if (sBufferPoolTailPtr == sBufferPoolHeadPtr) {
      sBufferPoolHeadPtr = sBufferPoolBase;
      sBufferPoolTailPtr = nullptr;
   }
}

static pm4::Buffer *
allocateBufferObj()
{
   while (true) {
      auto buffer = sBufferItemPool.load(std::memory_order_acquire);

      if (buffer == nullptr) {
         break;
      }

      auto next = buffer->next.load(std::memory_order_acquire);

      if (sBufferItemPool.compare_exchange_weak(buffer, next, std::memory_order_release, std::memory_order_relaxed)) {
         return buffer;
      }
   }

   return new pm4::Buffer();
}

static void
freeBufferObj(pm4::Buffer *cb)
{
   auto top = sBufferItemPool.load(std::memory_order_acquire);

   while (true) {
      cb->next.store(top, std::memory_order_release);

      if (sBufferItemPool.compare_exchange_weak(top, cb, std::memory_order_release, std::memory_order_relaxed)) {
         return;
      }
   }
}

static pm4::Buffer *
allocateCommandBuffer(uint32_t size)
{
   // This is to ensure that only one command buffer is lease from our
   //  pool at any particular time.
   decaf_check(!sBufferPoolLeased);

   // Only the main core can have command buffers
   if (gx2::internal::getMainCoreId() != coreinit::OSGetCoreId()) {
      gLog->warn("Tried to allocate command buffer on non-main graphics core");
      return nullptr;
   }

   // Lets try to get ourselves a buffer from the pool
   uint32_t *allocatedBuffer = nullptr;
   uint32_t allocatedSize = 0;

   while (!allocatedBuffer) {
      allocatedBuffer = allocateFromPool(size, allocatedSize);

      if (!allocatedBuffer) {
         // If we failed to allocate from the pool, lets wait till
         //  a buffer has been freed, and then try again
         GX2WaitTimeStamp(GX2GetRetiredTimeStamp() + 1);
      }
   }

   // We need to grab a buffer object to hold the info
   auto cb = allocateBufferObj();
   cb->displayList = false;
   cb->submitTime = 0;
   cb->curSize = 0;
   cb->maxSize = allocatedSize;
   cb->buffer = allocatedBuffer;

   sBufferPoolLeased = true;
   return cb;
}

void
freeCommandBuffer(pm4::Buffer *cb)
{
   // Lets just check this to make sure nothing funny happened
   decaf_check(cb->curSize == cb->maxSize);

   // Free the buffer back to the pool if its not a direct-called
   //  display list sent by the application.
   if (!cb->displayList) {
      freeToPool(cb->buffer, cb->maxSize);
   }

   // Save its buffer object for later
   freeBufferObj(cb);
}

static void
flushActiveCommandBuffer()
{
   auto core = coreinit::OSGetCoreId();
   auto cb = sActiveBuffer[core];

   decaf_check(cb);
   decaf_check(!cb->displayList);

   // Lets make sure our lease is still active, and then release it.
   decaf_check(sBufferPoolLeased);
   sBufferPoolLeased = false;

   // Release the remaining space from the buffer back to the
   //  pool so it can be used by the next command buffer!
   returnToPool(cb->buffer, cb->curSize, cb->maxSize);
   cb->maxSize = cb->curSize;

   if (cb->curSize == 0) {
      // There was no space taken, we can directly free this...
      freeBufferObj(cb);
   } else {
      // Send buffer to our driver!
      gpu::queueCommandBuffer(cb);
   }

   // This is no longer the active buffer
   sActiveBuffer[core] = nullptr;
}

pm4::Buffer *
flushCommandBuffer(uint32_t neededSize)
{
   auto core = coreinit::OSGetCoreId();
   auto cb = sActiveBuffer[core];

   decaf_check(cb);

   if (cb->displayList) {
      void *newList = nullptr;
      uint32_t newSize = 0;

      // End the active display list
      padCommandBuffer(cb);

      // Ask user to allocate new display list
      std::tie(newList, newSize) = displayListOverrun(cb->buffer, cb->curSize * 4, neededSize * 4);

      if (!newList || !newSize) {
         decaf_abort("Unable to handle display list overrun");
      }

      // Record the new information returned from the application
      cb->buffer = reinterpret_cast<uint32_t *>(newList);
      cb->curSize = 0;
      cb->maxSize = newSize / 4;

      return cb;
   }

   // Flush the existing buffer
   flushActiveCommandBuffer();

   // Allocate new buffer
   sActiveBuffer[core] = allocateCommandBuffer(neededSize);

   return sActiveBuffer[core];
}

pm4::Buffer *
getCommandBuffer(uint32_t size)
{
   auto core = coreinit::OSGetCoreId();
   auto &cb = sActiveBuffer[core];

   decaf_check(cb);

   if (cb->curSize + size > cb->maxSize) {
      cb = flushCommandBuffer(size);
   }

   return cb;
}

void
padCommandBuffer(pm4::Buffer *buffer)
{
   // Display list is meant to be padded to 32 bytes
   auto alignedSize = align_up(buffer->curSize, 32 / 4);

   decaf_check(alignedSize <= buffer->maxSize);

   while (buffer->curSize < alignedSize) {
      buffer->buffer[buffer->curSize++] = byte_swap(0xBEEF2929);
   }
}

void
queueDisplayList(uint32_t *buffer,
                 uint32_t size)
{
   // Set up our buffer object
   auto cb = allocateBufferObj();
   cb->displayList = true;
   cb->submitTime = 0;
   cb->curSize = size;
   cb->maxSize = size;
   cb->buffer = buffer;

   // Send buffer to our driver!
   gpu::queueCommandBuffer(cb);
}

bool
getUserCommandBuffer(uint32_t **buffer,
                     uint32_t *maxSize)
{
   auto core = coreinit::OSGetCoreId();
   auto &cb = sActiveBuffer[core];
   decaf_check(cb);

   if (!cb->displayList) {
      return false;
   }

   if (buffer) {
      *buffer = cb->buffer;
   }

   if (maxSize) {
      *maxSize = cb->maxSize;
   }

   return true;
}

void
beginUserCommandBuffer(uint32_t *buffer,
                       uint32_t size)
{
   auto core = coreinit::OSGetCoreId();

   if (core == getMainCoreId()) {
      // Flush any commands that were already pending
      flushActiveCommandBuffer();
   }

   // Set up our buffer object
   auto cb = allocateBufferObj();
   cb->displayList = true;
   cb->submitTime = 0;
   cb->curSize = 0;
   cb->maxSize = size;
   cb->buffer = buffer;

   decaf_check(!sActiveBuffer[core]);
   sActiveBuffer[core] = cb;
}

uint32_t
endUserCommandBuffer(uint32_t *buffer)
{
   auto core = coreinit::OSGetCoreId();
   auto &cb = sActiveBuffer[core];

   decaf_check(cb);

   decaf_check(cb->displayList);
   decaf_check(buffer == cb->buffer);

   // Pad and get its size
   padCommandBuffer(cb);
   auto usedSize = cb->curSize;

   // Release the buffer object to the pool
   freeBufferObj(sActiveBuffer[core]);
   sActiveBuffer[core] = nullptr;

   // Allocate a new buffer!
   if (core == getMainCoreId()) {
      sActiveBuffer[core] = allocateCommandBuffer(0x100);
   }

   return usedSize;
}

} // namespace internal

} // namespace gx2
