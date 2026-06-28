# 3. FOOTSWITCH_1 meaning depends on the TIME mode

Status: accepted

## Context

The user fixed the two footswitches to **tap tempo** (FS1) and **bypass** (FS2),
with the both-held DFU gesture mandatory — leaving no switch for looper transport.
But the TIME engine's anchor identity is its hands-free **looper**, which
fundamentally needs a record/play stomp.

## Decision

Make FOOTSWITCH_1's short-press meaning follow TOGGLE_2 (the TIME mode):

- **Delay / Tape-slip** — short press = tap tempo; **hold = freeze / havoc**.
- **Looper** — short press = **record → play → overdub** cycle; **hold = stop /
  clear**.

Tap tempo is meaningless for a loop, so the overload is unambiguous within each
mode. The control loop reads the mode and dispatches FS1 events accordingly,
applying looper transport to the engine inside a critical section.

## Consequences

- A real hands-free looper coexists with tap + bypass on only two footswitches.
- FS1 behaviour is context-sensitive; the player must know the current TIME mode
  (set deliberately on a toggle, so this is acceptable).
- Freeze/havoc is unavailable in Looper mode (FS1 hold is stop/clear there); the
  SPACE bloom is likewise only reachable from the delay modes.
