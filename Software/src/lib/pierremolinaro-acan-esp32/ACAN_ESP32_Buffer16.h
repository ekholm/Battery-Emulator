//----------------------------------------------------------------------------------------

#pragma once

//----------------------------------------------------------------------------------------

#include "ACAN_ESP32_CANMessage.h"

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
    delete [] mBuffer ;
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
    delete [] mBuffer ;
    mBuffer = new CANMessage [inSize] ;
    const bool ok = mBuffer != NULL ;
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
    delete [] mBuffer ; mBuffer = nullptr ;
    mSize = 0 ;
    mReadIndex = 0 ;
    mCount = 0 ;
    mPeakCount = 0 ;
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
