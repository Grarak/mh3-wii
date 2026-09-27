# Optimization and gameplay follow-ups

- [ ] Profile and reduce the slow village → Moga Woods loading/transition.
  Measure exit selection → first field frame → player control, separately
  timing disc reads, decompression, RSO load/relocation/checksums, asset setup,
  texture upload and shader/pipeline creation. Compare cold and warm runs
  with Dolphin using the same post-Chief save and 4:3 presentation.
  Reproduce with `scripts/replay_village.py --name NAME --moga-woods`.
- [ ] Profile CPU/GPU frame time and pacing during movement after the collision
  fix. Identify hot decompiled functions and rewrite verified hot paths in
  readable C++ where useful; retain -O1 until measured gains justify changing it.
- [ ] Verify village building collisions later (dock boundary regression passes).
- [ ] Verify Dolphin's background savestate pipe and improve fast state replay.
