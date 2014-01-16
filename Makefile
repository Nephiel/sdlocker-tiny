# Makefile for avr-gcc, avrdude

SRC=sdlocker-tiny
BAUD=19200
PRGDEV=/dev/ttyACM0
PRGTYPE=avrisp
AVRTYPE=attiny85
AVRTYPESHORT=t85
AVRFREQ=8000000
CFLAGS=-g -DF_CPU=$(AVRFREQ) -Wall -Os -Werror -Wextra

all : $(SRC).hex

program : $(SRC).hex
	avrdude -P $(PRGDEV) -b $(BAUD) -c $(PRGTYPE) -p $(AVRTYPESHORT) -v -e -U flash:w:$(SRC).hex

$(SRC).o : $(SRC).cpp
	avr-gcc $(CFLAGS) -mmcu=$(AVRTYPE) -Wa,-ahlmns=$(SRC).lst -c -o $(SRC).o $(SRC).cpp

$(SRC).elf : $(SRC).o
	avr-gcc $(CFLAGS) -mmcu=$(AVRTYPE) -o $(SRC).elf $(SRC).o

$(SRC).hex : $(SRC).elf
	avr-objcopy -j .text -j .data -O ihex $(SRC).elf $(SRC).hex

clean :
	rm -f *.hex *.obj *.o *.lst *.elf
