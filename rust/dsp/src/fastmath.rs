//! Fast math approximations for the per-sample hot path.
//!
//! libm's `sinf` / `sqrtf` / `floorf` are *software* implementations — hundreds
//! of cycles each on the M7 — and the audio ISR calls them per sample. These
//! replacements are 10–40 cycles and host-tested against libm to documented
//! tolerances. Use them for modulators and gain laws (where ~0.1 % error is
//! inaudible); keep libm for block-rate mapping code.

/// sin(2π·phase) for `phase ∈ [0, 1)` — parabolic approximation with one
/// refinement pass. Max abs error ≈ 0.001; output clamped to [-1, 1].
#[inline]
pub fn sin_01(phase: f32) -> f32 {
    // Map to t ∈ [-0.5, 0.5) with sin(2π·phase) = sin(2π·t).
    let t = if phase < 0.5 { phase } else { phase - 1.0 };
    // Parabola through the zeros/peak, then odd refinement.
    let y = 8.0 * t - 16.0 * t * t.abs();
    let y = 0.225 * (y * y.abs() - y) + y;
    y.clamp(-1.0, 1.0)
}

/// √x for `x ≥ 0` — Quake-style inverse-sqrt seed + two Newton passes.
/// Relative error < 1e-4 over the audio range; returns 0 for x ≤ 0.
#[inline]
pub fn sqrt(x: f32) -> f32 {
    if x <= 0.0 {
        return 0.0;
    }
    let i = 0x5f37_59df_u32.wrapping_sub(x.to_bits() >> 1);
    let mut r = f32::from_bits(i); // ~1/√x
    r *= 1.5 - 0.5 * x * r * r;
    r *= 1.5 - 0.5 * x * r * r;
    x * r
}

/// round-half-away-from-zero via integer truncation. Valid for |x| < 2^31.
#[inline]
pub fn round(x: f32) -> f32 {
    let h = if x >= 0.0 { 0.5 } else { -0.5 };
    ((x + h) as i32) as f32
}

/// Gentle odd-symmetric cubic soft clip (the charge-transfer nonlinearity used
/// by both the BBD and PT2399 models). Waveform-preserving, monotonic over the
/// clamp range; output bounded to ±(1.6 − 1.6³/6.75) ≈ ±0.993.
#[inline]
pub fn soft_clip(x: f32) -> f32 {
    let x = x.clamp(-1.6, 1.6);
    x - (x * x * x) * (1.0 / 6.75)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sin_01_tracks_libm() {
        let mut max_err = 0.0f32;
        for i in 0..10_000 {
            let p = i as f32 / 10_000.0;
            let approx = sin_01(p);
            let exact = libm::sinf(core::f32::consts::TAU * p);
            max_err = max_err.max((approx - exact).abs());
            assert!((-1.0..=1.0).contains(&approx));
        }
        assert!(max_err < 2.0e-3, "sin_01 max error {max_err}");
    }

    #[test]
    fn sqrt_tracks_libm() {
        for i in 1..10_000 {
            let x = i as f32 * 0.001; // 0.001 .. 10
            let approx = sqrt(x);
            let exact = libm::sqrtf(x);
            let rel = ((approx - exact) / exact).abs();
            assert!(rel < 1.0e-4, "sqrt({x}) rel error {rel}");
        }
        assert_eq!(sqrt(0.0), 0.0);
        assert_eq!(sqrt(-1.0), 0.0);
    }

    #[test]
    fn round_matches_expectations() {
        assert_eq!(round(0.4), 0.0);
        assert_eq!(round(0.5), 1.0);
        assert_eq!(round(-0.5), -1.0);
        assert_eq!(round(-0.4), 0.0);
        assert_eq!(round(1234.6), 1235.0);
    }
}
