/*
 *  © 2020, Chris Harlow. All rights reserved.
 *  © 2020, Harald Barth.
 *  
 *  This file is part of Asbelos DCC API
 *
 *  This is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  It is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with CommandStation.  If not, see <https://www.gnu.org/licenses/>.
 */
 #pragma GCC optimize ("-O3")
#include <Arduino.h>

#include "DCCWaveform.h"
#include "DCCTimer.h"
#include "DIAG.h"
#include "freeMemory.h"

DCCWaveform  DCCWaveform::mainTrack(PREAMBLE_BITS_MAIN, true);
DCCWaveform *  DCCWaveform::progTrack=0; // maybe created later if a prog track is used

bool DCCWaveform::progTrackSyncMain=false; 
bool DCCWaveform::progTrackBoosted=false; 

  
void DCCWaveform::begin(MotorDriver * mainDriver, MotorDriver * progDriver,
                    MotorDriver * booster1, MotorDriver * booster2, MotorDriver * booster3, MotorDriver * booster4)
                    {
  mainTrack.motorDriver=mainDriver;
  mainTrack.setPowerMode(POWERMODE::OFF);      
  MotorDriver::usePWM= mainDriver->isPWMCapable();

  // Chain boosters together with main Motor Driver (reverse order add gets chain in real order)
  if (booster4) mainDriver->addBooster(4,booster4);
  if (booster3) mainDriver->addBooster(3,booster3);
  if (booster2) mainDriver->addBooster(2,booster2);
  if (booster1) mainDriver->addBooster(1,booster1);
  
  if (progDriver) {  // Optional prog track 
    progTrack=new DCCWaveform(PREAMBLE_BITS_PROG, false);
    progDriver->boosterId=255; // Not really a booster
    progTrack->motorDriver=progDriver;
    progTrack->setPowerMode(POWERMODE::OFF);
    // Fault pin config for odd motor boards (example pololu)
    MotorDriver::commonFaultPin = ((mainDriver->getFaultPin() == progDriver->getFaultPin())
        && (mainDriver->getFaultPin() != UNUSED_PIN));
    // Only use PWM if both pins are PWM capable. Otherwise JOIN does not work
    MotorDriver::usePWM=  MotorDriver::usePWM && progDriver->isPWMCapable();
  }
  
  if (MotorDriver::usePWM)
    DIAG(F("Signal pin config: high accuracy waveform"));
  else
    DIAG(F("Signal pin config: normal accuracy waveform"));
  DCCTimer::begin(progTrack ? DCCWaveform::interruptHandler : DCCWaveform::interruptHandlerNoProgtrack);     
}

void DCCWaveform::loop(bool ackManagerActive) {
  for (MotorDriver * driver=mainTrack.motorDriver;driver;driver=driver->nextDriver) driver->checkPowerOverload(false);
  if (progTrack) progTrack->motorDriver->checkPowerOverload( !ackManagerActive && !progTrackSyncMain && !progTrackBoosted);
  uint16_t myMillis=millis();
  if (gaugeSampleTime && (myMillis-lastGaugeTime > gaugeSampleTime)) {
    lastGaugeTime=myMillis;
    listRawGauges(&Serial);
  }
}
uint16_t DCCWaveform::lastGaugeTime=0;
uint16_t DCCWaveform::gaugeSampleTime=0; // millis between <g > responses, 0= no gauges requested 

void DCCWaveform::interruptHandler() {
  // call the timer edge sensitive actions for progtrack and maintrack
  // member functions would be cleaner but have more overhead
  byte sigMain=signalTransform[mainTrack.state];
  byte sigProg=progTrackSyncMain? sigMain : signalTransform[progTrack->state];
  
  // Set the signal state for both tracks
  mainTrack.motorDriver->setSignal(sigMain);
  progTrack->motorDriver->setSignal(sigProg);
  
  // Move on in the state engine
  mainTrack.state=stateTransform[mainTrack.state];    
  progTrack->state=stateTransform[progTrack->state];    


  // WAVE_PENDING means we dont yet know what the next bit is
  if (mainTrack.state==WAVE_PENDING) mainTrack.interrupt2();  
  if (progTrack->state==WAVE_PENDING) progTrack->interrupt2();
  else if (progTrack->ackPending) progTrack->checkAck();

}

void DCCWaveform::interruptHandlerNoProgtrack() {
  byte sigMain=signalTransform[mainTrack.state];
  
  // Set the signal state 
  mainTrack.motorDriver->setSignal(sigMain);
  
  // Move on in the state engine
  mainTrack.state=stateTransform[mainTrack.state];    

  // WAVE_PENDING means we dont yet know what the next bit is
  if (mainTrack.state==WAVE_PENDING) mainTrack.interrupt2();  
}


