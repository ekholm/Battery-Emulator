//----------------------------------------------------------------------------------------

#pragma once

//----------------------------------------------------------------------------------------

#include "ACAN_ESP32_CANMessage.h"
#include <esp_heap_caps.h>
#include <new>

//----------------------------------------------------------------------------------------

class ACAN_ESP32_Buffer16 {

  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
  // Default constructor
  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

  public: ACAN_ESP32_Buffer16 (void)  :
  mBuffer (NULL),
  mSize (0),
  mReadIndex (0),
  mCount (0),
  mPeakCount (0) {
  }

  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
  // Destructor
  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

  public: ~ ACAN_ESP32_Buffer16 (void) {
    release (mBuffer) ;
  }

  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
  // Private properties
  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

  private: CANMessage * mBuffer ;
  private: uint16_t mSize ;
  private: uint16_t mReadIndex ;
  private: uint16_t mCount ;
  private: uint16_t mPeakCount ; // > mSize if overflow did occur

  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
  // Accessors
  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

  public: inline uint16_t size (void) const { return mSize ; }
  public: inline uint16_t count (void) const { return mCount ; }
  public: inline uint16_t peakCount (void) const { return mPeakCount ; }
  public: inline uint16_t didOverflow (void) const { return mPeakCount > mSize ; }

  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
  // initWithSize
  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

  public: bool initWithSize (const uint16_t inSize) {
    release (mBuffer) ;
    mBuffer = allocate (inSize) ;
    const bool ok = (mBuffer != NULL) || (inSize == 0) ;
    mSize = ok ? inSize : 0 ;
    mReadIndex = 0 ;
    mCount = 0 ;
    mPeakCount = 0 ;
    return ok ;
  }

  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
  // append
  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

  // IRAM_ATTR on append and remove: both run inside the TWAI interrupt, which
  // the interrupt is kept alive through flash windows (ESP_INTR_FLAG_IRAM). If
  // the compiler emits an out-of-line copy - which has been observed happening to
  // another in-header ring at -Os - it must land in IRAM, not .flash.text.
  public: bool IRAM_ATTR append (const CANMessage & inMessage) {
    const bool ok = mCount < mSize ;
    if (ok) {
      uint16_t writeIndex = mReadIndex + mCount ;
      if (writeIndex >= mSize) {
        writeIndex -= mSize ;
      }
      mBuffer [writeIndex] = inMessage ;
      mCount += 1 ;
      if (mPeakCount < mCount) {
        mPeakCount = mCount ;
      }
    }else{
      mPeakCount = mSize + 1 ; // Overflow
    }
    return ok ;
  }

  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
  // Remove
  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

  public: bool IRAM_ATTR remove (CANMessage & outMessage) {
    const bool ok = mCount > 0 ;
    if (ok) {
      outMessage = mBuffer [mReadIndex] ;
      mCount -= 1 ;
      mReadIndex += 1 ;
      if (mReadIndex == mSize) {
        mReadIndex = 0 ;
      }
    }
    return ok ;
  }

  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
  // Free
  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

  public: void free (void) {
    release (mBuffer) ; mBuffer = nullptr ;
    mSize = 0 ;
    mReadIndex = 0 ;
    mCount = 0 ;
    mPeakCount = 0 ;
  }

  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
  // Storage: internal DRAM, never PSRAM
  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

  // The TWAI interrupt appends and removes with the flash cache off, and PSRAM is
  // unreachable then too. A plain new[] leaves the placement to the heap's policy,
  // and on an ESP32-S3 with PSRAM that policy sends any block over
  // CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (4,096 B) to PSRAM. A ring deep enough to
  // outlast a flash write is 3 KB, one step from that line, so the placement is
  // asked for rather than left to the size. A failed allocation returns NULL and
  // begin() reports it; new[] would have aborted.
  private: static CANMessage * allocate (const uint16_t inSize) {
    if (inSize == 0) {
      return NULL ;
    }
    void * storage = heap_caps_malloc (sizeof (CANMessage) * inSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) ;
    if (storage == NULL) {
      return NULL ;
    }
    CANMessage * buffer = static_cast <CANMessage *> (storage) ;
    for (uint16_t i = 0 ; i < inSize ; i++) {
      new (&buffer [i]) CANMessage () ;
    }
    return buffer ;
  }

  // CANMessage has no destructor to run, so the storage goes straight back.
  private: static void release (CANMessage * inBuffer) {
    heap_caps_free (inBuffer) ;
  }

  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
  // Reset Peak Count
  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

  public: inline void resetPeakCount (void) { mPeakCount = mCount ; }

  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
  // No copy
  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

  private: ACAN_ESP32_Buffer16 (const ACAN_ESP32_Buffer16 &) = delete ;
  private: ACAN_ESP32_Buffer16 & operator = (const ACAN_ESP32_Buffer16 &) = delete ;

  // - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

} ;

//----------------------------------------------------------------------------------------
