# Original Konzetsu clear and unlock behavior

Reference: `libcocos2dcpp.70256.so`, SHA-256
`b40710389d63524e85007239826418c3728c05daa974428733f10953fb328421`.
Audio source: local `arcaea_7.0.256c.apk`, `assets/audio/konzetsu/`.
Addresses below are IDA virtual addresses, not APK offsets.

## Confirmed action helpers

| Address | Identity established by RTTI |
| --- | --- |
| `0x14e48c8` | `cocos2d::FadeOut::create` |
| `0x166df68` | `cocos2d::FadeIn::create` |
| `0xf8c480` | `cocos2d::EaseCubicActionOut::create` |

Thus the fades use `1 - (1 - t)^3`, rather than linear interpolation or smoothstep.
`0xd05854` constructs sequences; `0x14eb9fc` constructs callbacks.

## Entry and ordinary door completion

`0xe62ba8 -> 0x8b9450` schedules `setupIntroAnimations` after **0.6 seconds**
when a just-completed challenge ID is present. Its callback at `0x79eae8` plays
audio **94**, then invokes hub state refresh `0xdd9660` with animation enabled.

The refresh calls `0x14d0d64` (`transitionToCleared`, identified through callback
RTTI), `0x1455464` (uncleared state), `0x7fec4c` (inactive state), and centerpiece
chain update `0x104c5bc`.

For an ordinary newly cleared door, `0x14d0d64` fades its old panel/number layers
out and its clear layers in over **0.7 seconds**, using cubic ease out.
`0x104c5bc` fades the corresponding central chain out over **0.3 seconds**.
The switch covers all six chains, including door VI.

## Five doors complete and sixth door unlock

`0xdd9660` checks `0x143bcb8`: this checks completion song records for challenges
I–V, including a distinct-song check. The client currently obtains completion
from its daily-task API; this patch retains that existing eligibility model.

When the original five-door condition is true, refresh establishes the cleared
state without animation, then runs `0x7fec4c` with animation enabled on each of
I–V. That action:

1. Fades clear badge (`+944`), clear chain (`+952`), and clear ripple (`+936`)
   out over **0.3 seconds**.
2. At zero opacity, callbacks replace the textures with their inactive variants.
3. Fades the same sprites back in over **0.3 seconds**.

The complete sequence is wrapped in cubic ease out. Apply easing to its combined
0.6-second clock, then evaluate the two linear fade halves. The texture swap
therefore occurs about 0.124 seconds into wall time, when the eased clock reaches
0.3 seconds. Easing each half separately would produce a different animation.
Callback `0x8f4ea4` sets `node-clear-chain-inactive.png`; `0xb413b8` sets
`node-clear-ripple-inactive.png`; `0xdef7d0` selects the inactive badge.
The latter is incorrectly merged into a neighboring function in this IDB;
its instructions, rather than that neighboring decompilation, establish the call.

Door VI's lock transition is in `0x1455464`: it fades out over **0.3 seconds**
when the five-door condition becomes true. Its central chain remains until VI
is itself complete. VI completion establishes clear state without animation,
then runs the same **0.3 + 0.3 second** inactive texture sequence.

## Audio mapping

`0x146b264` maps audio enum values to paths:

| Enum | Pointer | Path | Role confirmed by caller |
| --- | --- | --- | --- |
| 92 | `0x1b99838` | `konzetsu/node_chain_break.wav` | Centerpiece green-node sequence |
| 93 | `0x1ba09e8` | `konzetsu/loading.wav` | Loading |
| 94 | `0x1b99a10` | `konzetsu/challenge_chain_break.wav` | Just-completed challenge callback |

Recovered original audio:

- `challenge_chain_break.wav`: 2.136 seconds, SHA-256
  `7c08496fd8407a4ec6845e025f16d6d755860db298c939c34a9df8abfaa8fbe6`.
- `node_chain_break.wav`: 9.467 seconds, SHA-256
  `2cc202739f289e68ca9dbdfd76d63284edd9acc90f2a0c80f69f5c42f976318a`.

Both files are copied unchanged, including their float WAV encoding; Godot
successfully imports them. Only enum 94's sound belongs in the clear callback.
The client uses an independent player for that sound so subsequent node clicks
cannot replace its stream before its 2.136-second tail ends.

## Separate later bind interaction — do not confuse with fifth-door unlock

`allBindsConnected` at `0xab2fdc` is reached from manual bind interactions:
`0x150d91c -> 0x162d64c`, following I -> II -> IV -> VI -> V -> III.
The last connection callback `0xacc07c` invokes `0xab2fdc`.

It waits 0.5 seconds, uses a 0.45-second bind/overlay transition, switches center
side decorations after 0.29 seconds, and enters `playOutGreenNodeAnimation`
at `0xf6bd3c`. This plays audio 92 and chains six center animation phases.
The initial phase waits 1.6 seconds, then scales/fades a glow over 0.3 seconds,
then waits 0.2 seconds. Later phases continue through `0x1171f14`, `0x1846d20`,
`0x19f0690`, `0x11dadf4`, `0x14e4b84`, and `0x18f78b4`.
`0x1171f14` is another function boundary incorrectly merged in the IDB; its
instructions establish the phase's 0.3-second scale/fade and 0.2-second wait.

That later gameplay flow is outside the clear/unlock patch. Center clear,
fragment and glow-chain assets must not be added to fifth-door unlock simply
because they are present in the resource directory.

## Verification scope

`verify_konzetsu_clear_animation.gd` exercises single completion, multiple
completion, fifth-door unlock, VI completion, and immediate reentry. It checks
intermediate fade frames, original audio selection, lock state and interaction,
all six central chains, and persistence without animation replay. It restores
the preexisting task snapshot after running.

Earlier `build/verify_konzetsu_clear_transition.gd` used only five task entries;
`set_konzetsu_tasks` requires six entries and `challenge_16_clear` at index five.
That invalid fixture prevented the expected unlock and explains its timeout.
