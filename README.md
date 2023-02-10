Mutable Instruments' Eurorack Modules.
Alternate firmwares for various modules:

Plaits tidified:
=======
The waveshape oscillator algoritme is more in line with the real Tides module.
The harmonics shape are 1:1.
The filter is in the first half of the timbre knob then switches to a waveshaper.

Holding the left button and turning the attenuverters have various functions:
Timbre:
Model cv input selection
First half normal behavior
Second half acts as a sync input for osc models
FM: 
The first half activates a sine sub oscillator on aux (unaffected by the lpg/level)
Setting it to noon will set it to the default aux
The second half activates a square sub oscillator on aux (unaffected by the lpg/level)
Morph:
Crossfades the output from normal output towards the aux variant. (not the sub oscillators)

Tides freqlock:
=======
Hold the frequency button to lock the frequency and activates a transpose function on the frequency knob.

License
=======

Code (AVR projects): GPL3.0.

Code (STM32F projects): MIT license.

Hardware: cc-by-sa-3.0

By: Emilie Gillet (emilie.o.gillet@gmail.com)

Guidelines for derivative works
===============================

**Mutable Instruments is a registered trademark.**

The name "Mutable Instruments" should not be used on any of the derivative works you create from these files.

We do not recommend you to keep the original name of the Mutable Instruments module for your derivative works.

For example, your 5U adaptation of Mutable Instruments Clouds can be called "Foobar Modular - Particle Generator".
