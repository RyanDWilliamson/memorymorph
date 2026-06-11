# Echorec gain staging: analytic 1/G makeup, no compander

The DMM and SDD-555 instruments use a compander (compress → delay → expand) that
RMS-normalizes the wet path to a per-mode target, with hand-tuned makeup tables,
`kOutTrim`, and reverb-send scaling — a gain map spread across many constants.
The **real Binson Echorec had no compander** (it recorded straight to the
magnetic drum), so Echorec drops that machinery entirely and instead pins output
level structurally:

```
in → ×G (SW1 drive) → drumSaturate() → ×(1/G) → … → mix → output soft-clip ceiling
```

SW1 sets only the pre-gain `G`; the makeup is **computed as `1/G`**, not
tabulated. Because the soft-clip's small-signal slope ≈ 1, `×G … ×(1/G)` is
unity *by construction* — drive changes character, not level, with no RMS
tracking. Loud signals soft-compress (can't blow out), the saturator sits inside
the Swell feedback loop where its `tanh` self-limits runaway into a controlled
oscillating drone (authentic Gilmour swell), and a single output soft-clip
guards feedback peaks.

## Consequences

- No compander, no per-position makeup table, no `kOutTrim`, no send-scaling —
  the whole gain map is `G`, `1/G`, and one ceiling.
- "Unity even when pushing input gain" is a structural guarantee, not a tuning
  exercise.
- Swell is capped ≤ 0.95; self-oscillation is a feature, bounded by the in-loop
  saturator rather than prevented.
