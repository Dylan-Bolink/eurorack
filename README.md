# Mutable Instruments - Marbles: Truchets

A reimagining of Mutable Instruments [**_Grids_**](https://pichenettes.github.io/mutable-instruments-documentation/modules/grids/) drum pattern generator, now living inside Marbles. Truchets is build up on  [**_Marbles 1.3_**](https://pichenettes.github.io/mutable-instruments-documentation/modules/marbles/firmware/). All other modes still function the same as in the original firmware.

## Activating Grids Mode

Long press T mode while on drum mode (Solid red) to enter Grids mode (Blinking red)

---

### Grids Mode

When Grids mode is active, the T-section transforms into a three-voice drum trigger generator based on the classic Grids topology.

---

## Outputs
| Output | Function |
|------|----------|
| **T1** | Kick trigger |
| **T2** | Hi-hat trigger |
| **T3** | Snare trigger |
| **Y** | Accent gate (active when pattern accent is high) |
| **X1/X2/X3** | Random voltages (**X2** still follows master clock in the current beta) |

---

## Density Controls

The T-section knobs control trigger density for each voice:

| Knob | Controls |
|------|----------|
| **Bias (T)** | Kick density |
| **Rate** | Hi-hat density |
| **Jitter** | Snare density |

All three respond to CV input.

---

## Map & Tempo Controls

Hold **X Mode** (N in the manual) and turn knobs to access hidden parameters:

| Hold X Mode + | Controls |
|---------------|----------|
| **Steps** | Map X |
| **Bias (X)** | Map Y |
| **Spread** | Chaos |
| **Rate** | Tempo (stored on mode entry) |

These where the old Y parameters but Y is now used as a accent output

---

## CV swap System

Truchets features a flexible CV routing system. These CV routings only apply while you are in the Grids T model (Blinking red) While holding **X Mode** (N in the manual), press buttons to cycle through CV swap states:

### Map CV Routing

| Hold X Mode + Press | LED State | CV Source |
|---------------------|-----------|-----------|
| **T Range** (Map X) | Off | No CV |
| | Green | Steps CV → Map X |
| | Blinking | Bias (T) CV → Map X |
| **X Ext** (Chaos) | Off | No CV |
| | Green | Spread CV → Chaos |
| | Blinking | Rate CV → Chaos |
| **X Range** (Map Y) | Off | No CV |
| | Green | Bias (X) CV → Map Y |
| | Blinking | Jitter CV → Map Y |

The button location makes the most sense with the original marbles module where the buttons and corresponding knobs are aligned from left to right.

---

### Deja vu and Deja vu Lock CV Swap

Locking the T side works with the grids mode. Unlocked it will play a 32 step pattern. In a locked state it will follow the deja vu length setting. Deja vu amount knob is ignored in the current revision of the firmware.

While holding **X Mode** (N in the manual) and pressing the lock buttons the deja vu cv input can be repurposed:


| Hold X Mode + Press | LED State | Function |
|---------------------|-----------|----------|
| **T Déjà Vu** | Off | Normal behavior |
| | Green | Déjà Vu CV gates T-side lock |
| **X Déjà Vu** | Off | Normal behavior |
| | Green | Déjà Vu CV gates X-side lock |

When enabled, a **gate signal (+2.5V)** on the Déjà Vu CV input flips the lock state:
- If unlocked → locks the pattern
- If locked → unlocks the pattern

Both T and X can be enabled simultaneously for synchronized lock control. If non are active the Deja vu cv input will work as before.

---

## LED Reference (while holding X Mode)

| LED | Shows |
|-----|-------|
| T Range | Map X CV swap state |
| X Ext | Chaos CV swap state |
| X Range | Map Y CV swap state |
| T Model | Explicit reset enabled |
| T Déjà Vu | T lock CV swap enabled |
| X Déjà Vu | X lock CV swap enabled |

---

## Credits

- Original Marbles & Grids firmware by **Émilie Gillet** (Mutable Instruments)
