// **********************************************************
// ************************ openLRSngDL *********************
// **********************************************************
// ** by Kari Hautio - kha @ AeroQuad/RCGroups/IRC(Freenode)
// ** other commits by cTn-dev, rlboyd, DTFUHF, pwarren
//
// Developer chat at IRC: #openLRS @ freenode
//
// This code is based on OpenLRSng
//
// Donations for development tools and utilities (beer) here
// https://www.paypal.com/cgi-bin/webscr?cmd=_s-xclick&hosted_button_id=DSWGKGKPRX5CS
//
// **********************************************************
// ************ based on: OpenLRS thUndeadMod ***************
// Mihai Andrian - thUndead http://www.fpvuk.org/forum/index.php?topic=3642.0
//
// **********************************************************
// *************** based on: OpenLRS Code *******************
// ***  OpenLRS Designed by Melih Karakelle on 2010-2011  ***
// **  an Arudino based RC Rx/Tx system with extra futures **
// **       This Source code licensed under GPL            **
// **********************************************************

// **********************************************************
// **************** original OpenLRS DEVELOPERS *************
// Mihai Andrian - thUndead http://www.fpvuk.org/forum/index.php?topic=3642.0
// Melih Karakelle (http://www.flytron.com) (forum nick name: Flytron)
// Jan-Dirk Schuitemaker (http://www.schuitemaker.org/) (forum nick name: CrashingDutchman)
// Etienne Saint-Paul (http://www.gameseed.fr) (forum nick name: Etienne)

#include<ctype.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<avr/interrupt.h>
#include "system.h"
#include "io.h"
#include "serial.h"
#include "print.h"
#include "version.h"
#include "crc.h"
#include "binding.h"
#include "hardware.h"
#include "wd.h"
#include "common.h"
#include "chpicker.h"
#include "cli.h"

uint8_t RF_channel = 0;

uint32_t lastSent = 0;
uint32_t lastReceived = 0;

uint8_t peerRSSI = 0;
uint8_t localRSSI = 0;

uint16_t linkQuality = 0;
uint8_t linkQualityPeer = 0;

uint32_t packetInterval = 0;

#ifndef BZ_FREQ
#define BZ_FREQ 2000
#endif

void __putc(char c)
{
  serialWriteSync(c);
}

void bindMode(void)
{
  uint32_t prevsend = millis();
  uint8_t  tx_buf[sizeof(bind_data) + 1];
  bool  sendBinds = 1;

  init_rfm(1);

  while (serialAvailable()) {
    serialRead();    // flush serial
  }

  Red_LED_OFF;

  while (1) {
    if (sendBinds & (millis() - prevsend > 200)) {
      prevsend = millis();
      Green_LED_ON;
      buzzerOn(BZ_FREQ);
      tx_buf[0] = 'b';
      memcpy(tx_buf + 1, &bind_data, sizeof(bind_data));
      tx_packet(tx_buf, sizeof(bind_data) + 1);
      Green_LED_OFF;
      buzzerOff();
      RF_Mode = Receive;
      rx_reset();
      delay(50);
      if (RF_Mode == Received) {
        RF_Mode = Receive;
        spiSendAddress(0x7f);   // Send the package read command
        if ('B' == spiReadData()) {
          sendBinds = 0;
        }
      }
    }

    if (!digitalRead(BTN)) {
      sendBinds = 1;
    }

    while (serialAvailable()) {
      Red_LED_ON;
      Green_LED_ON;
      switch (serialRead()) {
      case '\n':
      case '\r':
        printStrLn("Enter menu...");
        handleCLI();
        init_rfm(1);
        break;
      case '#':
        scannerMode();
        break;
      default:
        break;
      }
      Red_LED_OFF;
      Green_LED_OFF;
    }
  }
}

