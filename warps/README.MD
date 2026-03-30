# Mutable Instruments – Warps: Artifacts

A reimagining of Mutable Instruments canceled module [**_Codes_**](https://pichenettes.github.io/mutable-instruments-documentation/trivia_and_history/cancelled_projects/), Artifacts explores the beauty of digital decay.
While Codes was conceived as a two-layer audio degradation module, Artifacts reinterprets that vision. Using a single meta knob to morph between two degradation forms, blurring the boundary between the analog past and digital ruin.

---

## Modes

You switch modes the same way as in the Parasites firmware:  
Hold the **Int. Osc** button and turn the big knob.

| # | Mode | Source |
|---|------|---------|
| 1 | **Crush** | Artifacts |
| 2 | **Lo-Fi** | Artifacts |
| 3 | **Modern Lo-Fi** | Artifacts |
| 4 | **Radio** | Artifacts |
| 5 | **Dream** | Artifacts |
| 6 | **Frequency Shifter** | Mutable Instruments |
| 7 | **Dual Bit Mangler** | Parasites |
| 8 | **Delay / Locked Delay** | Parasites + Artifacts tweak |
| 9 | **Meta (original)** | Mutable Instruments |

---

# Artifacts Modes

All new Artifacts modes use a **crossfader** before the main signal path.  
The **Timbre knob** acts as that crossfader, and the **Aux output** carries the inverse signal.  
The internal oscillator behaves as in the original firmware.

Each mode introduces **stereo separation**, achieved through subtle differences in processing between L (main) and R (aux) channels.

---

### 1. Crush
> *Digital math destruction — from quantized chaos to alien harmonics.*

- **Left side (CCW)**: Bitcrusher — bitwise operations (OR, XOR, SHIFT) on quantized signals. Quality controls bit depth.
- **Right side (CW)**: Chebyshev waveshaping — comparator logic fed through polynomial distortion. Quality controls harmonic degree.

---

### 2. Lo-Fi
> *Tape nostalgia from cassette hiss to VHS snow.*

- **Left side (CCW)**: cassette emulation with wow, flutter, hiss, and EQ shaping.  The last part introduces half speed and quarter speed.  
- **Right side (CW)**: VHS emulation with dropouts, motor hum, hiss, snow, and color shifts. At the far end the tape is nearly destroyed.

---

### 3. Modern Lo-Fi
> *MP3 dreams and broken LimeWire downloads.*

- **Left side (CCW)**: CORRUPT PLACEHOLDER TEXT
- **Right side (CW)**: Decimator, micro-looping and stutter, evolving into MP3-style “birdie” artifacts. At the final 1%, the effect enters a looping state. The **Timbre knob** becomes a loop-speed control.

### 4. Radio
> *Transmission artifacts and multipath interference.*

- **Left side (CCW)**: AM/FM demodulation artifacts — carrier bleed, bandwidth limiting, rectification distortion, static, and signal fading. Quality controls radio band (strong signal to numbers station).
- **Right side (CW)**: Comb filter resonance — multipath interference simulation with jitter modulation. Quality controls jitter depth (multipath severity).

---

### 5. Dreamy
> *placeholder text*
- **Left side (CCW)**: Smearing and soft time blur.
  Adds subtle delay feedback for movement and texture.
- **Right side (CW)**: DRIFT PLACEHOLDER TEXT

---

### 8. Delay / Locked Delay
> *Half speed is best speed.*

This mode adds a twist to Parasites’ delay:
- Inserting a cable into the **Wet/Dry CV input** engages “locked speed.”  
- Delay speed (Big knob) becomes quantized to stable ratios.  
- Sending **+2.5V** locks the loop, freezing the input.

 <sub>Note: while freezing the loop can still be manipulated through some ways. Currently this is the only way to get the tape style in a frozen loop.</sub>

---

## Patch Tips

1. All three Artifacts modes feature stereo separation. Patch one input and set the crossfader to noon for instant dual image.  
2. Feed an attenuated LFO to the **smear side** for instant vaporwave texture.  
3. Use positive/negative gates to jump into the extreme sides of the effects.  
4. The sample-rate reducer goes extremely low, perfect for feeding filters or resonators.
5. Send a attenuated LFO to the algorithm cv while in **locked delay** mode

---


## Credits

- Original firmware by **Émilie Gillet** (Mutable Instruments)  
- Warps parasites by **Matthias Puech**


---