// An instance of this class handles the DCC transmissions for one track. (main or prog)
// Interrupts are marshalled via the statics.
// A track has a current transmit buffer, and a pending buffer.
// When the current buffer is exhausted, either the pending buffer (if there is one waiting) or an idle buffer.


// This bitmask has 9 entries as each byte is trasmitted as a zero + 8 bits.
const byte bitMask[] = {0x00, 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01};


DCCWaveform::DCCWaveform( byte preambleBits, bool isMain) {
  isMainTrack = isMain;
  packetPending = false;
  memcpy(transmitPacket, idlePacket, sizeof(idlePacket));
  state = WAVE_START;
  // The +1 below is to allow the preamble generator to create the stop bit
  // for the previous packet. 
  requiredPreambles = preambleBits+1;  
  bytes_sent = 0;
  bits_sent = 0;
  ackPending=false;
}


void DCCWaveform::setPowerMode(POWERMODE mode) {
  // sets power on for all boosters on this track
  for (MotorDriver * driver=motorDriver;driver;driver=driver->nextDriver) driver->setPowerMode(mode);
}
 void DCCWaveform::setBoosterPowerMode(byte boosterId,POWERMODE mode) {
  for (MotorDriver * driver=mainTrack.motorDriver;driver;driver=driver->nextDriver) {
    if (driver->boosterId==boosterId) {
      driver->setPowerMode(mode);
      break;
    }
  }
 }
   
POWERMODE DCCWaveform::getPowerMode() {
  return motorDriver->getPowerMode();
}
void DCCWaveform::describeGauges(Print * stream, int sampleTimeSeconds) {
  gaugeSampleTime=1000*sampleTimeSeconds;
  if (progTrack) progTrack->motorDriver->describeGauge(stream);
  for (MotorDriver * driver=mainTrack.motorDriver;driver;driver=driver->nextDriver) driver->describeGauge(stream);
}

void DCCWaveform::listRawGauges(Print * stream) {
  stream->print("<g ");
  if (progTrack) progTrack->motorDriver->printRawCurrent(stream);
  for (MotorDriver * driver=mainTrack.motorDriver;driver;driver=driver->nextDriver) driver->printRawCurrent(stream); 
  stream->print('>');     
}




// For each state of the wave  nextState=stateTransform[currentState] 
const WAVE_STATE DCCWaveform::stateTransform[]={
   /* WAVE_START   -> */ WAVE_PENDING,
   /* WAVE_MID_1   -> */ WAVE_START,
   /* WAVE_HIGH_0  -> */ WAVE_MID_0,
   /* WAVE_MID_0   -> */ WAVE_LOW_0,
   /* WAVE_LOW_0   -> */ WAVE_START,
   /* WAVE_PENDING (should not happen) -> */ WAVE_PENDING};

// For each state of the wave, signal pin is HIGH or LOW   
const bool DCCWaveform::signalTransform[]={
   /* WAVE_START   -> */ HIGH,
   /* WAVE_MID_1   -> */ LOW,
   /* WAVE_HIGH_0  -> */ HIGH,
   /* WAVE_MID_0   -> */ LOW,
   /* WAVE_LOW_0   -> */ LOW,
   /* WAVE_PENDING (should not happen) -> */ LOW};
        
void DCCWaveform::interrupt2() {
  // calculate the next bit to be sent:
  // set state WAVE_MID_1  for a 1=bit
  //        or WAVE_HIGH_0 for a 0 bit.

  if (remainingPreambles > 0 ) {
    state=WAVE_MID_1;  // switch state to trigger LOW on next interrupt
    remainingPreambles--;
    // Update free memory diagnostic as we don't have anything else to do this time.
    // Allow for checkAck and its called functions using 22 bytes more.
    updateMinimumFreeMemory(22); 
    return;
  }

  // Wave has gone HIGH but what happens next depends on the bit to be transmitted
  // beware OF 9-BIT MASK  generating a zero to start each byte
  state=(transmitPacket[bytes_sent] & bitMask[bits_sent])? WAVE_MID_1 : WAVE_HIGH_0; 
  bits_sent++;

  // If this is the last bit of a byte, prepare for the next byte

  if (bits_sent == 9) { // zero followed by 8 bits of a byte
    //end of Byte
    bits_sent = 0;
    bytes_sent++;
    // if this is the last byte, prepere for next packet
    if (bytes_sent >= transmitLength) {
      // end of transmission buffer... repeat or switch to next message
      bytes_sent = 0;
      remainingPreambles = requiredPreambles;

      if (transmitRepeats > 0) {
        transmitRepeats--;
      }
      else if (packetPending) {
        // Copy pending packet to transmit packet
        // a fixed length memcpy is faster than a variable length loop for these small lengths
        // for (int b = 0; b < pendingLength; b++) transmitPacket[b] = pendingPacket[b];
        memcpy( transmitPacket, pendingPacket, sizeof(pendingPacket));
        
        transmitLength = pendingLength;
        transmitRepeats = pendingRepeats;
        packetPending = false;
        sentResetsSincePacket=0;
      }
      else {
        // Fortunately reset and idle packets are the same length
        memcpy( transmitPacket, isMainTrack ? idlePacket : resetPacket, sizeof(idlePacket));
        transmitLength = sizeof(idlePacket);
        transmitRepeats = 0;
        if (sentResetsSincePacket<250) sentResetsSincePacket++;
      }
    }
  }  
}



