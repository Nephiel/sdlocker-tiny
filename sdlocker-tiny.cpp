/*
 * sdlocker-tiny      Lock/unlock an SD card, uses ATTINY85
 *                    By Nephiel
 *
 * Based on sdlocker by karllunt (http://www.seanet.com/~karllunt/sdlocker.html)
 *
 *
 *              ATMEL ATTINY85
 *                   +-v-+
 *      nc     PB5  1|   |8  Vcc --- +3.3V
 *      CS <-- PB3  2|   |7  PB2 --> SCK
 *   LEDSW <-> PB4  3|   |6  PB1 <-- MISO
 *     GND --- GND  4|   |5  PB0 --> MOSI
 *                   +---+
 *
 *
 *                LEDSW--+
 *                       |
 * +3.3V    R1     LED   |    R2    Switch
 *  Vcc----\/\/\---[>|---+---\/\/\---[*]----GND
 *          300               300
 *
 *
 *                SD CARD
 * _______
 * [ 9 ]  \ rsv      nc
 *   [ 1 ] |  CS <-- CS
 *   [ 2 ] |  DI <-- MOSI
 *   [ 3 ] | GND --- GND
 *   [ 4 ] | Vcc --- +3.3V
 *   [ 5 ] | CLK <-- SCK
 *   [ 6 ] | GND --- GND
 *   [ 7 ] |  DO --> MISO
 *   [ 8 ] | rsv     nc
 * --------+
 *
 *
 *            ADDITIONAL NOTES
 *
 *  You might need to change the fuses on the ATTINY85:
 *  lfuse=E2,  hfuse=DF,  efuse=FF or 01
 *
 *  Use the built-in card-detect switch on the SD card socket
 *  to cut the power to the circuit when the card is removed.
 *
 *  Use a LM3940 to obtain 3.3V from a 5V source such as USB.
 *
 */



#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/interrupt.h>
#include <inttypes.h>
#include <ctype.h>
#include <util/delay.h>

#ifndef FALSE
#define FALSE       0
#define TRUE        !FALSE
#endif


/*
 * Define commands for the SD card
 */
#define SD_GO_IDLE          (0x40 + 0)  // CMD0 - go to idle state
#define SD_INIT             (0x40 + 1)  // CMD1 - start initialization
#define SD_SEND_IF_COND     (0x40 + 8)  // CMD8 - send interface (conditional), works for SDHC only
#define SD_SEND_CSD         (0x40 + 9)  // CMD9 - send CSD block (16 bytes)
#define SD_SEND_CID         (0x40 + 10) // CMD10 - send CID block (16 bytes)
#define SD_SEND_STATUS      (0x40 + 13) // CMD13 - send card status
#define SD_SET_BLK_LEN      (0x40 + 16) // CMD16 - set length of block in bytes
#define SD_READ_BLK         (0x40 + 17) // read single block
#define SD_LOCK_UNLOCK      (0x40 + 42) // CMD42 - lock/unlock card
#define CMD55               (0x40 + 55) // multi-byte preface command
#define SD_READ_OCR         (0x40 + 58) // read OCR
#define SD_ADV_INIT         (0xc0 + 41) // ACMD41, for SDHC cards - advanced start initialization
#define SD_PROGRAM_CSD      (0x40 + 27) // CMD27 - get CSD block (15 bytes data + CRC)


/*
 * Define the lock bit mask within byte 14 of the CSD
 */
#define LOCK_BIT_MASK       0x10 // mask for the lock bit


/*
 * Define error codes that can be returned by local functions
 */
#define SDCARD_OK           0   // success
#define SDCARD_NOT_DETECTED 1   // unable to detect SD card
#define SDCARD_TIMEOUT      2   // last operation timed out
#define SDCARD_RWFAIL       3   // read/write command failed


/*
 * Define card types that could be reported by the SD card during probe
 */
#define SDTYPE_UNKNOWN      0   // card type not determined
#define SDTYPE_SD           1   // SD v1 (1 MB to 2 GB)
#define SDTYPE_SDHC         2   // SDHC (4 GB to 32 GB)


/*
 * Define the port and DDR used by the SPI.
 */
#define SPI_PORT    PORTB
#define SPI_DDR     DDRB
#define SPI_PIN     PINB


/*
 * Define bits used by the SPI port.
 */
#define MOSI_BIT    0
#define MISO_BIT    1
#define SCK_BIT     2
#define CS_BIT      3


