This ist my ToDo List for things left to do:


ODF Loadin in GO:
=================
- Find out why building ranks takes so different times. Sometimes 1-2 seconds and sometimes 15-20 seconds for the same ODF. 

Release Alignment:
==================
- for T float versions a more precise aproach and bugfix in the analyze_lut.py has been applied. The changes have to be applied to GO as well
- [DONE v227] LUT release time range: für unbounded releases wird n_end aus
  latest_attack_loop_end_sample berechnet (max aller SMPL-Loop-Enden), nicht aus
  WAV-Länge oder erstem Loop-Ende. C++ (GetLatestLoopEnd()), analyze und verify
  verwenden dieselbe Logik. CSV loggt latest_loop_end und loop_count.
- [DONE / already optimized] LUTCache period range: ComputeCorrelationLut
  already restricts n_start/n_end from min/max_key_press_ms, and builds
  loop_mono only up to n_end-1.  Nothing to do.

- [DONE] Store T in .golut Cache File:
  GOLUT_FORMAT_VERSION bumped to 2. Each GOLutEntry now stores
  period_samples (uint32_t) + period_float (double) alongside its points.
  OverrideCorrLutsFromCache restores both fields so GetPositionForCorrelation
  always uses the generation-time grid, even on the skipCorrLut path.

  

- Period Block Optimisation (non-octave stops):
  Currently, every non-octave pipe (aliquot, mixture: HarmonicNumber not a
  power of 2) runs autocorrelation individually to find its true waveform
  period T. This is correct but redundant: pipes within the same rank that
  share the same smpl-key offset (a "block") will always yield the same
  correction factor.
  
  Optimisation: within each rank, group pipes by their (smpl_midi - key_midi)
  offset. For each constant-offset block, run autocorrelation on ONE probe
  pipe only (e.g. the middle pipe of the block). Derive the factor
    factor = T_autocorr / T_formula
  and apply it to all pipes in that block without re-running autocorrelation.
  
  Implementation notes:
  - Needs a rank-level pass AFTER all pipes are loaded but BEFORE
    ComputeCorrelationLut is called (currently per-pipe in Finalize).
  - Best insertion point: a new GORank::ComputeAliquotPeriods() called from
    GOOrganController loading phase after all pipes in the rank are finalised.
  - Store the result as m_corrPeriodOverride (uint32_t, 0 = not set) on
    GOSoundProvider; ComputeCorrelationLut uses it instead of recomputing.
  - Mixture stops with repetition (Sesquialtera doppelt, Mixtur) have
    MULTIPLE blocks per rank (offset changes at repetition breakpoints),
    so each block needs its own probe pipe.
  - Expected speedup: O(blocks) autocorrelations instead of O(pipes).
    For a 61-note mixture with 3 blocks: 3 autocorrs instead of 61.











==================================================