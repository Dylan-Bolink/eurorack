# Mutable Instruments - Marbles: Truchets

A reimagining of Mutable Instruments [**_Grids_**](https://pichenettes.github.io/mutable-instruments-documentation/modules/grids/) drum pattern generator, now living inside Marbles. Truchets is built on [**_Marbles 1.3_**](https://pichenettes.github.io/mutable-instruments-documentation/modules/marbles/firmware/), it adds a new T-side mode and new X-side features while keeping all original Marbles functionality intact.

<br>

For the latest update file go to the [**release page**](https://github.com/Dylan-Bolink/eurorack/releases).<br>
When you need instructions on how to update your marbles go to the [**original manual**](https://arc.net/l/quote/yqufnyhb).

<br>

## Contents

1. [Button Behavior](#button-behavior)
2. [Marbles Enhancements](#marbles-enhancements)
   - [X Modes](#x-modes)
   - [X Ext Modes](#x-ext-modes)
   - [Explicit Reset](#explicit-reset)
3. [Activating Grids Mode](#activating-grids-mode)
4. [Outputs](#outputs)
5. [Standard Controls](#standard-controls)
   - [Density Knobs](#density-knobs)
   - [Deja Vu (T Side)](#deja-vu-t-side)
6. [X Shift Layer - Knobs](#x-shift-layer---knobs)
   - [Groove Offset](#groove-offset-x-bias)
   - [Rate](#rate-rate)
   - [Swing](#swing-jitter)
   - [Map X & Map Y](#map-x--map-y-steps--x-bias)
   - [Chaos](#chaos-spread)
   - [Accent Control](#accent-control-deja-vu)
   - [Accent Variation](#accent-variation-length)
7. [X Shift Layer - Buttons](#x-shift-layer---buttons)
   - [Pattern Banks](#pattern-banks)
   - [CV Swap Routing](#cv-swap-routing)
   - [Deja Vu CV Swap](#deja-vu-cv-swap)
8. [Advanced Settings Layer](#advanced-settings-layer)
9. [Patching Ideas](#patching-ideas)
10. [Credits](#credits)

<br>

## Button Behavior

**T Mode** and **X Mode** buttons use tap and long-press to navigate between modes:

| Press Duration | Behavior |
|----------------|----------|
| **Short tap** (< 275ms) | Cycle within current bank |
| **Medium press** (275ms–2s) | Ignored (prevents accidental changes) |
| **Long press** (≥ 2s) | Switch between banks (no blink/blink) |

<br>

## Marbles Enhancements

These features extend the standard Marbles functionality and are available outside of Grids mode.

### X Modes

The **X Mode** button now has 5 modes split into two banks:

| Bank  | LED State | Name |
|------|-----------|------|
| Basic | Solid Green | Identical |
| Basic | Solid Orange | Bump |
| Basic | Solid Red | Tilt |
| Alternative | Blinking Green | Round Robin |
| Alternative | Blinking Orange | Envelope |

> Short tap cycles within the current bank. Long press switches between banks.

<br>

### Round Robin (Blinking green)

Cycles through X1/X2/X3 outputs one at a time. Each clock pulse advances to the next channel. When clock is not patched the clocking signal comes from T2.

> In normal green mode, all three X outputs share the same random sequence in a shift-register style — X1 gets the newest value, X2 gets the previous, and X3 the one before that. In round robin, only one output updates per clock tick while the other two hold their last value, cycling X1 → X2 → X3.

<br>

### Envelope (Blinking orange)

Generates attack-decay envelopes on X outputs triggered by t1t2t3 or clock input.

| Control | Function |
|---------|----------|
| **Spread** | Random envelope time distribution |
| **Bias** | Envelope length |
| **Steps** | Attack/decay ratio |


**X Range** changes retrigger behavior:

| LED State | Retrigger Mode |
|-----------|----------------|
| Green (narrow) | Hard reset on every trigger |
| Orange (positive) | Only retrigger after attack phase (Serge-style) |
| Red (full) | Only retrigger after envelope completes (legato) |

> With external clock, envelopes trigger round-robin style (one channel per trigger). With internal clock, all channels trigger independently.

<br>

### X Ext Modes

**X Ext** cycles through 3 modes:

| LED State | Mode | Function |
|-----------|------|----------|
| Off | Normal | Standard behavior |
| Solid Green | External | Original external input behavior |
| Blinking Green | Transpose | Spread CV input becomes V/Oct transposition |

> In **Transpose** mode, the Spread CV input applies a pitch offset to all three X outputs. The input uses a V/Oct correction curve for tracking accuracy. While the hardware is not made for it this curve tries to fix that.

<br>

### Explicit Reset

Press **T Mode** + **X Mode** to cycle through 4 states:

| State  | T Section Reset | X Section Reset |
|-------|-----------------|-----------------|
| Off | Disabled | Disabled |
| 1 | Enabled (T Jitter input) | Disabled |
| 2 | Disabled | Enabled (X Steps input) |
| 3 | Enabled | Enabled |

> After toggling, **T Deja Vu** and **X Deja Vu** LEDs flash for 1 second to indicate which sections are active.

<br>

## Activating Grids Mode

Long press **T Mode** while on drum mode (solid red) to enter Grids mode (blinking red).

> To exit, long press **T Mode** without changing anything in the advanced layer.

## Grids mode outputs

| Output | Function |
|--------|----------|
| **T1** | Kick trigger |
| **T2** | Hi-hat trigger |
| **T3** | Snare trigger |
| **Y** | Accent output |
| **X1/X2/X3** | Random voltages |

> X1/X2/X3 are following the standard Marbles behaviour. All X controls still function like the original firmware. X2 follows hihat pattern instead of master tempo source.

<br>

## Standard Controls

<img src="../images/truchets_cheatsheet_1.png" style="width:500px;">

### Density Knobs

| Knob | Controls |
|------|----------|
| **Bias (T)** | Kick density |
| **Rate** | Hi-hat density |
| **Jitter** | Snare density |

> All three respond to CV input.

### Deja Vu (T Side)

The Deja Vu section controls pattern looping:

- **T Deja Vu Button**: Tap to toggle loop lock on/off
- **Deja Vu Length**: Sets loop length (when locked)
- **Deja Vu Amount**: Controls loop behavior (see below)

#### Deja Vu Amount Behavior

| Direction | Effect |
|-----------|--------|
| **Left** | Chance to shift loop start point +1 step each cycle |
| **Noon** | Neutral |
| **Right** | Chance for density drift on steps (clears when unlocked) |

> When the knob is fully left the chance to shift is 100%.

> Density drift is inspired by the Chaos parameter. The key differences: Chaos always adds hits (fills), while Density drift can both add and remove hits. Chaos is always random (even when the loop is locked), while Density drift locks with Deja Vu. Chaos re-rolls once per loop cycle, while Density drift is applied per step.

<br>

## X Shift Layer - Knobs

**Hold X Mode** and turn knobs to access hidden parameters.

> All shift layer parameters have a neutral position at noon (12 o'clock), meaning they have no effect on the sound until you turn them.

<img src="../images/truchets_cheatsheet_2.png" style="width:500px;">

### Groove Offset (x Bias)

| Position | Effect |
|----------|--------|
| **Far left** | Kick +3 steps late |
| **Center left** | Kick +1 and +2 steps late |
| **Left** | Kick micro-timing late (up to 50%) |
| **Noon** | Neutral |
| **Right** | Snare micro-timing late (up to 50%) |
| **Center right** | Snare +1 and +2 steps late |
| **Far right** | Snare +3 steps late |

### Rate (Rate)

The same Rate/tempo control from the standard Marbles interface, moved here to free up the knob for hi-hat density.

### Swing (Jitter)

| Position | Effect |
|----------|--------|
| **Left** | Classic swing |
| **Noon** | Neutral |
| **Right** | Tresillo swing |

> Both swings go to 50% max.

### Map X & Map Y (Steps & x Bias)

Change the pattern coordinates on the current bank. 

> [Original Grids manual](https://pichenettes.github.io/mutable-instruments-documentation/modules/grids/manual/)

### Chaos (Spread)

| Position | Effect |
|----------|--------|
| **Left** | Random tempo jitter (humanize) |
| **Noon** | Neutral |
| **Right** | Density Chaos (ghost notes / fills) |

> The left side of the chaos knob uses the normal Jitter logic from the other modes.

### Accent Control (Deja vu)

| Position | Effect |
|----------|--------|
| **Far left** | Kick accent only |
| **Left** | Hi-hat accent only |
| **Center-left** | Snare accent only |
| **Noon** | All accents combined (original behaviour) |
| **Right to far right** | All accents combined, lowering threshold |

> 192 is the original grids threshold for an accent and is used on the left to noon side of the knob.

> Lowering threshold creates more gates. If accent control is fully to the right an accent will always be fired when any of the outputs are firing.

### Accent Variation (Length)

| Position | Effect |
|----------|--------|
| **Left** | Random voltage window |
| **Noon** | Neutral (5V gates) |
| **Right** | Velocity-sensitive gates (stronger accents = higher voltage) |

> Both sides start as fixed 5V gates near noon. Turning the knob further from center widens the voltage window, reaching the full 0V–5V range at the extremes.

> The random voltages are locked with the Deja vu function.

<br>

## X Shift Layer - Buttons

**Hold X Mode** and press buttons to access CV routing and bank select.

<img src="../images/truchets_cheatsheet_3.png" style="width:500px;">

### Pattern Banks

Hold **X Mode** + tap **T Mode** to cycle through pattern banks:

| LED State | Bank |
|-----------|------|
| Solid Green | OG Grids |
| Solid Orange | Electronic |
| Solid Red | Breakbeat |
| Blinking Green | OG Grids (no interpolation) |
| Blinking Orange | Electronic (no interpolation) |
| Blinking Red | Breakbeat (no interpolation) |

> **Interpolation vs No Interpolation:** With interpolation (solid LED), patterns morph smoothly between the 25 positions in the 5x5 grid as you adjust Map X/Y (Same as the original Grids). Without interpolation (blinking LED), Map X/Y snap to the nearest of the 25 grid positions — no morphing, just the raw patterns.

### CV Swap Routing

| Hold X Mode + Press | LED State | CV Source |
|---------------------|-----------|-----------|
| **T Range** | Off | No CV |
| | Green | Steps CV → Map X |
| | Blinking | Bias (T) CV → Map X |
| **X Ext** | Off | No CV |
| | Green | Spread CV → Chaos |
| | Blinking | Rate CV → Chaos |
| **X Range** | Off | No CV |
| | Green | Bias (X) CV → Map Y |
| | Blinking | Jitter CV → Map Y |

> The routing follows a left-to-right logic across the panel. The buttons (center) map to the shift layer knobs (right side): one press assigns the CV input next to that knob (green), a second press mirrors it to the corresponding CV input on the left side of the module (blinking).

### Deja Vu CV Swap

| Hold X Mode + Press | LED State | Function |
|---------------------|-----------|----------|
| **T Deja Vu** | Off | Normal behavior |
| | Green | Deja Vu CV gates T-side lock |
| **X Deja Vu** | Off | Normal behavior |
| | Green | Deja Vu CV gates X-side lock |

> When enabled, a **gate signal (+2.5V)** on the Deja Vu CV input flips the lock state.

<br>

## Advanced Settings Layer

**Hold T Mode** and press buttons to toggle advanced settings.

<img src="../images/truchets_cheatsheet_4.png" style="width:500px;">

| Hold T Mode + Press | Setting | Off (default) | On (LED lit) |
|----------------------|---------|---------------|--------------|
| **T Deja Vu** | Loop playhead | Shared playhead | Independent playhead |
| **X Deja Vu** | Loop start | Dynamic (from current step) | Always from beat 1 |
| **T Range** | Read mode | Normal | Henri |
| **X Ext** | Accent hang | Normal gates | Hanging accents |
| **X Range** | Knob swap | Normal knob locations | Knobs swapped |
| **X Mode** | Explicit reset | Off | 4 states (see [Explicit Reset](#explicit-reset)) |

### Setting Descriptions

#### Independent Loop Playhead (T Deja Vu)
When **on**, the loop has its own playhead separate from the main pattern. When you unlock, playback stays in sync with where the pattern would have been.

#### Loop from Beat 1 (X Deja Vu)
When **on**, loops always start from step 1 of the pattern instead of the step where you activated the loop.

#### Henri Mode (T Range)
Switches pattern reading between Normal (original Grids algorithm) and Henri (alternate reading from Grids4Live Max plugin). Henri mode produces different rhythmic relationships between kick, snare, and hi-hat.

#### Accent Hang (X Ext)
When **on** AND an accent variation is active, accents will sustain until the next accent fires.

> This turns accent into a sample and hold output driven by accent and the current accent variation.

#### Knob Swap (X Range)
When **on**, the Marbles knobs (**Steps**, **Bias**, **Spread**) directly control Grids coordinates (**Map X**, **Map Y**, **Chaos**). Marbles X parameters use stored alternate values. When **off** (default), Marbles knobs control Marbles X parameters and Grids coordinates comes from the shift layer.

> When toggling, current knob positions are captured to prevent parameter jumps.

> Knob swap is independent of CV swap. CV swap routes CV inputs to Grids coordinates, while knob swap changes which parameters the physical knobs control. Both can be used at the same time.

#### Explicit Reset (X Mode)
Now supports 4 independent states for T and X sections. See [Explicit Reset](#explicit-reset) for full details.

#### Gate length control (t Bias & Jitter)
While **Holding T Mode** the gate configuration parameters are still accessible. 
> [Marbles manual about gate configuration](https://arc.net/l/quote/heojrdjx)


<br>

## Patching ideas

- Grids mode can still feel really random by turning Chaos fully clockwise or counter clockwise and having a high swing value.
- Use a small amount of Jitter for a humanized feel.
- Turning on Henri mode can give you a B side of a beat. Turning it on and off can give some nice breaks.
- Self patching Marbles X side to any grid parameter can give great results. 
- Having a high spread and steps on the X side can give random gate outputs driven by the Grids gate generation. Essentially creating 7 gate outputs.
- Having a offset module patched to Map/Chaos cv in can give you direct control over these parameters without using the shift layer. Think of Stages, Shades or Blinds
- Transpose mode lets you transpose X outputs with a sequencer or keyboard CV patched to the Spread CV input. Or you can add a extra lfo on top of the randomness.

<br>

## Credits

- Original Marbles & Grids firmware by **Émilie Gillet** (Mutable Instruments)
- This very handy grids [pattern website](https://goodtohear.co.uk/tools/grids-sequencer) by **Michael Forrest**
- VCV Rack Topograph for all the A/B Testing by **Dale Johnson**
- Henri mode based on Grids4Live by **Henri David**