/*
 * Define the port, DDR, and bit used for the LED and the switch.
 * The ATTINY85 doesn't have enough I/O pins so we need to share one.
 * Note the LED output is active low.
 */
#define LEDSW_PORT      PORTB
#define LEDSW_DDR       DDRB
#define LEDSW_PIN       PINB
#define LEDSW_BIT       4
#define LEDSW_MASK      (1<<LEDSW_BIT)

#define LEDSW_AS_LED    (LEDSW_DDR|=LEDSW_MASK)     // make the line an output
#define LEDSW_AS_SW     (LEDSW_DDR&=~LEDSW_MASK)    // make the line an input

#define TURN_LED_ON     (LEDSW_PORT&=~LEDSW_MASK)   // set line low, led on
#define TURN_LED_OFF    (LEDSW_PORT|=LEDSW_MASK)    // set line high, led off
#define SW_PULLUP       TURN_LED_OFF                // set line high, enable pullup
#define SW_GET_STATE    !(LEDSW_PIN & LEDSW_MASK)   // get switch state (1 = pressed)


/*
 * Define switch states
 */
#define SW_PRESSED      1
#define SW_RELEASED     0


/*
 * Define LED blinking patterns.
 */
#define PATTERN_LOCKED        0x80000000      // Led steady ON, card is locked (write-protected)
#define PATTERN_UNLOCKED      0x00000000      // Led steady OFF, card is unlocked (write allowed)
#define PATTERN_BOOTING       0x844b0000      // Device just powered up (or card just inserted, if using slot detect switch)
#define PATTERN_LOADING       0xa0000000      // Device trying to read the card. Fast blink 1
#define PATTERN_READING       0xa5000000      // Device trying to read registers from a card. Fast blink 2
#define PATTERN_FAILED        0x00030003      // Device could not change card lock state. Slow blink 1
#define PATTERN_WERROR        0x000f000f      // Device could not write registers to a card. Slow blink 2


/*
 * Define the CRC7 polynomial
 */
#define CRC7_POLY       0x89    // polynomial used for CSD CRCs



/*
 * Local variables
 */
uint32_t    LEDPattern; // Blinking patterns
uint8_t     sdtype;     // Flag for SD card type
uint8_t     csd[16];    // Card registers
uint8_t     cid[16];
uint8_t     crctable[256];


/*
 * Local functions
 */
static void     Select(void);
static void     Deselect(void);
static uint8_t  Xchg(uint8_t c);
static uint8_t  SDInit(void);
static uint8_t  ReadCSD(void);
static uint8_t  WriteCSD(void);

static uint8_t  SD_send_command(uint8_t command, uint32_t arg);
static uint8_t  SD_wait_for_data(void);

static void     GenerateCRCTable(void);
static uint8_t  AddByteToCRC(uint8_t crc, uint8_t b);

static void     BlinkLED(uint32_t pattern);
static uint8_t  ButtonIs(uint8_t state);
static uint8_t  ReadSwitchOnce(void);
static uint8_t  CardIsLocked(void);
static void     ReadState(void);
static void     ShowState(void);
static void     ToggleState(void);



int main(void)
{
    uint8_t prevState; // Last known state of the card (locked or unlocked)

    // Set up the hardware lines and ports associated with accessing the SD card.
    SPI_PORT |= (1<<MOSI_BIT) | (1<<SCK_BIT);                   // drive outputs to the SPI port
    SPI_DDR  |= (1<<CS_BIT) | (1<<MOSI_BIT) | (1<<SCK_BIT);     // make the proper lines outputs
    SPI_PORT |= (1<<MISO_BIT);                                  // turn on pull-up for input
    Deselect();                 // Start with SD card disabled

    GenerateCRCTable();         // Needed for some SD commands

    LEDSW_AS_LED;               // Set shared LED/switch pin as output (LED)
    BlinkLED(PATTERN_BOOTING);  // Test LED on power on
    ReadState();                // Read the card for the first time

    while (1)
    {
        ShowState();                // Display the current state

        if (ButtonIs(SW_PRESSED))   // If the user presses the button...
        {
            prevState = CardIsLocked();     // remember the current state
            ToggleState();                  // then, attempt to change it
            ReadState();                    // and read again to verify the change

            if (CardIsLocked() == prevState)    // if state did not change as expected
            {
                BlinkLED(PATTERN_FAILED);   // blink error a few times
                BlinkLED(PATTERN_FAILED);
                BlinkLED(PATTERN_FAILED);
            }

            ShowState();                    // Display the updated state, and...

            while (!ButtonIs(SW_RELEASED))  // ...wait until the button is released
            // note (ButtonIs(SW_PRESSED)) wouldn't do here, we want to debounce the releasing
            {
                 _delay_ms(25);
            }
        }
    } // end main while (1) loop

    return 0; // should never be reached
}



