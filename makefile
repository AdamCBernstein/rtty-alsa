CFLAGS = -g -I../alsa-utils -Wall -DALSA -DASOUNDLIB_H
all: rtty-alsa

rtty-alsa: rtty-alsa.o
	cc -o rtty-alsa rtty-alsa.o -lasound -lm
clean: 
	rm -f rtty-alsa.o rtty-alsa