void bindRX(bool timeout)
{
  uint32_t start = millis();
  printStrLn("waiting bind...");
  init_rfm(1);
  RF_Mode = Receive;
  to_rx_mode();
  while(!timeout || ((millis() - start) < 500)) {
    if (RF_Mode == Received) {
      printStrLn("Got pkt\n");
      spiSendAddress(0x7f);   // Send the package read command
      uint8_t rxb = spiReadData();
      if (rxb == 'b') {
        for (uint8_t i = 0; i < sizeof(bind_data); i++) {
          *(((uint8_t*) &bind_data) + i) = spiReadData();
        }
        if (bind_data.version == BINDING_VERSION) {
          printStrLn("data good\n");
          rxb = 'B';
          tx_packet(&rxb, 1); // ACK that we got bound
          bindWriteEeprom();
          Red_LED_ON; //signal we got bound on LED:s
          Green_LED_ON; //signal we got bound on LED:s
          return;
        }
      }
    }

  }
}

#define MODE_MASTER 0
#define MODE_SLAVE  1
bool slaveMode = false;

static inline void checkOperatingMode()
{
  if (digitalRead(SLAVE_SELECT)) {
    slaveMode = false;
  } else {
    slaveMode = true;
  }
}

uint8_t tx_buf[MAX_PACKET_SIZE];
uint8_t rx_buf[MAX_PACKET_SIZE];

uint8_t pktbuf[MAX_PACKET_SIZE-1];
uint8_t pktindex = 100;
uint8_t pktsize  = 0;

#define MASTER_SEQ 0x80
#define SLAVE_SEQ  0x40

/* Frame format:

MASTER->SLAVE:
  byte0 : control (bitmasked)
  x------- seq_M_S master changes this when it is putting new data
  -x------ seq_S_M last bit seen by master is relayed back
  --xxxxxx
           x=0 idle
	   x < MAX_PACKET_SIZE DATA
	   else reserved

SLAVE->MASTER
  byte0 : control (bitmasked)
  x------- seq_M_S - not needed
  -x------ seq_S_M -
  --xxxxxx
           x=0 idle
	   x < MAX_PACKET_SIZE DATAPACK
	   else reserved  --1xxxxx data packet xxxxx + 1 bytes of data attached
    byte1... data (upto 32 if packetsize permits)
    if possible rssi and quality are appended
    byteN+1 RSSI
    byteN+2 quality
  ..000000 idle
    byte1 RSSI
    byte2 quality
*/

void setup(void)
{
  watchdogConfig(WATCHDOG_OFF);

  setupSPI();
#ifdef SDN_pin
  pinMode(SDN_pin, OUTPUT); //SDN
  digitalWrite(SDN_pin, 0);
#endif
  //LED and other interfaces
  pinMode(Red_LED, OUTPUT); //RED LED
  pinMode(Green_LED, OUTPUT); //GREEN LED
#ifdef Red_LED2
  pinMode(Red_LED2, OUTPUT); //RED LED
  pinMode(Green_LED2, OUTPUT); //GREEN LED
#endif
  // pinMode(BTN, INPUT); //Button
  pinMode(SLAVE_SELECT, INPUT);
  digitalWrite(SLAVE_SELECT, HIGH); // enable pullup for TX:s with open collector output
  buzzerInit();

  serialInit(115200,SERIAL_8N1);

  checkOperatingMode();

  printStr("OpenLRSng DL starting ");
  printVersion(version);
  printStr(" on HW ");
  printUL(BOARD_TYPE);
  printStr(" (");
  printUL(RFMTYPE);
  printStr("MHz) MDOE=");

  buzzerOn(BZ_FREQ);
  digitalWrite(BTN, HIGH);
  Red_LED_ON ;
  sei();

  delay(50);
  if (!slaveMode) {
    printStrLn("MASTER");
    if (!bindReadEeprom()) {
      printStrLn("eeprom bogus reinit....");
      bindInitDefaults();
      bindWriteEeprom();
    }
    if (!digitalRead(BTN)) {
      bindMode();
    }
  } else {
    printStrLn("SLAVE");
    if (!digitalRead(BTN) || !bindReadEeprom()) {
      bindRX(false);
    } else {
      bindRX(true);
    }
  }

  packetInterval = getInterval(&bind_data);

  printStrLn("Entering normal mode");

  serialFlush();
  serialInit(bind_data.serial_baudrate, bind_data.serial_mode);

  Red_LED_OFF;
  buzzerOff();

  init_rfm(0);
  rfmSetChannel(RF_channel);
  rx_reset();

  if (slaveMode) {
    to_rx_mode();
    RF_Mode = Receive;
  }

  watchdogConfig(WATCHDOG_2S);
  lastReceived=micros();
}