/*
 * ButtonIs(state)
 * Checks if the button matches the specified state (pressed or not).
 * Handles debouncing. Returns 1 on match, 0 otherwise.
 */
static uint8_t ButtonIs(uint8_t state)
{
    uint8_t match = 0;              // Assume state doesn't match
    uint8_t i;

    if (ReadSwitchOnce() == state)  // if switch state seems to match
    {
        match = 1;
        for (i=0; i<5; i++)         // debounce check every 100ms, 5 times
        {
            _delay_ms(100);
            if (ReadSwitchOnce() != state)  // if state doesn't match now
            {
                match = 0;          // terminate debounce check
                break;
            }
        }
    }
    return match;
}



/*
 * ReadSwitchOnce()
 * Checks if the switch is closed. NO debouncing.
 * Also handles I/O switching for the shared LEDSW pin, so that line
 * is always set as an output (LED) outside of this function.
 */
static uint8_t ReadSwitchOnce(void)
{
    uint8_t switchState;

    TURN_LED_OFF;   // Set line high to turn off LED
    LEDSW_AS_SW;    // Set shared pin as input (switch)
    SW_PULLUP;      // Enable internal pull-up by setting line high again

    switchState = SW_GET_STATE;

    LEDSW_AS_LED;     // Set shared pin as output (LED) again
    if (CardIsLocked())    // and if needed,
    {
        TURN_LED_ON;  // turn LED back on before returning
    }

    return switchState;
}



/*
 * BlinkLED(pattern)
 * Makes the LED blink in the specified pattern.
 */
void BlinkLED(uint32_t pattern)
{
    uint8_t i;
    for (i=0; i<32; i++)
    {
        if (pattern & 0x80000000)
        {
            TURN_LED_ON;
        }
        else
        {
            TURN_LED_OFF;
        }
        _delay_ms(35);
        pattern = pattern << 1;
        if (pattern == 0)
        {
            break; // leave blink loop if no more ON bits
        }
    }
}



/*
 * ReadState()
 * Read the locked/unlocked state from the card.
 * This function won't return until the state has been read.
 */
static void ReadState(void)
{
    uint8_t r;

    // In all cases, try first to initialize the card.
    r = SDInit();
    while (r != SDCARD_OK)
    {
        BlinkLED(PATTERN_LOADING);
        r = SDInit(); // keep trying
    }

    // Card initialized, now read the CSD
    r = ReadCSD();
    while (r != SDCARD_OK)
    {
        BlinkLED(PATTERN_READING);
        r = ReadCSD(); // keep trying
    }
}



/*
 * ShowState()
 * Shows the locked/unlocked state of the card, using the LED
 * LED steadily on  = card is locked (write-protected)
 * LED off          = card is unlocked
 */
static void ShowState(void)
{
    if (CardIsLocked())
    {
        BlinkLED(PATTERN_LOCKED);
    }
    else
    {
        BlinkLED(PATTERN_UNLOCKED);
    }
}



/*
 * ToggleState:
 * Toggle the locked/unlocked state on the card.
 */
static void ToggleState(void)
{
    uint8_t r;

    if (CardIsLocked())     // get ready to unlock it
    {
        csd[14] &= ~LOCK_BIT_MASK;   // clear bit 12 of CSD (temp lock)
    }
    else                    // otherwise, get ready to lock it
    {
      csd[14] |= LOCK_BIT_MASK;      // set bit 12 of CSD (temp lock)
    }

    r = WriteCSD(); // Attempt to write the new state to the card.
    if (r != SDCARD_OK) // If state not properly written...
    {
        BlinkLED(PATTERN_WERROR);   // ...notify this error
        BlinkLED(PATTERN_WERROR);
        BlinkLED(PATTERN_WERROR);
    }
}



/*
 * CardIsLocked()
 * Returns 1 if the card is locked, 0 otherwise
 */
static uint8_t CardIsLocked(void)
{
    return (csd[14] & LOCK_BIT_MASK); // check lock bit from CSD
}



/*
 * Select()
 * Selects (CS enable) the SD card.
 */
static void Select(void)
{
    SPI_PORT &= ~(1<<CS_BIT);
}



