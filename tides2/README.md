# Mutable Instruments - Tides 2: Freshets

[**_Freshets_**](https://en.wikipedia.org/wiki/Freshet) builds on the [**_Tides Symbiote_**](https://leandrob13.github.io/Electronic-Ruminations/tides-symbiote/) firmware by Leandro B, continuing development with frequency locking, an alt output layer for every output mode, Formant oscillator and additional engine refinements. 

<br>

For the latest update file go to the [**release page**](https://github.com/Dylan-Bolink/eurorack/releases).<br>
For the orginal Tides manual go to the [**Tides manual**](https://pichenettes.github.io/mutable-instruments-documentation/modules/tides_2018/manual/).

<br>

## Contents

1. [Frequency Lock](#frequency-lock)
2. [Alt Output](#alt-output)
   - [Alt: Normal](#alt-normal)
   - [Alt: Amplitude](#alt-amplitude)
   - [Alt: Slope / Phase](#alt-slope--phase)
   - [Alt: Frequency](#alt-frequency)
3. [Engines](#engines)
   - [Attractors](#attractors)
   - [Formant](#formant)
   - [Wavetable](#wavetable)
   - [Chord](#chord)
4. [Patching Ideas](#patching-ideas)
5. [Credits](#credits)

<br>

## Frequency Lock

Long-press **Range** to lock the current frequency. The range LED blinks to indicate lock is active. While locked, short-pressing Range cycles through transpose modes.

Long-press **Range** again to unlock.

| Color | Function | Range |
|-------|----------|--------|
| **Green** | Semitones | +- 1 octave |
| **Yellow** | Fifths and octaves | +- 2 octaves |
| **Red** | Octaves | +- 4 octaves |

> Patching a cable into **Clock** exits frequency lock since you enter [**clocked speed**](https://arc.net/l/quote/tgzhboie) from the original firmware.

<br>

## Alt Output

Long-press **Output mode** to toggle the alt output. Alt output changes the behavior of the current output mode.

**LED indicator:**
- When the output has a visible LED color: the LED blinks on/off
- When the output LED is normally off: the LED cycles through green, yellow, red as a short flash

<br>

<a id="alt-normal"></a>

### <img src="https://pichenettes.github.io/mutable-instruments-documentation/modules/tides_2018/images/mode_0.png" style="width:27px; height: 27px; transform: translateY(5px); margin-right: 5px;"> Alt: Normal

Replaces the wavefolder with a more crude digital wavefolder.

<a id="alt-amplitude"></a>

### <img src="https://pichenettes.github.io/mutable-instruments-documentation/modules/tides_2018/images/mode_1.png" style="width:27px; height: 27px; transform: translateY(5px); margin-right: 5px;"> Alt: Amplitude

Instead of smooth crossfading, **shift** selects one output at a time. Four discrete positions, no blending.

<a id="alt-slope--phase"></a>

### <img src="https://pichenettes.github.io/mutable-instruments-documentation/modules/tides_2018/images/mode_2.png" style="width:27px; height: 27px; transform: translateY(5px); margin-right: 5px;"> Alt: Slope / Phase

**Shift** spreads different waveshapes(shape parameter) across the four outputs instead of phase offsets. 

<a id="alt-frequency"></a>

### <img src="https://pichenettes.github.io/mutable-instruments-documentation/modules/tides_2018/images/mode_3.png" style="width:27px; height: 27px; transform: translateY(5px); margin-right: 5px;"> Alt: Frequency

Mixes voices into outputs.

| Output | Function |
|--------|----------|
| **1** | Root voice (unchanged) |
| **2** | Mix of all 4 voices |
| **3** | Odd mix (voices 1 + 3) |
| **4** | Even mix (voices 2 + 4) |

<br>

## Engines

The first **Ramp mode** (no LED) activates specialized synthesis engines. Each output mode selects a different engine.

<img src="https://pichenettes.github.io/mutable-instruments-documentation/modules/tides_2018/images/mode_0.png" style="width:22px; height: 22px;"> The other engines.<br>
<img src="https://pichenettes.github.io/mutable-instruments-documentation/modules/tides_2018/images/icon_ad_envelope.png" style="width:22px; height: 22px;"> One-shot unipolar AD envelope generation.<br>
<img src="https://pichenettes.github.io/mutable-instruments-documentation/modules/tides_2018/images/icon_cyclic.png" style="width:22px; height: 22px;"> Cyclic bipolar oscillations.<br>
<img src="https://pichenettes.github.io/mutable-instruments-documentation/modules/tides_2018/images/icon_ar_envelope.png" style="width:22px; height: 22px;"> One-shot unipolar AR envelope generation.<br>

<a id="attractors"></a>

### <img src="https://pichenettes.github.io/mutable-instruments-documentation/modules/tides_2018/images/mode_0.png" style="width:27px; height: 27px; transform: translateY(5px); margin-right: 5px;"> Attractors

Two chaotic attractor systems running simultaneously.

| Output | Normal | Alt |
|--------|--------|-----|
| **1** | Lorenz X | Thomas X |
| **2** | Lorenz Y | Thomas Y |
| **3** | Rossler X | Chua X |
| **4** | Rossler Y | Chua Y |

| Knob / input | Function |
|------|----------|
| **Slope** | Lorenz chaos / Thomas damping |
| **Smoothness** | Rossler chaos / Chua drive |
| **Shift** | Output gain |
| **Trig** | Resets attractor pair 1 (Lorenz / Thomas) |
| **Clock** | Resets attractor pair 2 (Rossler / Chua) |

#### Alt: Thomas + Chua

Switches from the Lorenz and Rossler pair to Thomas and Chua attractors. Thomas produces smooth, flowing 3D orbits. Chua is a circuit-based attractor with spikier, more unpredictable behavior.

<a id="formant"></a>

### <img src="https://pichenettes.github.io/mutable-instruments-documentation/modules/tides_2018/images/mode_1.png" style="width:27px; height: 27px; transform: translateY(5px); margin-right: 5px;"> Formant

A driver oscillator excites a Serge style envelope generator (formant envelope). Serge envelope generaters skips pulses if the envelope is stil in a attacking phase. The formant envelope follows the frequency of the driver oscillator except in the alt output.

| Knob | Function |
|------|----------|
| **Frequency** | Driver frequency |
| **Slope** | AD balance control of the formant envelope |
| **Shape** | Shape of formant envelope |
| **Smoothness** | Filtering/wavefolding the formant envelope |
| **Shift** | Formant frequency ratio (+-48 semitones) |

| Output | Function |
|--------|----------|
| **1** | Formant waveform (formant) |
| **2** | Logic combined pulse (formant) |
| **3** | Pulse wave (driver) |
| **4** | Sine wave (driver) |

> The logic combined pulse is a 3 state pulse wave where smoothness attenuates it at the first half. The second half of smoothness introduces a sub square oscillator.

> With the **shift** attenuverter fully open, the **shift** CV input tracks approximately v/oct.

#### Trig input
Patching the **trig** input decouples the formant envelope from the driver oscillator. The driver pulse is normalized in the software to the **trig** input if not patched.

#### Alt: Decouple formant frequency

The formant frequency no longer tracks the driver pitch. Instead, **Shift** sets an absolute formant frequency.


<a id="wavetable"></a>

### <img src="https://pichenettes.github.io/mutable-instruments-documentation/modules/tides_2018/images/mode_2.png" style="width:27px; height: 27px; transform: translateY(5px); margin-right: 5px;"> Wavetable

A 3D wave terrain synthesizer. Three knobs navigate a grid of wavetables.

| Output | Function |
|--------|----------|
| **1** | Wavetable bipolar |
| **2** | Wavetable unipolar |
| **3** | 1-bit wavetable output |
| **4** | Sub-octave pulse |

> The 1-bit output is a nod to the [**_sheep_**](https://pichenettes.github.io/mutable-instruments-documentation/modules/tides_original/firmware/) firmware.

| Knob | Function |
|------|----------|
| **Slope** | X axis |
| **Shift** | Y axis |
| **Shape** | Z axis |
| **Smoothness** | Smoothness |

#### Alt: No Interpolation

All interpolation between wavetable positions is removed. X, Y, and Z snap to the nearest grid point.

<a id="chord"></a>

### <img src="https://pichenettes.github.io/mutable-instruments-documentation/modules/tides_2018/images/mode_3.png" style="width:27px; height: 27px; transform: translateY(5px); margin-right: 5px;"> Chord

Four-voice chord generator using the same engine as normal frequency mode but with a chord ratio table.

#### Alt: Harmonic Mix

Same behavior as the normal frequency alt output:

| Output | Function |
|--------|----------|
| **1** | Root voice (unchanged)  |
| **2** | Mix of all 4 voices |
| **3** | Odd mix (voices 1 + 3) |
| **4** | Even mix (voices 2 + 4) |


<br>

## Patching Ideas

- Use the formant engine with the alt output and a slow LFO on the shift CV. The fixed formant creates evolving vowel textures as the driver pitch moves independently.
- The chord and different frequency mode alt outputs (root + mix + odd + even) into a stereo mixer give instant wide chord pads. Pan odd and even hard left/right.
- MORE PATCHES!

<br>

## Credits

- Original Tides firmware by **Émilie Gillet** (Mutable Instruments)
- Symbiote Tides firmware by **Leandro B**
- Chord table from **Jon Butler**