uint8_t state=0;
uint8_t lostpkts=10; //slow hop at start

uint16_t CRCtx, CRCrx;

void slaveLoop()
{
  watchdogReset();
  uint32_t now = micros();
  bool     needHop=false;
  switch (state) {
  case 0: // waiting for packet
    if (RF_Mode == Received) {
      Green_LED_ON;
      Red_LED_OFF;
      // got packet
      lastReceived = now;
      lostpkts=0;
      linkQuality = (linkQuality << 1) | 1;

      RF_Mode = Receive;

      spiSendAddress(0x7f); // Send the package read command
      for (int16_t i = 0; i < bind_data.packetSize; i++) {
        rx_buf[i] = spiReadData();
      }

      uint8_t payloadBytes = (rx_buf[0] & 0x3f);
      if (payloadBytes > (bind_data.packetSize - 1)) {
        // INVALID DATA
        payloadBytes = 0;
      }

      // Check if this is a new packet from master and not a resent one
      if ((rx_buf[0] ^ tx_buf[0]) & MASTER_SEQ) {
        tx_buf[0] ^= MASTER_SEQ;
        if (payloadBytes) {
          // DATA FRAME
          if (bind_data.flags & PACKET_MODE) {
            serialWrite(0xf0);
            serialWrite(payloadBytes);
            CRCtx = 0;
          }
          for (uint8_t i=0; i < payloadBytes; i++) {
            serialWrite(rx_buf[1 + i]);
            if ((bind_data.flags & PACKET_MODE) && (bind_data.flags & PACKETCRC_MODE)) {
              CRC16_add(&CRCtx, rx_buf[1 + i]);
            }
          }
          if ((bind_data.flags & PACKET_MODE) && (bind_data.flags & PACKETCRC_MODE)) {
            serialWrite(CRCtx >> 8);
            serialWrite(CRCtx & 0xff);
          }
        }
      }

      // extract peerRSSI if it is attached
      if (payloadBytes) {
        if ((payloadBytes + 1) < bind_data.packetSize ) {
          peerRSSI = rx_buf[payloadBytes + 1];
        }
      } else {
        peerRSSI = rx_buf[1];
      }

      // construct TX packet, resend if the ack was not done
      if (!((rx_buf[0] ^ tx_buf[0]) & SLAVE_SEQ)) {
        uint8_t i = 0;
        if (bind_data.flags & PACKET_MODE) {
          if (pktindex == 255) {
            for (i=0; i < pktsize; i++) {
              tx_buf[i + 1] = pktbuf[i];
            }
            pktindex=100;
            serialWrite(0xf0);
            serialWrite(0xff);
          }
        } else {
          for (i=0; serialAvailable() && (i < (bind_data.packetSize-1)); i++) {
            tx_buf[i + 1] = serialRead();
          }
        }
        tx_buf[0] &= MASTER_SEQ | SLAVE_SEQ;
        if (i) {
          tx_buf[0] |= i;
          tx_buf[0] ^= SLAVE_SEQ;
        }
      }

      // fill in RSSI if it fits
      payloadBytes = (tx_buf[0] & 0x3f);
      if (payloadBytes) {
        if ((payloadBytes + 1) < bind_data.packetSize ) {
          tx_buf[payloadBytes + 1] = localRSSI;
        }
      } else {
        tx_buf[1] = localRSSI;
      }

      state = 1;
      tx_packet_async(tx_buf, bind_data.packetSize);
      if ((bind_data.flags & (PACKET_MODE | STATUSPACKET_MODE)) == (PACKET_MODE | STATUSPACKET_MODE)) {
        serialWrite(0xf0);
        serialWrite(0x00); // indicate this is status packet
        serialWrite(localRSSI); // local RSSI
        serialWrite(peerRSSI);  // remote RSSI
        serialWrite(countSetBits(linkQuality)); // linkq
      }
      localRSSI = 0;
    } else {
      uint8_t tmp = rfmGetRSSI();
      if (tmp > localRSSI) {
        localRSSI = tmp;
      }
      if ((now - lastReceived) > (packetInterval + 500)) {
        Red_LED_ON;
        linkQuality = (linkQuality << 1);
        if ((bind_data.flags & (PACKET_MODE | STATUSPACKET_MODE)) == (PACKET_MODE | STATUSPACKET_MODE)) {
          serialWrite(0xf0);
          serialWrite(0x00); // indicate this is status packet
          serialWrite(localRSSI); // local RSSI
          serialWrite(peerRSSI);  // remote RSSI
          serialWrite(countSetBits(linkQuality)); // linkq
        }

        if (lostpkts++ < 10) {
          needHop=1;
        } else {
          if (lostpkts > 25) {
            needHop=1;
            lostpkts=10;
          }
        }
        // missed a packet
        lastReceived += packetInterval;
      }
    }
    break;
  case 1: // waiting TX completion
    switch (tx_done()) {
    case 2: // tx timeout
      // rfm init ??
    case 1: // ok
      Green_LED_OFF;
      state = 0;
      needHop=1;
      rfmSetChannel(RF_channel);
      RF_Mode = Receive;
      rx_reset();
      break;
    }
    break;
  }
  if (needHop) {
    RF_channel++;
    if ((RF_channel == MAXHOPS) || (bind_data.hopchannel[RF_channel] == 0)) {
      RF_channel = 0;
    }
    rfmSetChannel(RF_channel);
  }
}

