rtty-alsa is generates Audio Frequency Shift Key (AFSK) tones that are
compatible with any Radio Teletype (RTTY) equipment that can decode
(A)FSK tones to DC keying pulses. 

This project was originally written using the Linux Open Sound System 
(OSS), which is generally no longer supported. The port to ALSA was done
by comparing very simple ALSA programs to initialize the sound card.

The original tone generator code came from this project
  dtmf-encode_bit 0.1
  (C) 1998 Itai Nahshon (nahshon@actcom.co.il)

The Dual Tone generation is not needed by this project. Conceptually, 
the tone generation in rtty-alsa is the same as part of the dtmf-encode_bit
project. In no way is rtty-alsa a fork of the this DTMF project.

Much of the ASCII to Baudot 5-bit code in this project is very similar to
rtty-rpi (https://github.com/AdamCBernstein/rtty-rpi). Perhaps there is a
refactoring opportunity here to unify the AFSK tone geenertion code into
this project.

Dependencies:
  libasound2-dev
  apt-get install libasound2-dev