/*
 * Deselect:
 * Deselects (CS disable) the SD card.
 */
static void Deselect(void)
{
    SPI_PORT |= (1<<CS_BIT);
}



/*
 * Xchg(c)
 * Exchange a byte of data with the SD card via host's SPI bus.
 */
static uint8_t Xchg(uint8_t c)
{
    uint8_t bit = 0;

    // I tried to get the SPI to work following Atmel's USI specs, and failed
    // However, bit-banging works, so I'm going with that.
    for (bit=0; bit<8; bit++)   // Loop through 8 bits
    {
        if(c & 0x80) SPI_PORT |= (1<<MOSI_BIT); // If bit(7) of "c" is high
        else SPI_PORT &= ~(1<<MOSI_BIT);        // if bit(7) of "c" is low
        SPI_PORT |= (1<<SCK_BIT);               // Serial Clock Rising Edge
        c <<= 1;                                // Shift "c" to the left by one bit
        if(SPI_PIN & (1<<MISO_BIT)) c |= 0x01;  // If bit of slave c is high
        else c &= ~0x01;                        // if bit of slave c is low
        SPI_PORT &= ~(1<<SCK_BIT);              // Serial Clock Falling Edge
    }

    return c;   // Returns shifted c in value
}



/*
 * SDInit()
 * Initialize the SD card.
 */
static uint8_t SDInit(void)
{
    uint16_t    i;
    uint8_t     response;

    sdtype = SDTYPE_UNKNOWN;    // assume this fails
    /*
     * Begin initialization by sending CMD0 and waiting until SD card
     * responds with In Idle Mode (0x01). If the response is not 0x01
     * within a reasonable amount of time, there is no SD card on the bus.
     */
    Deselect();             // always make sure card was not selected
    for (i=0; i<10; i++)    // send several clocks while card power stabilizes
    {
        Xchg(0xff);
    }

    for (i=0; i<10; i++)
    {
        response = SD_send_command(SD_GO_IDLE, 0);  // send CMD0 - go to idle state
        if (response == 0x01)
        {
            break;
        }
    }
    if (response != 0x01)
    {
        return SDCARD_NOT_DETECTED;
    }

    response = SD_send_command(SD_SEND_IF_COND, 0x1aa); // check if card is SDv2 (SDHC)
    if (response == 0x01)                               // if card is SDHC...
    {
        for (i=0; i<4; i++)                             // burn the 4-byte response (OCR)
        {
            Xchg(0xff);
        }
        for (i=20000; i>0; i--)
        {
            response = SD_send_command(SD_ADV_INIT, 1UL<<30);
            if (response == 0)
            {
                break;
            }
        }
        sdtype = SDTYPE_SDHC;
    }
    else
    {                                               // if card is SD...
        response = SD_send_command(SD_READ_OCR, 0);
        if (response == 0x01)
        {
            for (i=0; i<4; i++)                     // burn the 4-byte response (OCR)
            {
                Xchg(0xff);
            }
            for (i=20000; i>0; i--)
            {
                response = SD_send_command(SD_INIT, 0);
                if (response == 0)
                {
                    break;
                }
            }
            SD_send_command(SD_SET_BLK_LEN, 512);
            sdtype = SDTYPE_SD;
        }
    }

    Xchg(0xff); // send 8 final clocks

    /*
     * At this point, the SD card has completed initialization. The calling routine
     * could now increase the SPI clock rate for the SD card to the maximum allowed by
     * the SD card (typically, 20 MHz).
     */

    return SDCARD_OK;   // if no power routine or turning off the card, call it good
}



/*
 * ReadCSD()
 * Reads the CSD from the card, storing it in csd[].
 */
static uint8_t ReadCSD(void)
{
    uint8_t i;
    uint8_t response;

    for (i=0; i<16; i++)
    {
        csd[i] = 0;
    }

    response = SD_send_command(SD_SEND_CSD, 0);
    response = SD_wait_for_data();
    if (response != 0xfe)
    {
        return SDCARD_RWFAIL;
    }

    for (i=0; i<16; i++)
    {
        csd[i] = Xchg(0xff);
    }

    Xchg(0xff); // burn the CRC
    return SDCARD_OK;
}



/*
 * WriteCSD()
 * Writes csd[] to the CSD on the card.
 */
