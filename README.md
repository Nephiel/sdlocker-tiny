sdlocker-tiny
=============
**Attiny85**-based device to **enable** and **disable write-protection** on **any SD card**.
[![sdlocker-tiny built inside the shell of an old PlayStation Memory Card](https://raw.githubusercontent.com/Nephiel/sdlocker-tiny/gh-pages/img/sdlocker-tiny-sm.jpg "sdlocker-tiny built inside the shell of an old PlayStation Memory Card")](https://raw.githubusercontent.com/Nephiel/sdlocker-tiny/gh-pages/img/sdlocker-tiny.jpg)  
Based on **sdlocker** by ***Karl Lunt***, see http://www.seanet.com/~karllunt/sdlocker.html


I routinely use **USB drives** loaded with software tools and benchmarks to diagnose and fix computers. I wanted a way to protect those drives from viruses, malware, filesystem corruption and accidental erase.

USB drives with a **write-protect switch** do exist, but are hard to find and expensive. Full-sized SD cards have a slider tab to write-protect them, but **it actually does** ***nothing*** - it's up to the card reader to report it to the OS, and it's up to the OS to decide whether to comply and mount the card read-only or not. Most of them simply ignore it, so **SD cards may be overwritten regardless of the write-protect tab**.

The **sdlocker** allows me to ***truly* write-protect any SD card** by toggling the TMP\_WRITE\_PROTECT bit on the flash memory controller of the card itself. Together with a USB card reader, this write-protected card **can then be used as a read-only USB drive**.

This is my own fork of the original sdlocker, tailored to suit my needs - smaller, simpler, and USB-powered.


Changes from original sdlocker
------------------------------

- Ported from **Atmega328** to **Attiny85**
- Removed all the **UART code** and **unused functions**
- Rewrote the **user interface** (1 button, 1 LED)


Device Usage
------------

The **LED** shows the **state of the inserted card** at all times:
- **Steady off:** card is unlocked (writable)
- **Steady on:** card is locked (write-protected)
- **Blinking, fast:** device is reading the card
- **Blinking, slow:** card is faulty or not properly inserted

**Holding the button** down (over half a second) **toggles** the write-protection of the inserted card.


Schematic
---------

- See *sdlocker-tiny.cpp* for ASCII schematic.

- The button and LED share the same pin on the Attiny.

- The RESET pin on the Attiny is left unconnected. It could be turned into another I/O pin by changing the appropiate fuse, but flashing the chip after that (even to change the fuse back) would require a HV programmer.

- The SD card socket has a card-detect switch that shorts a pin to ground when a card is inserted.  
  I used this pin as ground to cut the power to the device while there is no card.

- To turn the 5V from USB into 3.3V, I used a Texas Instruments LM3940 + 2 capacitors  
  (see the LM3940 datasheet, *Typical application*)

- Both the Attiny85 and the SD card **run on 3.3V**. **Do *not* power them with 5V** from USB!  
  The Attiny can take it, but the card will be destroyed.


Compiling and Flashing
----------------------

The provided makefile uses **avr-gcc** and **avrdude**. Edit it to match your programmer.  
Run *make* to compile and *make program* to flash.