void masterLoop()
{
  uint32_t now = micros();

  if (RF_Mode == Received) {
    // got packet
    Red_LED_OFF;

    lastReceived = (micros() | 1);
    linkQuality |= 1;
    RF_Mode = Receive;

    spiSendAddress(0x7f); // Send the package read command
    for (int16_t i = 0; i < bind_data.packetSize; i++) {
      rx_buf[i] = spiReadData();
    }
    uint8_t payloadBytes = rx_buf[0] & 0x3f;
    if (payloadBytes > (bind_data.packetSize - 1)) {
      payloadBytes = 0;
    }
    if ((rx_buf[0] ^ tx_buf[0]) & SLAVE_SEQ) {
      tx_buf[0] ^= SLAVE_SEQ;
      if (payloadBytes) {
        // DATA FRAME
        if (bind_data.flags & PACKET_MODE) {
          serialWrite(0xf0);
          serialWrite(payloadBytes);
          CRCtx = 0;
        }
        for (uint8_t i=0; i < payloadBytes; i++) {
          serialWrite(rx_buf[1 + i]);
          if ((bind_data.flags & PACKET_MODE) && (bind_data.flags & PACKETCRC_MODE)) {
            CRC16_add(&CRCtx, rx_buf[1 + i]);
          }
        }
        if ((bind_data.flags & PACKET_MODE) && (bind_data.flags & PACKETCRC_MODE)) {
          serialWrite(CRCtx >> 8);
          serialWrite(CRCtx & 0xff);
        }
      }
    }
    // extract peerRSSI if it is attached
    if (payloadBytes) {
      if ((payloadBytes + 1) < bind_data.packetSize ) {
        peerRSSI = rx_buf[payloadBytes + 1];
      }
    } else {
      peerRSSI = rx_buf[1];
    }
  }

  uint8_t tmp = rfmGetRSSI();
  if (tmp > localRSSI) {
    localRSSI = tmp;
  }

  if ((now - lastSent) >= packetInterval) {
    lastSent = now;

    watchdogReset();

    if (lastReceived) {
      if ((now - lastReceived) > packetInterval) {
        // telemetry lost
        Red_LED_ON;
        if (!(bind_data.flags & MUTE_TX)) {
          buzzerOn(BZ_FREQ);
        }
        lastReceived = 0;
      } else {
        // telemetry link re-established
        buzzerOff();
      }
    }

    // Construct packet to be sent, if slave did not respond resend last
    Green_LED_ON;
    if (!((rx_buf[0] ^ tx_buf[0]) & MASTER_SEQ)) {
      uint8_t i = 0;
      if (bind_data.flags & PACKET_MODE) {
        if (pktindex == 255) {
          for (i = 0; i < pktsize; i++) {
            tx_buf[i + 1] = pktbuf[i];
          }
          pktindex = 100;
          serialWrite(0xf0);
          serialWrite(0xff);
        }
      } else {
        for (i=0; serialAvailable() && (i < (bind_data.packetSize-1)); i++) {
          tx_buf[i+1] = serialRead();
        }
      }
      tx_buf[0] &= MASTER_SEQ | SLAVE_SEQ;
      if (i) {
        tx_buf[0] |= i;
        tx_buf[0] ^= MASTER_SEQ;
      }
    }

    // fill in RSSI if it fits
    uint8_t payloadBytes = tx_buf[0] & 0x3f;
    if (payloadBytes) {
      if ((payloadBytes + 1) < bind_data.packetSize ) {
        tx_buf[payloadBytes + 1] = localRSSI;
      }
    } else {
      tx_buf[1] = localRSSI;
    }

    // Send the data over RF on the next frequency
    RF_channel++;
    if ((RF_channel == MAXHOPS) || (bind_data.hopchannel[RF_channel] == 0)) {
      RF_channel = 0;
    }
    rfmSetChannel(RF_channel);
    tx_packet_async(tx_buf, bind_data.packetSize);
    if ((bind_data.flags & (PACKET_MODE | STATUSPACKET_MODE)) == (PACKET_MODE | STATUSPACKET_MODE)) {
      serialWrite(0xf0);
      serialWrite(0x00); // indicate this is status packet
      serialWrite(localRSSI); // local RSSI
      serialWrite(peerRSSI);  // remote RSSI
      serialWrite(countSetBits(linkQuality)); // linkq
    }
    localRSSI = 0;
  }

  if (tx_done() == 1) {
    linkQuality <<= 1;
    RF_Mode = Receive;
    rx_reset();
  }
  Green_LED_OFF;
}