static uint8_t WriteCSD(void)
{
    uint8_t     response;
    uint8_t     tcrc;
    uint16_t    i;

    response = SD_send_command(SD_PROGRAM_CSD, 0);
    if (response != 0)
    {
        return SDCARD_RWFAIL;
    }

    Xchg(0xfe); // send data token marking start of data block

    tcrc = 0;
    for (i=0; i<15; i++)    // for all 15 data bytes in CSD...
    {
        Xchg(csd[i]);           // send each byte via SPI
        tcrc = AddByteToCRC(tcrc, csd[i]);  // add byte to CRC
    }
    Xchg((tcrc<<1) + 1);        // format the CRC7 value and send it

    Xchg(0xff);         // ignore dummy checksum
    Xchg(0xff);         // ignore dummy checksum

    i = 0xffff;         // max timeout
    while (!Xchg(0xff) && (--i));   // wait until we are not busy

    if (i)
    {
        return SDCARD_OK;       // return success
    }
    else
    {
        return SDCARD_TIMEOUT;  // nope, didn't work
    }
}



static void GenerateCRCTable(void)
{
    uint16_t i, j;

    // generate a table value for all 256 possible byte values
    for (i=0; i<256; i++)
    {
        crctable[i] = (i & 0x80) ? i ^ CRC7_POLY : i;
        for (j=1; j<8; j++)
        {
            crctable[i] <<= 1;
            if (crctable[i] & 0x80)
            {
                crctable[i] ^= CRC7_POLY;
            }
        }
    }
}



static uint8_t AddByteToCRC(uint8_t crc, uint8_t b)
{
    return crctable[(crc << 1) ^ b];
}




/*
 * SD_send_command(command, arg)
 * Sends a raw command to SD card, returns the response.
 *
 * This routine accepts a single SD command and a 4-byte argument. It sends
 * the command plus argument, adding the appropriate CRC. It then returns
 * the one-byte response from the SD card.
 *
 * For advanced commands (those with a command byte having bit 7 set), this
 * routine automatically sends the required preface command (CMD55) before
 * sending the requested command.
 *
 * Upon exit, this routine returns the response byte from the SD card.
 * Possible responses are:
 *   0xff   No response from card; card might actually be missing
 *   0x01   SD card returned 0x01, which is OK for most commands
 *   0x??   other responses are command-specific
 */
static uint8_t SD_send_command(uint8_t command, uint32_t arg)
{
    uint8_t response;
    uint8_t i;
    uint8_t crc;

    if (command & 0x80)     // special case, ACMD(n) is sent as CMD55 and CMDn
    {
        command = command & 0x7f;               // strip high bit for later
        response = SD_send_command(CMD55, 0);   // send first part (recursion)
        if (response > 1)
        {
            return response;
        }
    }

    Deselect();
    Xchg(0xff);
    Select();   // enable CS
    Xchg(0xff);

    Xchg(command | 0x40);       // command always has bit 6 set!
    Xchg((uint8_t)(arg>>24));   // send data, starting with top byte
    Xchg((uint8_t)(arg>>16));
    Xchg((uint8_t)(arg>>8));
    Xchg((uint8_t)(arg&0xff));
    crc = 0x01;                 // good for most cases

    if (command == SD_GO_IDLE)
    {
        crc = 0x95;             // this will be good enough for most commands
    }
    if (command == SD_SEND_IF_COND)
    {
        crc = 0x87;             // special case, have to use different CRC
    }

    Xchg(crc);                  // send final byte

    for (i=0; i<10; i++)        // loop until timeout or response
    {
        response = Xchg(0xff);
        if ((response & 0x80) == 0)
        {
            break;              // high bit cleared means we got a response
        }
    }

    /*
     * We have issued the command but the SD card is still selected. We
     * only deselect the card if the command we just sent is NOT a command
     * that requires additional data exchange, such as reading or writing
     * a block.
     */
    if ((command != SD_READ_BLK) &&
        (command != SD_READ_OCR) &&
        (command != SD_SEND_CSD) &&
        (command != SD_SEND_STATUS) &&
        (command != SD_SEND_CID) &&
        (command != SD_SEND_IF_COND) &&
        (command != SD_LOCK_UNLOCK) &&
        (command != SD_PROGRAM_CSD))
    {
        Deselect(); // all done
        Xchg(0xff); // close with eight more clocks
    }

    return response;    // let the caller sort it out
}


static uint8_t SD_wait_for_data(void)
{
    uint8_t i;
    uint8_t r;

    for (i=0; i<100; i++)
    {
        r = Xchg(0xff);
        if (r != 0xff)
        {
            break;
        }
    }
    return r;
}