// Wait until there is no packet pending, then make this pending
void DCCWaveform::schedulePacket(const byte buffer[], byte byteCount, byte repeats) {
  if (byteCount > MAX_PACKET_SIZE) return; // allow for chksum
  while (packetPending);

  byte checksum = 0;
  for (byte b = 0; b < byteCount; b++) {
    checksum ^= buffer[b];
    pendingPacket[b] = buffer[b];
  }
  // buffer is MAX_PACKET_SIZE but pendingPacket is one bigger
  pendingPacket[byteCount] = checksum;
  pendingLength = byteCount + 1;
  pendingRepeats = repeats;
  packetPending = true;
  sentResetsSincePacket=0;
}

// Operations applicable to PROG track ONLY.
// (yes I know I could have subclassed the main track but...) 

void DCCWaveform::setAckBaseline() {
      if (isMainTrack) return;
      int baseline=motorDriver->getCurrentRaw();
      ackThreshold= baseline + motorDriver->mA2raw(ackLimitmA);
      if (Diag::ACK) DIAG(F("ACK baseline=%d/%dmA Threshold=%d/%dmA Duration between %dus and %dus"),
			  baseline,motorDriver->raw2mA(baseline),
			  ackThreshold,motorDriver->raw2mA(ackThreshold),
                          minAckPulseDuration, maxAckPulseDuration);
}

void DCCWaveform::setAckPending() {
      if (isMainTrack) return; 
      ackMaxCurrent=0;
      ackPulseStart=0;
      ackPulseDuration=0;
      ackDetected=false;
      ackCheckStart=millis();
      ackPending=true;  // interrupt routines will now take note
}

byte DCCWaveform::getAck() {
      if (ackPending) return (2);  // still waiting
      if (Diag::ACK) DIAG(F("%S after %dmS max=%d/%dmA pulse=%duS"),ackDetected?F("ACK"):F("NO-ACK"), ackCheckDuration, 
           ackMaxCurrent,motorDriver->raw2mA(ackMaxCurrent), ackPulseDuration);
      if (ackDetected) return (1); // Yes we had an ack
      return(0);  // pending set off but not detected means no ACK.   
}

void DCCWaveform::checkAck() {
    // This function operates in interrupt() time so must be fast and can't DIAG 
    if (sentResetsSincePacket > 6) {  //ACK timeout
        ackCheckDuration=millis()-ackCheckStart;
        ackPending = false;
        return; 
    }
      
    int current=motorDriver->getCurrentRaw();
    if (current > ackMaxCurrent) ackMaxCurrent=current;
    // An ACK is a pulse lasting between minAckPulseDuration and maxAckPulseDuration uSecs (refer @haba)
        
    if (current>ackThreshold) {
       if (ackPulseStart==0) ackPulseStart=micros();    // leading edge of pulse detected
       return;
    }
    
    // not in pulse
    if (ackPulseStart==0) return; // keep waiting for leading edge 
    
    // detected trailing edge of pulse
    ackPulseDuration=micros()-ackPulseStart;
               
    if (ackPulseDuration>=minAckPulseDuration && ackPulseDuration<=maxAckPulseDuration) {
        ackCheckDuration=millis()-ackCheckStart;
        ackDetected=true;
        ackPending=false;
        transmitRepeats=0;  // shortcut remaining repeat packets 
        return;  // we have a genuine ACK result
    }      
    ackPulseStart=0;  // We have detected a too-short or too-long pulse so ignore and wait for next leading edge 
}