void loop(void)
{
  if (spiReadRegister(0x0C) == 0) {     // detect the locked module and reboot
    printStrLn("module locked?");
    Red_LED_ON;
    init_rfm(0);
    rx_reset();
    Red_LED_OFF;
  }

  // pktindex
  // 0 - (pktsize-1) grabbing data
  // 100 sync wait
  // 101 sync seen
  // 102 get crchi
  // 103 get crclo
  // 255 packet ready

  if (bind_data.flags & PACKET_MODE) {
    while (serialAvailable() && (pktindex != 255)) {
      uint8_t d = serialRead();
      if (pktindex == 100) {
        if (d == 0xf0) {
          pktindex = 101;
        }
      } else if (pktindex == 101) {
        if ((d > 0) && (d < bind_data.packetSize)) {
          pktsize = d;
          pktindex = 0;
          CRCrx = 0;
        } else  {
          // INVALID SIZE
          if (d != 0xf0) { // leave to 101 if byte is 0xf0 (syncbyte)
            pktindex = 100;
          }
        }
      } else if (pktindex == 102) {
        CRCrx ^= ((uint16_t)d) << 8;
        pktindex=103;
      } else if (pktindex == 103) {
        if (CRCrx == d) {
          pktindex = 255;
        } else {
          serialWrite(0xf0);
          serialWrite(0xfe);
          pktindex = 100;
        }
      } else {
        pktbuf[pktindex++] = d;
        if (bind_data.flags & PACKETCRC_MODE) {
          CRC16_add(&CRCrx, d);
        }
        if (pktindex == pktsize) {
          pktindex = (bind_data.flags & PACKETCRC_MODE) ? 102 : 255;
        }
      }
    }
  }

  if (slaveMode) {
    slaveLoop();
  } else {
    masterLoop();
  }
}

int main(void)
{
  init();

  setup();

  for (;;) {
    loop();
  }

  return 0;
}

