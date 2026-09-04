// ============================================================================
// tuner_evo.h — evolutionary tuning of the 16-grey apply trains.
//
// WHY THIS EXISTS. The grammar converger in tuner.h searches a (d, w, g, r)
// tuple: d darkens, w whitens, one float gap, r re-darkens. That is four
// integers, and it emits the same shape family whether a train has 13 slots
// or 20 — more slots make trains longer, not more expressive. Worse, when
// the gap probe measures the float gain as zero (which it does, on every
// panel tried so far) the length penalty makes any gap strictly
// negative-scoring, so the search collapses to three integers. Levels whose
// target does not land on that coarse lattice cannot be reached at all, and
// the planner just oscillates between the two nearest points — which is
// exactly what L9 did for an entire run, stuck at +18 for 30 cycles.
//
// The evidence that the shapes are out there: every time the raw-sequence
// rescue ran, it beat the planner by an order of magnitude in ONE scan —
// L8 12.2 -> 1.3, L1 6.7 -> 0.1, L10 19.5 -> 1.7 — and every winner was
// off-grammar. L10's was {1,1,1,1,1,1,1,2,2,1,1,0,2,...}: seven darkens,
// two whitens, two re-darkens, a float, then a whiten. No (d,w,g,r) tuple
// produces that.
//
// So: drop the grammar entirely. Evolve the drive sequence itself.
//
// THROUGHPUT IS THE WHOLE GAME. A scan costs ~4 s and a paint ~0.3 s, so
// the tuner is scan-bound and the only figure that matters is candidates
// measured per scan. The grammar converger manages 15 (one per level). This
// manages 60, by exploiting two things:
//
//   * a column's train is chosen by its LEVEL INDEX, so the 15 usable
//     levels are 15 independent experiment slots, and
//   * the engine only drives pixels whose value CHANGED, so a band painted
//     earlier is not re-driven when a later band is painted.
//
// Which means each horizontal band can carry a different train set. Paint
// band 0 with candidate set 0, swap the train library, paint band 1 with
// set 1, and band 0 keeps the drive it already received. Four paints, one
// scan, 60 measurements.
//
// THE CARD (960 x 540, sixteen 60 px columns — column g is level g):
//
//   rows   0..139   dithered reference, density g/15   <- the target
//   rows 140..239   band 0: the incumbent (elite)
//   rows 240..339   band 1: mutant
//   rows 340..439   band 2: mutant
//   rows 440..539   band 3: mutant
//
// Band 0 holds the ELITE rather than a fourth mutant, and that is not a
// wasted slot: it re-measures the incumbent in the same frame as its
// challengers, so selection compares like with like. Judging a challenger
// against a reference captured in an earlier frame carries the session's
// drift into every decision — the rescue in tuner.h learned that the
// expensive way, where a column-position bias of ~13 units once made a
// level "improve" by adopting its own shape unchanged.
//
// THE ALGORITHM is evolutionary programming rather than a GA: mutation
// only, no crossover, with a self-adapting mutation strength per level.
// Crossover between drive sequences is not obviously meaningful — splicing
// the first half of one train onto the second half of another breaks the
// charge balance and the temporal structure both — whereas mutation of a
// sequence maps directly onto "try a slightly different waveform".
//
// Each level is an independent (1+3) evolution strategy. Fifteen of them
// run in parallel because they occupy different columns.
// ============================================================================

#pragma once

#if TUNEUP_HAVE_JPEGDEC

static const int EVO_BANDS   = 4;               // candidates per level per scan
// How many times the staircase repair may swap inverted levels to their
// runner-up before giving up. Each attempt costs one scan pair.
static const int EVO_REPAIR_TRIES = 2;
static const int EVO_REF_H   = 140;             // dithered reference band
static const int EVO_BAND_H  = 100;             // each candidate patch
static const int EVO_FIRST_L = 1;               // levels evolved: 1..14
static const int EVO_LAST_L  = 14;
// How hard neighbour spacing is weighed against absolute placement. Banding
// is a spacing artefact, so this is not a refinement - it is the term that
// decides whether the ramp looks smooth. Applied in the SQUARED domain
// (fitness is err^2 + resid^2), hence smaller than the old linear 0.6.
static const float EVO_SPACING_W = 0.35f;
// How hard a fragile incumbent is penalised, per scan unit of measured
// spread. Position sensitivity is a property of the WAVEFORM, not only the
// panel - tuning once made a level 5x MORE position-sensitive while placing
// it perfectly - so a shape that lands correctly everywhere is worth more
// than one that lands correctly in the spot it was measured.
static const float EVO_ROBUST_W = 0.4f;
// Row charging time every evolutionary run tunes AT, set explicitly rather
// than inherited. Inheriting is a trap: installing a blob restores the row
// timing it was tuned at, so starting a fresh tune right after boot would
// silently calibrate against the OLD table's timing and bake it in. Set it,
// log it, store it in the blob.
static const int EVO_ROW_EXTRA_US = 4;

// Held LE width every run tunes AT, pinned for the same reason. 1000 ns is
// the middle of a measured plateau (900..1100 differ by ~1 lum, and past
// ~2 us transparent-latch bleed makes dithered content WORSE than no hold
// at all) - so this is a value to keep, not to raise.
static const int EVO_LE_HOLD_NS = 1000;

// Pass period HIGH tunes at, pinned like the rest of the timing. 15000 was
// too tight to hold the tables it produced: the pass allowance is period
// minus the settle floor, and full-height content needs the row loop plus
// 540 x row_extra inside it. At 15 ms that allowance is 13.5 ms and the
// content alone very nearly fills it, so every heavy frame over-ran and
// over-dosed - which is what made the match card and the staircase
// disagree by 14 lum while the match card itself read a perfect 1.5.
// 18500 was tried and REVERTED. It did fix a real fault - measured with
// EPD_GREY16_PASS_TIMING, photo content ran 16.8..17.9 ms passes against a
// wanted 15 ms, a 19% over-dose on every heavy frame - but the cure cost
// more than the disease. Black is anchored at 11 darkening pulses over a
// 255-unit range, so one pulse is worth ~23 units at 18.5 ms against
// ~18.6 at 15 ms, and level placement got visibly coarser: two independent
// runs at 18.5 ms produced staircase steps of 2..48 and -2..41 where the
// 15 ms table gives 7..27, with a repeatable ~48-unit cliff the panel
// simply cannot land inside. The over-dose is still real and still
// unfixed; the answer is to shorten the ROW LOOP so 15 ms fits honestly,
// not to stretch the period until it does.
//
// Raising it further is NOT free: a longer pass delivers more dose per
// slot, so black saturates sooner and fewer slots still respond (19 ms was
// measured with only 7 of 20 live against 15 ms's 10). That is why
// row_extra stays at 4 rather than 6 - 6 would need ~19 ms, and this
// board's dark end is already crushed. `P <us>` still overrides.
//
// 0 = INHERIT THE BOARD'S PRESET, which is the setting to leave it on. Every
// number argued out above was measured on a LilyGo T5 S3 GPS, and pinning it
// here as a constant quietly applied one board's answer to every other one.
// It is wrong on the H716 by more than a preference: that board's preset asks
// for 24 ms because its rows are slower, and forcing 15 ms puts the period
// UNDER the row loop, so every pass overruns, clamps to the settle floor, and
// dose goes content-dependent — the precise fault the constant period exists
// to remove. A tune run that way calibrates against a moving target.
//
// Inheriting costs the LilyGo nothing: no preset but the H716's sets these
// fields, so it takes Config's default, which is the same 15000 this constant
// held. `P <us>` still overrides for a deliberate experiment.
static const int EVO_PERIOD_US_HIGH = 0;

// The seed genome every level starts from: 11 darkens then 6 whitens.
// Deliberately identical for all levels — the point of the exercise is that
// evolution differentiates them, not that we hand it a good starting
// guess. 11 is the measured saturation knee, so the darken run is as deep
// as the panel usefully goes, and the whitens give the light end somewhere
// to climb back to.
//
// The knee is a property of the pass PERIOD, not of the train length, and
// both qualities now run 15 ms — so 11 is right for NORMAL's 13 slots as
// well as HIGH's 20. NORMAL simply has less room left over for whitens,
// which is exactly what having fewer slots means.
static const int EVO_SEED_DARK_MAX  = 11;
static const int EVO_SEED_LIGHT_MAX = 6;
static int evoSeedDark()  { return min(EVO_SEED_DARK_MAX, tuneLen - 2); }
static int evoSeedLight() { return min(EVO_SEED_LIGHT_MAX, tuneLen - evoSeedDark()); }
// Black anchor: the saturation knee. The dithered reference is built from
// level 15 pixels, so this must be a real black and must not evolve.
static int evoAnchorDark() { return min(EVO_SEED_DARK_MAX, tuneLen); }

// ---------------------------------------------------------------------------
// Positional randomisation.
//
// A row is driven as a unit and its pixels share a charge source, so what a
// patch actually receives can depend on WHERE it is and on what its
// neighbours are doing in the same pass. Under a fixed layout - column g
// always level g, elite always band 0 - any such effect is not noise, it is
// a systematic BIAS welded to that level and that candidate slot, and no
// number of generations averages it out. Worse, the elite occupying the
// same band every generation means the incumbent is consistently favoured
// or penalised against its own challengers.
//
// So both are shuffled every generation: which column holds which level,
// and which band holds the incumbent. The comparison stays in-frame (a
// level's reference and its candidates remain in the same column as each
// other), but the physical position rotates, which turns position from a
// bias into variance that 30 generations can average away.
// ---------------------------------------------------------------------------
//
// TOGGLEABLE, because the two settings answer different questions. With the
// layout FIXED, each level is measured at the same spot every generation, so
// the positional offset is a constant added to that level's error - the
// search converges hard and fast because it is chasing a stationary target,
// and it bakes the offset into the train. With it SHUFFLED, the offset
// becomes per-generation noise: convergence is slower and noisier, but the
// result is not tied to one position on the glass.
//
// Fixed is therefore the better instrument for asking "what can this search
// do", and shuffled the better one for producing tables meant to be right
// everywhere.
static bool evoRandomPos = false;

// ---------------------------------------------------------------------------
// Settle-slot mask.
//
// Tests whether the position-dependent variation is really about WHERE a
// patch sits, or about CHARGE DEPLETION - a row driven hard has less left to
// give, and consecutive driving passes never let the supply recover. If that
// is the mechanism, giving every drive a recovery pass should collapse the
// spread; if the spread is unchanged, depletion is not the cause.
//
// Pinning every other slot float would test it, but halves the drive budget
// and puts black out of reach: the deep end needs a run of CONSECUTIVE
// darkens to saturate. So the train is split instead -
//
//   slots 1..7    unrestricted   consecutive drive, enough to reach black
//   slots 8..25   every EVEN slot pinned float, odd slots free
//
// - which leaves 16 usable slots of 25: seven consecutive for depth, then
// nine with a settle pass between each for fine trim.
//
// In 0-based indices that is 0..6 free, then free only where the index is
// EVEN (index 7 is slot 8, which is even, so pinned).
static const int EVO_FREE_PREFIX = 7;      // slots 1..7 unrestricted
// OFF by default. It was introduced to give the supply a recovery pass
// between drives, and it did remove the vertical component (6.1 -> 0.4
// units). But row_extra_us now addresses the same charging problem directly
// and at the source, for free at 16-grey - and it does so without spending
// nine of twenty-five slots. Keeping both is redundant, and every pinned
// slot is expressive range the search cannot use.
static bool evoSettleMask = false;

static bool evoSlotUsable(int i) {
  if (!evoSettleMask) return true;
  if (i < EVO_FREE_PREFIX) return true;
  return (i & 1) == 0;
}

static uint8_t evoPerm[16];        // perm[column] = level shown there
static uint8_t evoEliteBand[16];   // which band holds the incumbent, per level

static void evoShufflePositions() {
  for (int i = 0; i < 16; i++) evoPerm[i] = (uint8_t)i;
  if (!evoRandomPos) return;                    // identity: column g = level g
  for (int i = 15; i > 0; i--) {
    const int j = (int)random(0, i + 1);
    const uint8_t t = evoPerm[i]; evoPerm[i] = evoPerm[j]; evoPerm[j] = t;
  }
}

// An individual is a PAIR: the apply that lands the grey AND the remove
// that takes it back to white (Tony's design, 28 Jul 2026). The two are
// DC-locked — the remove's net whitens always equal the apply's net
// darkens — and BOTH halves are measured every generation: the card is
// erased band-by-band with each candidate's own remove anyway (the ledger
// demands it), so one extra scan of the erased card turns that erase from
// pure overhead into half the experiment. Fitness is the round trip:
// err^2 + residual^2, smaller is better.
struct EvoLv {
  uint8_t elite[TUNE_LEN_MAX];
  uint8_t eliteRm[TUNE_LEN_MAX];    // its DC-locked erase train
  float   eliteErr;                 // signed apply error, last generation
  float   eliteResid;               // signed erase residual, last generation
  uint8_t cand[EVO_BANDS][TUNE_LEN_MAX];
  uint8_t candRm[EVO_BANDS][TUNE_LEN_MAX];
  float   sigma;                    // mutation strength (expected edit count)
  int     stale;                    // generations since the elite improved
  int     wins;                     // accepted mutations, for the 1/5th rule
  int     trials;
  // Hall of fame: the two best PAIRS this level has ever measured, scored
  // on err^2 + resid^2 alone (spacing is contextual and cannot travel).
  // Kept because the elite DRIFTS - once true differences fall below the
  // scan noise a lucky read can displace a better incumbent, and the run
  // walks away from pairs it had already found. Re-tested against the
  // incumbent at the end, in one frame, before anything is committed.
  float   bestScore, secondScore;
  uint8_t best[TUNE_LEN_MAX],   second[TUNE_LEN_MAX];
  uint8_t bestRm[TUNE_LEN_MAX], secondRm[TUNE_LEN_MAX];
  // Running spread of THIS elite's measured error, Welford, reset whenever
  // the elite changes. Because the incumbent is re-measured every
  // generation at a fresh band (and column, if shuffling), that spread IS
  // its position sensitivity - and we know two shapes can hit the same
  // target with 4x different sensitivity, so without this the search picks
  // between them by coin toss.
  float   rMean, rM2;
  int     rN;
};
// PSRAM, allocated on first use: sixteen of these are ~8 KB, and .bss
// lives in the internal RAM lwIP needs for packet buffers.
static EvoLv *ev = nullptr;
static bool evoAllocState() {
  if (!ev) ev = (EvoLv *)heap_caps_calloc(16, sizeof(EvoLv), MALLOC_CAP_SPIRAM);
  return ev != nullptr;
}

// ---------------------------------------------------------------------------
// The reference ladder — and why it must NOT be linear in ink density.
//
// evoErrors() computes err = nat - ref, so ref[] IS the target curve. The
// tune can only place a level where its reference sits, and no amount of
// convergence can separate two targets that are closer together than the
// scan's own noise.
//
// Building the references at density g/15 makes them linear in INK AREA,
// and reflectance is not linear in ink area — it compresses hard once the
// paper is mostly covered. Measured on this rig the gaps ran
//
//     5.0 7.0 7.0 6.0 8.0 9.0 9.0 10.0 7.0 10.0 6.0 5.0 8.0 3.0 1.0
//
// so the last three levels shared four units of range while the midtones
// got ten apiece. L13, L14 and L15 were being asked to land 1–3 units
// apart, inside the noise, and the search dutifully drove all three to
// black — three greys thrown away, and a staircase that reads 24, 1, 2, 0
// at the dark end however well the tune converges.
//
// So measure the density→luminance curve once and invert it: choose each
// level's dither threshold so the RESULTING luminances come out evenly
// spaced. The thresholds bunch toward the dark end, which is precisely the
// compensation the compression asks for.
// ---------------------------------------------------------------------------
// Defaults to the plain density ladder — these are exactly (g*64+7)/15 —
// so anything that draws a reference without running a tune first (the
// standalone staircase check) still gets a sane card.
static uint8_t evoRefThr[16] = { 0, 4, 9, 13, 17, 21, 26, 30,
                                 34, 38, 43, 47, 51, 55, 60, 64 };

// On by default since the M5PaperS3 A/B (29 Jul): the corrected ladder took
// the NORMAL staircase's worst gap from 41 to 31 and the confirm from 2.9 to
// 2.2 on otherwise identical runs. The premise is still shaky — inverting
// the density->luminance curve point by point assumes that curve is smooth
// in threshold, and a Bayer pattern's dot gain moves between thresholds in
// ways sixteen samples cannot capture — so corrected ladders still show odd
// adjacent gap pairs, and the switch ('F 0') stays for A/B runs.
static bool evoRefLadderFix = true;

static void evoRefThrLinear() {
  for (int g = 0; g < 16; g++) evoRefThr[g] = (uint8_t)((g * 64 + 7) / 15);
}

// `measured` must have been read with evoRefThrLinear() in force.
static void evoRefThrFromMeasured(const float measured[16]) {
  float thrLin[16], m[16];
  for (int g = 0; g < 16; g++) thrLin[g] = (float)((g * 64 + 7) / 15);

  // The ladder has to be monotone to invert. Scan noise can put a wobble in
  // it, so force it down rather than let one local inversion produce a
  // nonsense threshold for every level above it.
  m[0] = measured[0];
  for (int g = 1; g < 16; g++)
    m[g] = (measured[g] < m[g - 1] - 0.05f) ? measured[g] : m[g - 1] - 0.05f;

  const float top = m[0], bot = m[15];
  evoRefThr[0]  = 0;      // paper
  evoRefThr[15] = 64;     // full ink — the pinned anchor the tune is built on
  for (int g = 1; g < 15; g++) {
    const float target = top + (bot - top) * (g / 15.0f);
    int k = 0;
    while (k < 14 && m[k + 1] > target) k++;
    const float span = m[k] - m[k + 1];
    const float frac = (span > 1e-3f) ? (m[k] - target) / span : 0.0f;
    const float t = thrLin[k] + frac * (thrLin[k + 1] - thrLin[k]);
    int ti = (int)lroundf(t);
    if (ti < 0)  ti = 0;
    if (ti > 64) ti = 64;
    evoRefThr[g] = (uint8_t)ti;
  }
  // Strictly increasing, or two levels would share a density and become the
  // same target — the very failure this exists to remove.
  for (int g = 1; g < 15; g++)
    if (evoRefThr[g] <= evoRefThr[g - 1]) evoRefThr[g] = evoRefThr[g - 1] + 1;
  if (evoRefThr[14] >= 64) evoRefThr[14] = 63;
}

// ---------------------------------------------------------------------------
// Card
// ---------------------------------------------------------------------------

// Fill the canvas for a paint that has bands 0..upto populated. Bands after
// `upto` stay white so they are not driven yet; bands before it keep their
// values so the delta engine leaves them alone and their earlier drive
// stands.
static void evoFillCanvas(int upto) {
  uint8_t *fb = epd.getBuffer();
  const int W = epd.width(), H = epd.height();
  for (int y = 0; y < H; y++) {
    uint8_t *row = fb + (size_t)y * W;
    if (y < EVO_REF_H) {
      // Reference: Bayer dither of density g/15 built ONLY from level 0 and
      // level 15 pixels, so it is optical truth for level g independent of
      // any train being evolved. This is why level 15 is pinned rather than
      // evolved — the reference is made of it.
      for (int x = 0; x < W; x++) {
        const int g = evoPerm[(x * 16) / W];
        const int thr = evoRefThr[g];
        row[x] = (tuneBayer8[y & 7][x & 7] < thr) ? 15 : 0;
      }
      continue;
    }
    const int band = (y - EVO_REF_H) / EVO_BAND_H;
    if (band > upto) { memset(row, 0, W); continue; }
    for (int x = 0; x < W; x++) row[x] = evoPerm[(x * 16) / W];
  }
}

// Load one band's candidate set into the train library. Level 15 keeps the
// pinned anchor and level 0 is never driven.
static void evoUploadBand(int band) {
  for (int g = EVO_FIRST_L; g <= EVO_LAST_L; g++)
    epd.driver().setDecisionTrain((uint8_t)(g << 1), ev[g].cand[band]);
}

// Which column currently shows a given level - the inverse of the shuffle,
// needed to read the card back.
static int evoColOf(int level) {
  for (int c = 0; c < 16; c++) if (evoPerm[c] == level) return c;
  return level;
}

static void evoPinAnchor() {
  uint8_t t[TUNE_LEN_MAX];
  memset(t, 0, sizeof(t));
  const int n = evoAnchorDark();
  { int p = 0, placed = 0;
    while (placed < n && p < tuneLen) {
      if (evoSlotUsable(p)) { t[p] = 1; placed++; }
      p++;
    } }
  epd.driver().setDecisionTrain((uint8_t)(15 << 1), t);
  // Mirror it into the shared tuner state so the charge-matched removes and
  // the reports downstream see the same train the glass did.
  tl[15] = {};
  tl[15].hasRaw = true;
  memcpy(tl[15].raw, t, TUNE_LEN_MAX);
}

// Upload the erase trains PAIRED with the applies a given band was painted
// with. Each band carries a different candidate set, so each needs its own
// removes — and those removes are the candidates' own evolved halves, not
// a formula derivation: the erase that follows is how they get measured.
// The DC lock (evoBalanceRemove) guarantees each pair's ledger is exact.
static void evoUploadRemovesFor(int band) {
  uint8_t rm[TUNE_LEN_MAX];
  for (int g = EVO_FIRST_L; g <= EVO_LAST_L; g++)
    epd.driver().setDecisionTrain((uint8_t)((g << 1) | 1), ev[g].candRm[band]);
  tuneRemoveOf(15, rm);                    // the pinned anchor keeps its formula
  epd.driver().setDecisionTrain((uint8_t)((15 << 1) | 1), rm);
}

// Take the card back off the glass IN REVERSE, each band erased with the
// erase matched to the apply that put it there.
//
// This is not tidiness, it is the charge ledger. A hard clear is DC
// balanced within itself, but it delivers a FIXED charge regardless of what
// the pixel was holding - so it does not undo what the card just applied.
// Painting a card is net darkening; clearing it is not the inverse of that.
// Repeat a few hundred times and the accumulated bias is real, and it drifts
// the panel's response mid-session - which is precisely the drift that has
// been polluting every measurement here.
//
// Reverse order because that is the order that keeps each band's pixels
// under a single unambiguous decision: band b goes white while bands 0..b-1
// still hold their levels, so only band b is driven, with only band b's
// removes loaded.
static void evoEraseCard() {
  for (int b = EVO_BANDS - 1; b >= 0; b--) {
    evoUploadRemovesFor(b);
    evoFillCanvas(b - 1);      // whites band b; earlier bands untouched
    tunePaint();
  }
  // Finally the dithered reference, which is made of level 15 pixels and so
  // carries the anchor's charge.
  memset(epd.getBuffer(), 0, (size_t)epd.width() * epd.height());
  tunePaint();
}

// Paint the whole card: reference plus all four bands, each band driven
// under its own train set. Four paints, but only the band that changed is
// driven each time, so each band really does get its own waveform.
static void evoPaintCard() {
  evoPinAnchor();
  tuneClear();                       // activation flash, then straight to drive
  for (int b = 0; b < EVO_BANDS; b++) {
    evoUploadBand(b);
    evoFillCanvas(b);
    tunePaint();
  }
}

static void evoRepaintCard() { evoPaintCard(); }

// ---------------------------------------------------------------------------
// Measurement
// ---------------------------------------------------------------------------

// ref[g] from the dither band, nat[b][g] from each candidate band.
static bool evoMeasure(float ref[16], float nat[EVO_BANDS][16]) {
  tuneRepaint = evoRepaintCard;
  if (!tuneScan(tgeo.x0mm, tgeo.y0mm, tgeo.wmm, tgeo.hmm)) return false;
  for (int g = 0; g < 16; g++) {
    int px0, px1, sx0, sx1, sy0, sy1;
    tuneColSpan(evoColOf(g), &px0, &px1);
    tunePanelRect(px0, px1, 12, EVO_REF_H - 12, &sx0, &sx1, &sy0, &sy1);
    ref[g] = tuneRectMean(sx0, sx1, sy0, sy1);
    for (int b = 0; b < EVO_BANDS; b++) {
      const int y0 = EVO_REF_H + b * EVO_BAND_H + 12;
      const int y1 = EVO_REF_H + (b + 1) * EVO_BAND_H - 12;
      tunePanelRect(px0, px1, y0, y1, &sx0, &sx1, &sy0, &sy1);
      nat[b][g] = tuneRectMean(sx0, sx1, sy0, sy1);
    }
  }
  return true;
}

// Error of each candidate against its level's own reference, with the
// vertical gradient removed PER BAND. Columns 0 and 15 carry the same drive
// in every band (white, and the pinned anchor), so the difference they show
// against the reference band is pure geometry — interpolate it across the
// levels and subtract.
static void evoErrors(const float ref[16], const float nat[EVO_BANDS][16],
                      float err[EVO_BANDS][16]) {
  for (int b = 0; b < EVO_BANDS; b++) {
    const float g0 = nat[b][0] - ref[0], g15 = nat[b][15] - ref[15];
    for (int g = 0; g < 16; g++)
      err[b][g] = (nat[b][g] - ref[g]) - (g0 + (g15 - g0) * (g / 15.0f));
  }
}

// ---------------------------------------------------------------------------
// Variation
// ---------------------------------------------------------------------------

// Mutate a drive sequence. Four operators: substitute, insert (shifting
// right), delete (shifting left), and SWAP of two adjacent pulses. The
// first three are what the rescue used, and they are what reach shapes the
// grammar cannot express: interior floats, alternating tails, a whiten
// after a re-darken.
//
// SWAP is the fine-trim operator and it is qualitatively different from the
// other three: it changes only the ORDER of two pulses, so the train's net
// charge is EXACTLY unchanged and its charge-matched erase does not move
// either. Yet the optical effect is real, because these pulses are not
// commutative - a whiten fires far harder from a dark state than a light
// one, and a darken after deep whitening rides the fresh-response boost. So
// {1,2} and {2,1} deliver the same charge to different effect, which is
// precisely the small, DC-safe step needed once the remaining errors are a
// unit or two and substitute/insert/delete all overshoot.
//
// It therefore gets likelier as sigma falls: a converged level trims order,
// a stuck one rewrites content.
static void evoMutate(const uint8_t *src, uint8_t *dst, float sigma) {
  memcpy(dst, src, TUNE_LEN_MAX);
  const int last = tuneLen - 1;
  int edits = (int)sigma;
  if ((float)random(0, 1000) / 1000.0f < (sigma - (float)edits)) edits++;
  if (edits < 1) edits = 1;
  const int swapPct = constrain((int)(70.0f / max(1.0f, sigma)), 15, 70);

  if (evoSettleMask) {
    // Insert and delete shift the sequence, which would slide drives into
    // pinned slots - so this mode mutates by substitution and by swapping
    // two usable slots, both of which preserve the mask.
    int usable[TUNE_LEN_MAX], nu = 0;
    for (int i = 0; i < tuneLen; i++) if (evoSlotUsable(i)) usable[nu++] = i;
    for (int e = 0; e < edits && nu > 0; e++) {
      if ((int)random(0, 100) < swapPct && nu >= 2) {
        const int k = (int)random(0, nu - 1);
        const int a = usable[k], b2 = usable[k + 1];
        const uint8_t t = dst[a]; dst[a] = dst[b2]; dst[b2] = t;
      } else {
        dst[usable[(int)random(0, nu)]] = (uint8_t)random(0, 3);
      }
    }
    for (int i = 0; i < tuneLen; i++) if (!evoSlotUsable(i)) dst[i] = 0;
    for (int i = tuneLen; i < TUNE_LEN_MAX; i++) dst[i] = 0;
    return;
  }

  for (int e = 0; e < edits; e++) {
    int op = ((int)random(0, 100) < swapPct) ? 3 : (int)random(0, 3);
    if (op == 3) {
      // Find an adjacent pair that actually differs - swapping two equal
      // codes is a no-op and would waste one of the four band slots.
      const int start = (int)random(0, last > 0 ? last : 1);
      int p = -1;
      for (int k = 0; k < last; k++) {
        const int i = (start + k) % last;
        if (dst[i] != dst[i + 1]) { p = i; break; }
      }
      if (p < 0) { op = 0; }        // uniform train: nothing to reorder
      else {
        const uint8_t t = dst[p];
        dst[p] = dst[p + 1];
        dst[p + 1] = t;
        continue;
      }
    }
    const int pos = (int)random(0, tuneLen);
    switch (op) {
      case 0:
        dst[pos] = (uint8_t)random(0, 3);
        break;
      case 1:
        for (int i = last; i > pos; i--) dst[i] = dst[i - 1];
        dst[pos] = (uint8_t)random(0, 3);
        break;
      default:
        for (int i = pos; i < last; i++) dst[i] = dst[i + 1];
        dst[last] = 0;
        break;
    }
  }
  for (int i = tuneLen; i < TUNE_LEN_MAX; i++) dst[i] = 0;
}

static bool evoAllFloat(const uint8_t *t) {
  for (int i = 0; i < tuneLen; i++) if (t[i]) return false;
  return true;
}

// Net darkening of a train: darkens minus whitens.
static int evoNetOf(const uint8_t *t) {
  int net = 0;
  for (int i = 0; i < tuneLen; i++) {
    if (t[i] == 1) net++;
    else if (t[i] == 2) net--;
  }
  return net;
}

// A candidate must be NET DARKENING to be usable.
//
// The search does not otherwise care about charge, and with the settle mask
// it found shapes that were net-NEGATIVE while still landing genuinely dark
// - seven consecutive darkens into saturation, then a tail spent mostly
// whitening, which lifts far less than those darkens deepened. Optically
// fine, arithmetically nonsense, and unerasable: tuneRemoveOf() reads
// net <= 0 as "nothing to erase" and emits a charge-neutral scrub, which
// left L9 ghosting at -15.8 units with three widening rounds unable to
// touch it. Any honest apply drawn from white is net-darkening, so requiring
// it costs the search nothing real and closes that hole.
static bool evoUsableCandidate(const uint8_t *t) {
  return !evoAllFloat(t) && evoNetOf(t) > 0;
}

// Net of a train counted over an explicit length (the 4-level tier runs
// 7 or 13 slots, not tuneLen).
static int evoNetOfLen(const uint8_t *t, int len) {
  int net = 0;
  for (int i = 0; i < len; i++) {
    if (t[i] == 1) net++;
    else if (t[i] == 2) net--;
  }
  return net;
}

// DC-NEUTRAL PAIR MUTATION (Tony's operator set, 28 Jul 2026). The genome
// is the apply and its remove taken TOGETHER — 2*len slots — and every
// operator preserves the combined net of zero by construction, so no
// candidate pair can ever carry a DC imbalance and no repair step exists
// to blur a mutation after the fact:
//
//   swap    exchange the contents of any two slots, same waveform or
//           across the two — a pure reorder of the combined pulse
//           multiset. Covers fine order-trims AND migrating drive mass
//           between the paint and the erase (a darken leaving the apply
//           as a whiten leaves the remove).
//   retire  pick one darken and one whiten anywhere in the pair and set
//           both to float — the pair shrinks by one charge-neutral quantum.
//   add     pick two float slots anywhere and make one a darken, one a
//           whiten — the pair grows by one charge-neutral quantum.
//
// Legacy code-3 pulses (old hand-built 4-level tables) are moved by swap
// but never created or retired — their charge is outside the ledger model.
static void evoMutatePair(uint8_t *ap, uint8_t *rm, float sigma, int len) {
  int edits = (int)sigma;
  if ((float)random(0, 1000) / 1000.0f < (sigma - (float)edits)) edits++;
  if (edits < 1) edits = 1;
  const int N = 2 * len;
  auto slot = [&](int k) -> uint8_t & { return (k < len) ? ap[k] : rm[k - len]; };
  for (int e = 0; e < edits; e++) {
    const int op = (int)random(0, 4);      // 0,1 swap; 2 retire; 3 add
    if (op <= 1) {
      const int i = (int)random(0, N), j = (int)random(0, N);
      const uint8_t t = slot(i); slot(i) = slot(j); slot(j) = t;
    } else if (op == 2) {
      int d = -1, w = -1;
      const int s = (int)random(0, N);
      for (int k = 0; k < N && (d < 0 || w < 0); k++) {
        const int p = (s + k) % N;
        if (d < 0 && slot(p) == 1) d = p;
        else if (w < 0 && slot(p) == 2) w = p;
      }
      if (d >= 0 && w >= 0) { slot(d) = 0; slot(w) = 0; }
    } else {
      int a = -1, b = -1;
      const int s = (int)random(0, N);
      for (int k = 0; k < N && b < 0; k++) {
        const int p = (s + k) % N;
        if (slot(p) == 0) { if (a < 0) a = p; else b = p; }
      }
      if (a >= 0 && b >= 0) {
        if ((int)random(0, 2)) { slot(a) = 1; slot(b) = 2; }
        else                   { slot(a) = 2; slot(b) = 1; }
      }
    }
  }
  for (int i = len; i < TUNE_LEN_MAX; i++) { ap[i] = 0; rm[i] = 0; }
}

// The round-trip fitness, smaller is better:
//
//     (1 + |grey error|)^2  +  (1 + |erase residual|)^evoGhostPow
//
// The ghost term says a surviving ghost is worse than a slightly-off grey:
// the grey is a rendering nuance the dither absorbs, the ghost haunts
// every frame after it. The +1 floors keep both terms alive near zero so
// neither half of the pair ever stops mattering.
//
// THE EXPONENT IS A KNOB. It defaults to 3 — CUBED — because that is the
// product decision: a ghost is worse than a slightly-off grey, and the
// dither absorbs grey error that no amount of rendering can hide a ghost
// behind (Tony, 29 and 30 Jul). At 4 units each the ghost costs 125
// against the grey's 25, and the preference grows with magnitude.
//
// What the cube costs, so a future reader does not rediscover it the hard
// way: the cheapest way for a level to be erasable is to barely move ink,
// so the cube biases the search toward thin applies.
//
//   - At 4 LEVELS that was fatal, and is not used there: the H716's first
//     from-scratch FAST run (30 Jul) collapsed L1 and L2 onto white,
//     curve 255 255 253 0. The 4-level tier therefore has its own
//     square-weighted evo4PairFitness — see tuner_evo4.h.
//   - At 16 levels it shows up at the LIGHT END, where placing a level
//     less than one darken below white needs a darken-then-whiten
//     combination — MORE pulses, which the cube reads as harder to erase.
//     Compare the H716's L1 apply: 28 Jul chose {1,1,2,2}, net zero,
//     landing 13 lum under white; 30 Jul cubed chose a lone darken,
//     landing 39 under, with L2 collapsed onto it.
//
// Neither of those is what produced the 30 Jul staircase inversions,
// though — that was card-vs-staircase context drift (up to 17 lum at L7),
// measured and printed by the verdict. Fitness weighting and context
// drift are independent problems; do not blame one for the other.
//
// Set to 2.0 ('Z 2' over serial) to A/B the square against a run that
// changes nothing else.
static float evoGhostPow = 3.0f;

static float evoPairFitness(float err, float resid) {
  const float ge = 1.0f + fabsf(err);
  const float gh = 1.0f + fabsf(resid);
  return ge * ge + powf(gh, evoGhostPow);
}

// SEED-TIME ONLY: bring an inherited remove into exact balance with its
// apply before the first generation. Every mutation thereafter is
// DC-neutral by construction (evoMutatePair), so this never runs again.
static void evoBalanceRemove(const uint8_t *ap, uint8_t *rm, int len) {
  const int need = -evoNetOfLen(ap, len);
  int have = evoNetOfLen(rm, len);
  for (int guard = 0; have != need && guard < 400; guard++) {
    const int i = (int)random(0, len);
    if (have > need) {                     // shed darkening
      if (rm[i] == 1)      { rm[i] = 0; have--; }
      else if (rm[i] == 0) { rm[i] = 2; have--; }
    } else {                               // add darkening
      if (rm[i] == 2)      { rm[i] = 0; have++; }
      else if (rm[i] == 0) { rm[i] = 1; have++; }
    }
  }
  // Random repair exhausted (train nearly saturated): finish linearly.
  for (int i = 0; have != need && i < len; i++) {
    while (have > need && rm[i] != 2) { rm[i] = (rm[i] == 1) ? 0 : 2; have--; }
    while (have < need && rm[i] != 1) { rm[i] = (rm[i] == 2) ? 0 : 1; have++; }
  }
}

// Build the next generation for one level: band 0 keeps the elite pair,
// bands 1..3 are fresh mutant pairs, each produced by the DC-neutral pair
// operators so its ledger is exact without any repair. A mutant whose
// apply went non-darkening is re-rolled, as is a duplicate pair —
// measuring the same sequences twice wastes a quarter of the generation's
// information.
static void evoBreed(int g) {
  EvoLv &L = ev[g];
  memcpy(L.cand[0],   L.elite,   TUNE_LEN_MAX);
  memcpy(L.candRm[0], L.eliteRm, TUNE_LEN_MAX);
  for (int b = 1; b < EVO_BANDS; b++) {
    for (int tries = 0; tries < 40; tries++) {
      memcpy(L.cand[b],   L.elite,   TUNE_LEN_MAX);
      memcpy(L.candRm[b], L.eliteRm, TUNE_LEN_MAX);
      evoMutatePair(L.cand[b], L.candRm[b], L.sigma, tuneLen);
      if (!evoUsableCandidate(L.cand[b])) continue;
      bool dup = false;
      for (int j = 0; j < b && !dup; j++)
        if (!memcmp(L.cand[j], L.cand[b], tuneLen) &&
            !memcmp(L.candRm[j], L.candRm[b], tuneLen)) dup = true;
      if (!dup) break;
    }
  }
  // Deal the four candidates into the four bands at random, remembering
  // where the incumbent landed. Without this the incumbent is in band 0
  // every generation, so any vertical position effect becomes a standing
  // advantage or handicap in every selection the run ever makes.
  // Bands are ALWAYS dealt at random, even when the columns are fixed.
  // These are four different vertical positions on the glass, so leaving the
  // incumbent parked in band 0 every generation gives it a standing
  // advantage or handicap against its own challengers - and it also means
  // the incumbent never samples more than one position, which is what the
  // robustness estimate below needs.
  int home = 0;
  {
    for (int i = EVO_BANDS - 1; i > 0; i--) {
      const int j = (int)random(0, i + 1);
      for (int p = 0; p < TUNE_LEN_MAX; p++) {
        uint8_t t = L.cand[i][p];   L.cand[i][p]   = L.cand[j][p];   L.cand[j][p]   = t;
        t = L.candRm[i][p];         L.candRm[i][p] = L.candRm[j][p]; L.candRm[j][p] = t;
      }
      if (home == i)      home = j;
      else if (home == j) home = i;
    }
  }
  evoEliteBand[g] = (uint8_t)home;
}

// ---------------------------------------------------------------------------
// Independent verification: the full-height staircase.
//
// The tune measures on the match card, where every row is half dithered
// reference (two levels) and half native (one level per column). A real
// image is not like that, and it matters: a row's conversion cost scales
// with how many distinct decisions it carries, the pass loop pads to a
// constant period, and if a row loop ever overruns that period the dose
// changes. This exact failure has been seen before - a ladder strictly
// monotonic on the match card showed a tie and an inversion on a
// full-height staircase.
//
// So before committing anything, paint the levels as solid full-height
// bars - the worst case for sweeps per row - and measure them against a
// full-height dithered reference painted the same way. Two scans, and it
// answers the only question that actually matters: do these tables render
// correctly on content that is not the tuning card?
//
// The reference is immune to the dose question by construction: it is built
// from level 0 and level 15 pixels only, so its optical value is the area
// average of paper and ink, not a function of any train being verified.
// ---------------------------------------------------------------------------
static void evoPaintBars(bool dithered) {
  uint8_t *fb = epd.getBuffer();
  const int W = epd.width(), H = epd.height();
  for (int y = 0; y < H; y++) {
    uint8_t *row = fb + (size_t)y * W;
    for (int x = 0; x < W; x++) {
      const int g = (x * 16) / W;
      if (dithered) {
        const int thr = evoRefThr[g];
        row[x] = (tuneBayer8[y & 7][x & 7] < thr) ? 15 : 0;
      } else {
        row[x] = (uint8_t)g;
      }
    }
  }
}
static void evoRepaintBarsRef()  { evoPaintBars(true);  tuneClear(); tunePaint(); }
static void evoRepaintBarsNat()  { evoPaintBars(false); tuneClear(); tunePaint(); }

// Unpaint whatever is on the glass with the removes matched to the trains
// currently in tl[], so a verification card is as charge-neutral as a
// tuning one.
static void evoUnpaintAll() {
  tuneUploadRemoves();
  tuneFill(0);
  tunePaint();
}

// Fill tl[] from the apply trains actually installed in the engine.
//
// The standalone checks (staircase 'S', uniformity 'U') run without a tune,
// so tl[] holds nothing — and evoUnpaintAll() derives its erase trains from
// tl[]. Deriving them from an empty state read every net charge as zero and
// uploaded charge-neutral scrubs OVER the installed removes: the check's
// own erases ghosted, and the engine kept the junk removes until the next
// setGreyLevels(). Adopting the installed applies keeps the removes matched
// to what is really driving the glass.
static bool tuneAdoptInstalled() {
  for (int g = 1; g <= 15; g++) {
    const uint8_t *t = epd.driver().decisionTrain((uint8_t)(g << 1));
    if (!t) return false;
    tl[g] = {};
    tl[g].hasRaw = true;
    memcpy(tl[g].raw, t, tuneLen);
    // The installed removes too, verbatim: they may be evolved pairs, and
    // re-deriving them by formula here would erase with something other
    // than what selection chose.
    const uint8_t *r = epd.driver().decisionTrain((uint8_t)((g << 1) | 1));
    if (r) {
      tl[g].hasRawRm = true;
      memcpy(tl[g].rawRm, r, tuneLen);
    }
  }
  memset(removeExtra, 0, sizeof(removeExtra));
  return true;
}

static bool evoMeasureBars(bool dithered, float out[16]) {
  evoPaintBars(dithered);
  tuneClear();
  tunePaint();
  tuneRepaint = dithered ? evoRepaintBarsRef : evoRepaintBarsNat;
  if (!tuneScan(tgeo.x0mm, tgeo.y0mm, tgeo.wmm, tgeo.hmm)) return false;
  const int H = epd.height();
  for (int g = 0; g < 16; g++) {
    int px0, px1, sx0, sx1, sy0, sy1;
    tuneColSpan(g, &px0, &px1);
    tunePanelRect(px0, px1, (int)(H * 0.06f), (int)(H * 0.94f), &sx0, &sx1, &sy0, &sy1);
    out[g] = tuneRectMean(sx0, sx1, sy0, sy1);
  }
  evoUnpaintAll();
  return true;
}

// Returns the worst placement error in scan units, or -1 if a scan failed.
// scOut, if given, receives the staircase-measured level curve — the curve
// of the levels as FULL-HEIGHT SOLID content renders them, which is the
// better model of a photograph than the match card's half-dither rows.
static float evoVerifyStaircase(const uint8_t lum[16], uint8_t *scOut = nullptr,
                                uint16_t *invMask = nullptr) {
  Serial.println("[evo] ---- verification: full-height staircase ----");
  static float ref[16], nat[16];
  if (!evoMeasureBars(true,  ref)) return -1;
  if (!evoMeasureBars(false, nat)) return -1;

  const float g0 = nat[0] - ref[0], g15 = nat[15] - ref[15];
  float err[16], meas[16];
  for (int g = 0; g < 16; g++) {
    err[g]  = (nat[g] - ref[g]) - (g0 + (g15 - g0) * (g / 15.0f));
    meas[g] = ref[g] + err[g];
  }

  Serial.print("[evo] staircase err:");
  for (int g = 0; g < 16; g++) Serial.printf(" %+4.1f", err[g]);
  Serial.println();

  // Normalised curve, so it can be compared directly with what the blob
  // stores and with the match-card curve.
  const float white = meas[0], black = meas[15];
  const float span = (white - black) > 1.0f ? (white - black) : 1.0f;
  uint8_t sc[16];
  for (int g = 0; g < 16; g++)
    sc[g] = (uint8_t)constrain((int)lroundf(255.0f * (meas[g] - black) / span), 0, 255);

  if (scOut) memcpy(scOut, sc, 16);
  Serial.print("[evo] staircase curve:");
  for (int g = 0; g < 16; g++) Serial.printf(" %3d", sc[g]);
  Serial.println();
  Serial.print("[evo] match-card curve:");
  for (int g = 0; g < 16; g++) Serial.printf(" %3d", lum[g]);
  Serial.println();

  int worstDelta = 0, worstAt = 0;
  for (int g = 0; g < 16; g++) {
    const int d = abs((int)sc[g] - (int)lum[g]);
    if (d > worstDelta) { worstDelta = d; worstAt = g; }
  }

  // Steps and monotonicity, which is what banding actually is.
  int inversions = 0, minStep = 999, maxStep = -999;
  if (invMask) *invMask = 0;
  Serial.print("[evo] staircase steps:");
  for (int g = 0; g < 15; g++) {
    const int st = (int)sc[g] - (int)sc[g + 1];
    Serial.printf(" %d", st);
    // A non-decreasing step means level g+1 failed to be darker than g.
    // Flag BOTH, since either one landing wrong produces the same reversal
    // and the repair pass has no way to know which is at fault.
    if (st <= 0) { inversions++; if (invMask) *invMask |= (1u << g) | (1u << (g + 1)); }
    minStep = min(minStep, st);
    maxStep = max(maxStep, st);
  }
  Serial.println();

  float worst = 0;
  for (int g = 1; g <= 14; g++) worst = max(worst, fabsf(err[g]));
  Serial.printf("[evo] VERDICT worst %.1f units | steps %d..%d (ideal 17) | "
                "%d inversion(s) | vs match card: max %d lum at L%d\n",
                worst, minStep, maxStep, inversions, worstDelta, worstAt);
  if (worstDelta > 20)
    Serial.println("[evo] WARNING: staircase disagrees with the match card - "
                   "dose is content-dependent, tables are card-specific");
  if (inversions)
    Serial.println("[evo] WARNING: staircase is NOT monotonic - this will band");
  return worst;
}

// ---------------------------------------------------------------------------
// Uniformity probe: how much does WHERE a patch is drawn change what it
// lands at?
//
// A row is driven as a unit from a shared charge source, so a patch's dose
// may depend on its position and on what its neighbours are doing. If that
// is true it sets a hard FLOOR on this panel: no waveform can be correct
// everywhere at once, and searching below that spread is chasing geometry.
//
// Two cards, same level, 64 patches each:
//
//   A  every patch DRIVEN to level L      spread = optics + geometry + drive
//   B  every patch a DITHER of density    spread = optics + geometry only
//      L/15 (black and white pixels, so
//      no waveform is involved at all)
//
// A - B per patch cancels the illumination falloff, the lens, the panel's
// own gradient and the scanner's geometry, and leaves the positional
// variation of the DRIVE. That difference is the number that matters.
// ---------------------------------------------------------------------------
static void evoPaintUniform(int level, bool dithered) {
  uint8_t *fb = epd.getBuffer();
  const int W = epd.width(), H = epd.height();
  const int thr = (level * 64 + 7) / 15;
  for (int y = 0; y < H; y++) {
    uint8_t *row = fb + (size_t)y * W;
    for (int x = 0; x < W; x++)
      row[x] = dithered ? ((tuneBayer8[y & 7][x & 7] < thr) ? 15 : 0)
                        : (uint8_t)level;
  }
}

static bool evoReadPatches(float out[EVO_BANDS][16]) {
  if (!tuneScan(tgeo.x0mm, tgeo.y0mm, tgeo.wmm, tgeo.hmm)) return false;
  for (int c = 0; c < 16; c++) {
    int px0, px1, sx0, sx1, sy0, sy1;
    tuneColSpan(c, &px0, &px1);
    for (int b = 0; b < EVO_BANDS; b++) {
      const int y0 = EVO_REF_H + b * EVO_BAND_H + 12;
      const int y1 = EVO_REF_H + (b + 1) * EVO_BAND_H - 12;
      tunePanelRect(px0, px1, y0, y1, &sx0, &sx1, &sy0, &sy1);
      out[b][c] = tuneRectMean(sx0, sx1, sy0, sy1);
    }
  }
  return true;
}

static bool runUniformityProbe(EPD_Painter::Quality q, const int *levels, int nlv) {
  if (!scPort) { Serial.println("[evo] no scanner selected"); return false; }
  epd.setQuality(q);
  tuneLen = epd.driver().g16TrainLen();
  // Honour the 'P' period override here too, so the probe can compare
  // "extra time inside each row's charging window" (row_extra_us) against
  // "the same extra time idling at the end of the pass" (a longer period).
  int &periodCfg = (q == EPD_Painter::Quality::QUALITY_HIGH)
                       ? epd.driver()._config.g16_pass_us_high
                       : epd.driver()._config.g16_pass_us_normal;
  if (tunePeriodUs) periodCfg = (int)tunePeriodUs;
  if (!epd.driver().setGreyLevels(16)) return false;
  if (!tuneAdoptInstalled()) {
    Serial.println("[evo] no installed trains to derive erases from");
    return false;
  }
  if (!tgeo.valid && !tuneRegister()) return false;
  Serial.println("[evo] ===== UNIFORMITY PROBE =====");
  Serial.printf("[evo] period %d us, row_extra %d us\n",
                periodCfg, epd.driver()._config.rowExtraUs());
  Serial.println("[evo] driven vs dithered, same density, 64 positions each");

  static float drv[EVO_BANDS][16], dit[EVO_BANDS][16];
  for (int i = 0; i < nlv; i++) {
    const int L = levels[i];
    evoPaintUniform(L, false); tuneClear(); tunePaint();
    if (!evoReadPatches(drv)) return false;
    evoUnpaintAll();
    evoPaintUniform(L, true);  tuneClear(); tunePaint();
    if (!evoReadPatches(dit)) return false;
    evoUnpaintAll();

    // Difference per patch, then its spread. The MEAN of the difference is
    // just this level's placement error and is not interesting here; the
    // SPREAD is the positional variation we came for.
    static float d[EVO_BANDS][16];
    float sum = 0, lo = 1e9f, hi = -1e9f;
    for (int b = 0; b < EVO_BANDS; b++)
      for (int c = 0; c < 16; c++) {
        d[b][c] = drv[b][c] - dit[b][c];
        sum += d[b][c];
        lo = min(lo, d[b][c]);
        hi = max(hi, d[b][c]);
      }
    const float mean = sum / (EVO_BANDS * 16);
    float ss = 0;
    for (int b = 0; b < EVO_BANDS; b++)
      for (int c = 0; c < 16; c++) ss += (d[b][c] - mean) * (d[b][c] - mean);
    const float sd = sqrtf(ss / (EVO_BANDS * 16));

    // Split it: how much is left-to-right, how much is top-to-bottom.
    float colM[16] = {0}, bandM[EVO_BANDS] = {0};
    for (int b = 0; b < EVO_BANDS; b++)
      for (int c = 0; c < 16; c++) { colM[c] += d[b][c] / EVO_BANDS; bandM[b] += d[b][c] / 16; }
    float cLo = 1e9f, cHi = -1e9f, bLo = 1e9f, bHi = -1e9f;
    for (int c = 0; c < 16; c++) { cLo = min(cLo, colM[c]); cHi = max(cHi, colM[c]); }
    for (int b = 0; b < EVO_BANDS; b++) { bLo = min(bLo, bandM[b]); bHi = max(bHi, bandM[b]); }

    Serial.printf("[evo] L%-2d  sd %.2f  range %.1f (%.1f..%.1f)  "
                  "horizontal %.1f  vertical %.1f\n",
                  L, sd, hi - lo, lo, hi, cHi - cLo, bHi - bLo);
    Serial.print("[evo]      per column:");
    for (int c = 0; c < 16; c++) Serial.printf(" %+.1f", colM[c] - mean);
    Serial.println();
    Serial.print("[evo]      per band:  ");
    for (int b = 0; b < EVO_BANDS; b++) Serial.printf(" %+.1f", bandM[b] - mean);
    Serial.println();
  }
  Serial.println("[evo] sd is the FLOOR: no waveform can beat the spread of "
                 "its own position");
  return true;
}

// ---------------------------------------------------------------------------
// Stability probe: how much does a grey MOVE with context and timing?
//
// A perfectly placed table is worth little if the same level renders
// differently depending on where it sits and what surrounds it. This probe
// therefore optimises for the OPPOSITE of the tuner: not "is this level
// correct" but "does this level hold still".
//
// The card carries three background bands - black, mid grey, white - and in
// each band, three test levels at three well-separated x positions. Every
// cell is a native patch with a Bayer dither of the SAME density directly
// beside it, so lamp falloff, lens vignetting and platen geometry cancel in
// the difference and what remains is the panel.
//
//   band 0  (rows   0..179)  black  background   level 15
//   band 1  (rows 180..359)  grey   background   level 8
//   band 2  (rows 360..539)  white  background   level 0
//
//   within a band, nine cells across: level cycles L4, L8, L12, so each
//   level appears at three separated x positions.
//
// The score is the spread (max-min, and sd) of (native - reference) for a
// given level across all nine of its samples. Smaller is steadier. The
// absolute value is deliberately ignored - that is the tuner's job.
// ---------------------------------------------------------------------------
static const int STAB_LEVELS[3] = { 4, 8, 12 };
static const int STAB_BG[3]     = { 15, 8, 0 };     // black, grey, white
static const int STAB_BANDH     = 180;
static const int STAB_CELLW     = 104;              // 50 native + 50 dither + gap
static const int STAB_X0        = 20;

static void stabPaintCard() {
  uint8_t *fb = epd.getBuffer();
  const int W = epd.width(), H = epd.height();
  for (int y = 0; y < H; y++) {
    const int band = min(2, y / STAB_BANDH);
    memset(fb + (size_t)y * W, (uint8_t)STAB_BG[band], W);
  }
  for (int band = 0; band < 3; band++) {
    const int cy = band * STAB_BANDH + (STAB_BANDH - 100) / 2;
    for (int c = 0; c < 9; c++) {
      const int lvl = STAB_LEVELS[c % 3];
      const int cx  = STAB_X0 + c * STAB_CELLW;
      const int thr = (lvl * 64 + 7) / 15;
      for (int y = cy; y < cy + 100; y++) {
        uint8_t *row = fb + (size_t)y * W;
        for (int x = cx; x < cx + 50; x++) row[x] = (uint8_t)lvl;          // native
        for (int x = cx + 52; x < cx + 102; x++)                            // reference
          row[x] = (tuneBayer8[y & 7][x & 7] < thr) ? 15 : 0;
      }
    }
  }
}

static void stabRepaint() { stabPaintCard(); tuneClear(); tunePaint(); }

// delta[level index][sample] = native - reference, illumination cancelled.
static bool stabMeasure(float delta[3][9]) {
  stabPaintCard();
  tuneClear();
  tunePaint();
  tuneRepaint = stabRepaint;
  if (!tuneScan(tgeo.x0mm, tgeo.y0mm, tgeo.wmm, tgeo.hmm)) return false;
  int n[3] = { 0, 0, 0 };
  for (int band = 0; band < 3; band++) {
    const int cy = band * STAB_BANDH + (STAB_BANDH - 100) / 2;
    for (int c = 0; c < 9; c++) {
      const int li = c % 3;
      const int cx = STAB_X0 + c * STAB_CELLW;
      int sx0, sx1, sy0, sy1;
      tunePanelRect(cx + 8, cx + 42, cy + 10, cy + 90, &sx0, &sx1, &sy0, &sy1);
      const float nat = tuneRectMean(sx0, sx1, sy0, sy1);
      tunePanelRect(cx + 60, cx + 94, cy + 10, cy + 90, &sx0, &sx1, &sy0, &sy1);
      const float ref = tuneRectMean(sx0, sx1, sy0, sy1);
      delta[li][n[li]++] = nat - ref;
    }
  }
  return true;
}

// Returns the mean per-level spread (max-min) — the figure being minimised.
static float stabScore(const float delta[3][9], bool verbose) {
  float total = 0;
  for (int li = 0; li < 3; li++) {
    float lo = 1e9f, hi = -1e9f, sum = 0;
    for (int i = 0; i < 9; i++) {
      lo = min(lo, delta[li][i]);
      hi = max(hi, delta[li][i]);
      sum += delta[li][i];
    }
    const float mean = sum / 9;
    float ss = 0;
    for (int i = 0; i < 9; i++) ss += (delta[li][i] - mean) * (delta[li][i] - mean);
    const float sd = sqrtf(ss / 9);
    total += (hi - lo);
    if (verbose) {
      Serial.printf("[stab]   L%-2d spread %5.1f  sd %4.2f  |", STAB_LEVELS[li], hi - lo, sd);
      for (int i = 0; i < 9; i++) Serial.printf("%+6.1f", delta[li][i]);
      Serial.println();
    }
  }
  return total / 3.0f;
}

static bool runStabilitySweep() {
  if (!scPort) { Serial.println("[stab] no scanner selected"); return false; }
  epd.setQuality(EPD_Painter::Quality::QUALITY_HIGH);
  tuneLen = epd.driver().g16TrainLen();
  if (!epd.driver().setGreyLevels(16)) return false;
  if (!tgeo.valid && !tuneRegister()) return false;

  static const int ROWEXTRA[] = { 0, 2, 4, 8 };
  const int NR = sizeof(ROWEXTRA) / sizeof(ROWEXTRA[0]);
  // Periods swept around THIS board's own presets, the same rule the dose
  // ladder ('D') uses. The old fixed 15/17/19 ms list was the LilyGo's
  // answer: on a board whose preset asks for 20 and 24 ms every entry sat
  // under the row loop, so the sweep measured the clamp, not the panel.
  const int pn = epd.driver()._config.g16_pass_us_normal;
  const int ph = epd.driver()._config.g16_pass_us_high;
  int PERIOD[3];
  int NP = 0;
  PERIOD[NP++] = ph;
  if (pn != ph) PERIOD[NP++] = pn;
  const int lower = min(pn, ph) - 2000;
  if (lower >= 10000) PERIOD[NP++] = lower;

  Serial.println("[stab] ===== STABILITY SWEEP (HIGH) =====");
  Serial.println("[stab] 3 levels x 3 positions x 3 backgrounds, native minus");
  Serial.println("[stab] an adjacent dither of the same density (optics cancel)");
  Serial.println("[stab] score = mean per-level spread; SMALLER IS STEADIER");

  float bestScore = 1e9f;
  int   bestRow = 0, bestPer = 0;
  static float delta[3][9];

  for (int r = 0; r < NR; r++) {
    for (int q = 0; q < NP; q++) {
      if (tuneAbortRequested()) { Serial.println("[stab] aborted"); return false; }
      epd.driver()._config.row_extra_us_high = ROWEXTRA[r];
      epd.driver()._config.g16_pass_us_high  = PERIOD[q];
      Serial.printf("\n[stab] --- row charge %d us, period %d us "
                    "(frame %.2f s) ---\n",
                    ROWEXTRA[r], PERIOD[q], PERIOD[q] * tuneLen / 1e6f);
      if (!stabMeasure(delta)) return false;
      const float sc = stabScore(delta, true);
      Serial.printf("[stab] SCORE %.2f  (row %d us, period %d us)%s\n",
                    sc, ROWEXTRA[r], PERIOD[q],
                    sc < bestScore ? "   <-- best so far" : "");
      if (sc < bestScore) { bestScore = sc; bestRow = ROWEXTRA[r]; bestPer = PERIOD[q]; }
    }
  }

  Serial.printf("\n[stab] ===== MOST STABLE: row charge %d us, period %d us, "
                "score %.2f =====\n", bestRow, bestPer, bestScore);
  epd.driver()._config.row_extra_us_high = bestRow;
  epd.driver()._config.g16_pass_us_high  = bestPer;
  Serial.println("[stab] (applied; re-tune HIGH at these timings to use them)");
  return true;
}

// ---------------------------------------------------------------------------
// How long does it cost to measure a frame's darkness before painting it?
//
// The compensation scheme needs a number describing the frame BEFORE the
// passes start, so whatever produces it lands directly on frame latency.
// This times the obvious candidates against each other.
//
// Row stride matters more than pixel stride: PSRAM fetches whole cache
// lines, so skipping pixels within a row still pays for the line, while
// skipping whole rows genuinely divides the traffic.
// ---------------------------------------------------------------------------
static void benchLumaScan() {
  const uint8_t *fb = epd.getBuffer();
  const int W = epd.width(), H = epd.height();
  Serial.printf("[bench] framebuffer %d x %d = %d bytes in PSRAM\n",
                W, H, W * H);

  for (int stride = 1; stride <= 16; stride *= 2) {
    // Three passes and take the best: the first walk warms the cache and
    // any single one can be interrupted by the tick.
    int64_t best = INT64_MAX;
    uint64_t sum = 0;
    for (int rep = 0; rep < 3; rep++) {
      const int64_t t0 = esp_timer_get_time();
      uint64_t s = 0;
      for (int y = 0; y < H; y += stride) {
        const uint8_t *row = fb + (size_t)y * W;
        for (int x = 0; x < W; x++) s += row[x];
      }
      const int64_t dt = esp_timer_get_time() - t0;
      if (dt < best) best = dt;
      sum = s;
    }
    const int rows = (H + stride - 1) / stride;
    Serial.printf("[bench] every %2d row(s): %6lld us  (%d rows, %d KB, "
                  "mean %.2f)\n",
                  stride, (long long)best, rows, rows * W / 1024,
                  (double)sum / (double)((size_t)rows * W));
  }
  Serial.println("[bench] NOTE: mean luminosity is the wrong predictor anyway -");
  Serial.println("[bench] demand is the DELTA against what is already on the");
  Serial.println("[bench] panel, which discovery already counts for free.");
}

// Does the LE hold actually hold?
//
// This exists to protect against a FALSE NULL. The widths that matter for a
// latch are a few hundred nanoseconds, which vanish inside a pass that is
// padded to a constant period - so if epd_hold_ns() silently did nothing,
// the LE sweep would return a flat line and read as "LE width is not the
// mechanism", which is the wrong conclusion drawn from a broken knob.
//
// So ask for an absurd width instead: 540 rows x 20 us is 10.8 ms per pass,
// which overruns the 15 ms period and therefore MUST show up as frame time.
// If the two numbers below are equal, the knob is dead and the sweep means
// nothing.
static void benchLeHold() {
  const int saved = epd.driver()._config.le_hold_ns;
  epd.setQuality(EPD_Painter::Quality::QUALITY_HIGH);
  if (!epd.driver().setGreyLevels(16)) { Serial.println("[bench] no 16-grey"); return; }
  static const int WID[] = { 0, 20000 };
  for (int i = 0; i < 2; i++) {
    epd.driver()._config.le_hold_ns = WID[i];
    tuneFill(8);
    epd.paint();
    while (!epd.driver().paintIdle()) delay(2);
    const uint32_t t0 = millis();
    tuneFill(4);
    epd.paint();
    while (!epd.driver().paintIdle()) delay(2);
    Serial.printf("[bench] LE hold %5d ns -> frame %lu ms\n",
                  WID[i], (unsigned long)(millis() - t0));
  }
  epd.driver()._config.le_hold_ns = saved;
  Serial.println("[bench] equal frames = the hold is a no-op, ignore any LE sweep");
}

// ---------------------------------------------------------------------------
// Compensation probe: can the pass period cancel a frame-darkness shift?
//
// The stability probe showed a level moves by a couple of lum depending on
// what surrounds it. This asks the follow-on question: can that shift be
// TUNED OUT at render time, using the one global knob a frame has - the
// pass period?
//
// Method. A strip of test cells sits at ONE FIXED y, and the whole of the
// rest of the panel is filled with a background that changes between runs.
// Every run starts from a hard-cleared (white) panel and the engine only
// drives changed pixels, so a black background makes the frame drive some
// half-million pixels and a white one almost none. The background is
// therefore standing in for the ink demand of a dark photograph.
//
// The cells never move, so position drops out of the comparison entirely -
// anything that changes between runs is frame demand and nothing else.
// That is exactly the confound the stability card's horizontal bands could
// not break, since there each background WAS a fixed band of the screen.
//
// At each background the period is swept in 1 ms steps, giving the slope
// d(lum)/d(ms). Dividing the background-induced shift by that slope gives
// the compensation directly, in milliseconds - which is the number the
// renderer needs in order to hold a patch still as content darkens.
//
// The dither reference is printed raw alongside each reading. It is built
// from saturated black and white pixels and so should barely move with
// period; if it does, the difference measurement is not clean and the
// slope below is contaminated.
// ---------------------------------------------------------------------------
static const int COMP_LEVELS[3] = { 4, 8, 12 };
static const int COMP_BG[3]     = { 0, 8, 15 };                    // white, grey, black
static const int COMP_ROWUS[5]  = { 0, 2, 4, 6, 8 };
// LE width, ns. The first sweep ran 0..1000 and the dithered reference was
// still falling at the top of it, so this range is extended to find where
// the latch actually completes - the equivalent of row charge's 6 us knee.
// 4 us costs 540 x 4 us = 2.16 ms per pass, which the padding still absorbs
// on match-card content.
// Wide enough to show the whole shape: the latch completing on the way up
// and transparent-latch bleed taking over past ~2 us. A fine sweep of
// 800..1200 was run and found only a 1.16 lum spread with the minimum at
// 1100 - i.e. 900..1100 is a plateau, not a peak, so 1000 is chosen as the
// middle of it rather than the measured optimum of one panel.
static const int COMP_LENS[5]   = { 0, 500, 1000, 2000, 4000 };
static const int COMP_NOMINAL   = 2;    // index of the in-use value

// LE width the ROW sweep is pinned at, so it measures row charge ON TOP OF
// a properly held latch rather than on top of whatever the loaded blob was
// tuned with. Older blobs carry le_hold_ns = 0, so inheriting it would
// quietly re-run the sweep we have already done.
static const int COMP_LE_FIXED  = 1000;

// Which timing control the sweep varies. All are swept the same way and
// judged on the same property: a knob is only usable as a global
// compensation if every level responds to it in the SAME DIRECTION.
//
// COMP_KNOB_LE additionally forces row charge to ZERO for the whole sweep.
// The two are alternative explanations of the same observation - a marginal
// latch and an incomplete charge both make busy lines render lighter - so
// leaving row charge at its tuned value would let it mask exactly the
// effect the LE sweep exists to find.
enum CompKnob { COMP_KNOB_PERIOD, COMP_KNOB_ROW, COMP_KNOB_LE };
static const int COMP_CELLW     = 104;
static const int COMP_X0        = 20;
static const int COMP_Y0        = 220;
static const int COMP_CELLH     = 100;

static int compBgLevel = 0;

static void compPaintCard() {
  uint8_t *fb = epd.getBuffer();
  const int W = epd.width(), H = epd.height();
  memset(fb, (uint8_t)compBgLevel, (size_t)W * H);
  for (int c = 0; c < 9; c++) {
    const int lvl = COMP_LEVELS[c % 3];
    const int cx  = COMP_X0 + c * COMP_CELLW;
    const int thr = (lvl * 64 + 7) / 15;
    for (int y = COMP_Y0; y < COMP_Y0 + COMP_CELLH; y++) {
      uint8_t *row = fb + (size_t)y * W;
      for (int x = cx; x < cx + 50; x++) row[x] = (uint8_t)lvl;      // native
      for (int x = cx + 52; x < cx + 102; x++)                        // reference
        row[x] = (tuneBayer8[y & 7][x & 7] < thr) ? 15 : 0;
    }
  }
}

static void compRepaint() { compPaintCard(); tuneClear(); tunePaint(); }

// nat[] and ref[] are averaged over the three x positions of each level.
static bool compMeasure(float nat[3], float ref[3]) {
  compPaintCard();
  tuneClear();
  tunePaint();
  tuneRepaint = compRepaint;
  if (!tuneScan(tgeo.x0mm, tgeo.y0mm, tgeo.wmm, tgeo.hmm)) return false;
  float ns[3] = { 0, 0, 0 }, rs[3] = { 0, 0, 0 };
  for (int c = 0; c < 9; c++) {
    const int li = c % 3;
    const int cx = COMP_X0 + c * COMP_CELLW;
    int sx0, sx1, sy0, sy1;
    tunePanelRect(cx + 8, cx + 42, COMP_Y0 + 10, COMP_Y0 + 90, &sx0, &sx1, &sy0, &sy1);
    ns[li] += tuneRectMean(sx0, sx1, sy0, sy1);
    tunePanelRect(cx + 60, cx + 94, COMP_Y0 + 10, COMP_Y0 + 90, &sx0, &sx1, &sy0, &sy1);
    rs[li] += tuneRectMean(sx0, sx1, sy0, sy1);
  }
  for (int li = 0; li < 3; li++) { nat[li] = ns[li] / 3.0f; ref[li] = rs[li] / 3.0f; }
  return true;
}

static const char *compBgName(int lv) {
  return lv == 0 ? "white" : (lv == 15 ? "black" : "grey ");
}

static bool runCompSweep(CompKnob knob) {
  if (!scPort) { Serial.println("[comp] no scanner selected"); return false; }
  epd.setQuality(EPD_Painter::Quality::QUALITY_HIGH);
  tuneLen = epd.driver().g16TrainLen();
  if (!epd.driver().setGreyLevels(16)) return false;
  if (!tgeo.valid && !tuneRegister()) return false;

  const int NB = 3, NP = 5;
  const int savedPeriod = epd.driver()._config.g16_pass_us_high;
  const int savedRow    = epd.driver()._config.row_extra_us_high;
  const int savedLe     = epd.driver()._config.le_hold_ns;

  // Period candidates centred on this board's own in-use value (which sits
  // at index COMP_NOMINAL), in 1 ms steps. The old fixed 13..17 ms list was
  // the LilyGo's range, and sat under other boards' row loops entirely.
  int compPeriod[5];
  for (int q = 0; q < NP; q++)
    compPeriod[q] = savedPeriod + (q - COMP_NOMINAL) * 1000;

  Serial.println("[comp] ===== COMPENSATION SWEEP (HIGH) =====");
  Serial.println("[comp] cells fixed in place, whole-panel background varied:");
  Serial.println("[comp] position cannot confound this, only frame ink demand");
  const char *knobName = (knob == COMP_KNOB_PERIOD) ? "pass period"
                       : (knob == COMP_KNOB_ROW)    ? "row charge"
                                                    : "LE width";
  const char *knobUnit = (knob == COMP_KNOB_PERIOD) ? "ms"
                       : (knob == COMP_KNOB_ROW)    ? "us"
                                                    : "ns";
  if (knob == COMP_KNOB_LE)
    Serial.println("[comp] row charge forced to 0 for this sweep - it would "
                   "otherwise mask the very effect being measured");
  if (knob == COMP_KNOB_ROW)
    Serial.printf("[comp] LE width pinned at %d ns for this sweep\n", COMP_LE_FIXED);
  Serial.printf("[comp] knob: %s | %d backgrounds x %d values = %d scans\n",
                knobName, NB, NP, NB * NP);

  // ABSOLUTE native luminance is what a renderer has to hold still, so that
  // is what gets analysed. The dither reference is recorded alongside but
  // NOT subtracted: it is built from level 0 and level 15 pixels, and those
  // are driven levels too - measured moving nearly 1 lum/ms - so a
  // difference measurement quietly removes part of the effect being looked
  // for. It is kept only as a check on the optics.
  static float nat[3][5][3];                    // [bg][value][level] absolute
  static float refv[3][5][3];

  for (int b = 0; b < NB; b++) {
    for (int q = 0; q < NP; q++) {
      if (tuneAbortRequested()) {
        Serial.println("[comp] aborted");
        epd.driver()._config.g16_pass_us_high  = savedPeriod;
        epd.driver()._config.row_extra_us_high = savedRow;
        epd.driver()._config.le_hold_ns        = savedLe;
        return false;
      }
      compBgLevel = COMP_BG[b];
      epd.driver()._config.g16_pass_us_high  = savedPeriod;
      epd.driver()._config.row_extra_us_high = savedRow;
      epd.driver()._config.le_hold_ns        = savedLe;
      if (knob == COMP_KNOB_PERIOD) {
        epd.driver()._config.g16_pass_us_high  = compPeriod[q];
      } else if (knob == COMP_KNOB_ROW) {
        epd.driver()._config.row_extra_us_high = COMP_ROWUS[q];
        epd.driver()._config.le_hold_ns        = COMP_LE_FIXED;
      } else {
        epd.driver()._config.row_extra_us_high = 0;
        epd.driver()._config.le_hold_ns        = COMP_LENS[q];
      }
      float n[3], r[3];
      if (!compMeasure(n, r)) {
        epd.driver()._config.g16_pass_us_high  = savedPeriod;
        epd.driver()._config.row_extra_us_high = savedRow;
        epd.driver()._config.le_hold_ns        = savedLe;
        return false;
      }
      Serial.printf("[comp] bg %s %s %4d %s |", compBgName(COMP_BG[b]), knobName,
                    knob == COMP_KNOB_PERIOD ? compPeriod[q] / 1000
                  : knob == COMP_KNOB_ROW    ? COMP_ROWUS[q]
                                             : COMP_LENS[q],
                    knobUnit);
      for (int li = 0; li < 3; li++) {
        nat[b][q][li]  = n[li];
        refv[b][q][li] = r[li];
        Serial.printf("  L%-2d %6.1f (ref %5.1f)", COMP_LEVELS[li], n[li], r[li]);
      }
      Serial.println();
    }
  }

  float xs[5];
  for (int q = 0; q < NP; q++)
    xs[q] = knob == COMP_KNOB_PERIOD ? compPeriod[q] / 1000.0f
          : knob == COMP_KNOB_ROW    ? (float)COMP_ROWUS[q]
                                     : (float)COMP_LENS[q];


  Serial.println("");
  Serial.printf("[comp] ----- response: lum per %s of %s -----\n", knobUnit, knobName);
  float slope[3];
  int nPos = 0, nNeg = 0;
  for (int li = 0; li < 3; li++) {
    float mx = 0, my = 0;
    int n = 0;
    for (int b = 0; b < NB; b++)
      for (int q = 0; q < NP; q++) { mx += xs[q]; my += nat[b][q][li]; n++; }
    mx /= n; my /= n;
    float sxy = 0, sxx = 0;
    for (int b = 0; b < NB; b++)
      for (int q = 0; q < NP; q++) {
        const float x = xs[q] - mx;
        sxy += x * (nat[b][q][li] - my);
        sxx += x * x;
      }
    slope[li] = sxx > 0 ? sxy / sxx : 0;
    // Significance has to be judged against the knob's own range, not a
    // fixed lum-per-unit figure: a real -3.15 lum/us of LE is -0.00315
    // lum/ns, which a fixed threshold reads as "flat" and reports as too
    // weak to matter. Compare the movement across the WHOLE sweep instead.
    const float travel = slope[li] * (xs[NP - 1] - xs[0]);
    if (travel > 1.0f) nPos++;
    else if (travel < -1.0f) nNeg++;
    float rlo = 1e9f, rhi = -1e9f;
    for (int b = 0; b < NB; b++)
      for (int q = 0; q < NP; q++) {
        rlo = min(rlo, refv[b][q][li]);
        rhi = max(rhi, refv[b][q][li]);
      }
    // Reported over the full sweep as well, because per-unit slope is
    // unreadable when the unit is nanoseconds.
    Serial.printf("[comp]   L%-2d  %+8.4f lum/%s  = %+6.2f lum across the sweep"
                  "   (dither ref itself moved %.1f)\n",
                  COMP_LEVELS[li], slope[li], knobUnit,
                  slope[li] * (xs[NP - 1] - xs[0]), rhi - rlo);
  }

  Serial.println("");
  Serial.printf("[comp] ----- background shift at the in-use value, and the %s "
                "that cancels it -----\n", knobUnit);
  for (int li = 0; li < 3; li++) {
    const float wht = nat[0][COMP_NOMINAL][li];
    const float gry = nat[1][COMP_NOMINAL][li];
    const float blk = nat[2][COMP_NOMINAL][li];
    Serial.printf("[comp]   L%-2d  white %6.1f  grey %6.1f  black %6.1f  |  "
                  "black-white %+5.1f", COMP_LEVELS[li], wht, gry, blk, blk - wht);
    if (fabsf(slope[li]) > 0.02f)
      Serial.printf("  ->  %+7.2f %s\n", -(blk - wht) / slope[li], knobUnit);
    else
      Serial.printf("  ->  cannot compensate (response is flat)\n");
  }

  // The verdict this whole probe exists to deliver.
  Serial.println("");
  if (nPos && nNeg)
    Serial.printf("[comp] VERDICT: %s is NOT usable as a global compensation - "
                  "%d level(s) get lighter and %d get darker, so no single "
                  "value holds the curve still.\n", knobName, nPos, nNeg);
  else if (nPos + nNeg < 3)
    Serial.printf("[comp] VERDICT: %s moves some levels less than 1 lum across "
                  "its whole range - too weak to compensate them.\n", knobName);
  else
    Serial.printf("[comp] VERDICT: %s is monotone across all three levels - "
                  "usable as a global compensation. Compare the required "
                  "values above: the closer they agree, the better one "
                  "setting serves the whole curve.\n", knobName);

  epd.driver()._config.g16_pass_us_high  = savedPeriod;
  epd.driver()._config.row_extra_us_high = savedRow;
  epd.driver()._config.le_hold_ns        = savedLe;
  Serial.printf("[comp] restored: period %d us, row charge %d us, LE %d ns\n",
                savedPeriod, savedRow, savedLe);
  return true;
}

// Re-measure the level curve on the STAIRCASE and store it as the blob's
// level_lum, replacing the match-card curve the tune saved.
//
// The curve exists for image dithering, and the two content types
// disagree: the match card's rows are half dither, half native (cheap
// rows, calibration conditions), while a photograph is full-height mixed
// content — staircase-like — whose heavier row loops shift every level.
// Measured on this board the two curves differ by up to 7 lum, a whole
// level step at the mid-tones, and diffusing error against a curve that
// wrong is exactly what photo banding looks like. Two scans per quality
// against a ten-minute retune.
static bool runStaircaseCurveStore(EPD_Painter::Quality q) {
  if (!scPort) { Serial.println("[stair] no scanner selected"); return false; }
  const Set s = (q == EPD_Painter::Quality::QUALITY_HIGH) ? SET_HIGH : SET_NORMAL;
  if (!tunedPresent(epd.driver(), s)) {
    Serial.printf("[stair] no stored %s blob to update - tune first\n",
                  tunedSetName(s));
    return false;
  }
  epd.setQuality(q);
  tuneLen = epd.driver().g16TrainLen();
  if (!epd.driver().setGreyLevels(16)) return false;
  if (!tuneAdoptInstalled()) {
    Serial.println("[stair] no installed trains to derive erases from");
    return false;
  }
  if (!tgeo.valid && !tuneRegister()) return false;

  // The stored curve, for the side-by-side the verdict prints.
  const uint8_t *cur = epd.driver()._config.levelLum(16);
  uint8_t curLum[16];
  if (cur) memcpy(curLum, cur, 16);
  else for (int i = 0; i < 16; i++) curLum[i] = (uint8_t)(255 - i * 255 / 15);

  Serial.printf("[stair] measuring the %s level curve on the staircase...\n",
                tunedSetName(s));
  uint8_t stairLum[16];
  if (evoVerifyStaircase(curLum, stairLum) < 0) return false;
  if (!tunedStoreCurve(epd.driver(), s, stairLum)) {
    Serial.println("[stair] could not update the stored blob");
    return false;
  }
  Serial.printf("[stair] %s blob now carries the staircase curve "
                "(trains and timing untouched)\n", tunedSetName(s));
  return true;
}

// ---------------------------------------------------------------------------
// Photo-context level curve (Tony's method, 28 Jul 2026).
//
// Paint the SPLASH ARTWORK as the background, overlay one small native
// patch per level at randomised positions, scan, and repeat with fresh
// positions. Four rounds average position sensitivity and lamp falloff
// into noise — and the surrounding content is exactly what the curve
// exists to serve: a photograph's ink demand, not a test card's. The
// staircase curve was a step closer to photo content than the match
// card; this is the photo itself. The result replaces the stored blob's
// level_lum, trains untouched.
//
// Layout: a 4x4 grid of cells, the 16 levels dealt into the cells in a
// fresh shuffle each round and jittered inside them — non-overlapping by
// construction, every level sampling four different regions of the
// panel and four different local neighbourhoods of the artwork.
// ---------------------------------------------------------------------------
static const int PHOTO_ROUNDS = 4;
static const int PHOTO_PATCH  = 84;      // px square; interior measured

static uint8_t photoLv[16];              // cell -> level this round
static int     photoPx[16], photoPy[16];

static void photoLayout() {
  const int W = epd.width(), H = epd.height();
  const int cw = W / 4, ch = H / 4;
  for (int i = 0; i < 16; i++) photoLv[i] = (uint8_t)i;
  for (int i = 15; i > 0; i--) {
    const int j = (int)random(0, i + 1);
    const uint8_t t = photoLv[i]; photoLv[i] = photoLv[j]; photoLv[j] = t;
  }
  for (int c = 0; c < 16; c++) {
    photoPx[c] = (c % 4) * cw + (int)random(8, cw - PHOTO_PATCH - 8);
    photoPy[c] = (c / 4) * ch + (int)random(8, ch - PHOTO_PATCH - 8);
  }
}

static void photoPaintCard() {
  uint8_t *fb = epd.getBuffer();
  const int W = epd.width();
  tuneupSplashDraw(fb, W, epd.height());
  for (int c = 0; c < 16; c++)
    for (int y = 0; y < PHOTO_PATCH; y++)
      memset(fb + (size_t)(photoPy[c] + y) * W + photoPx[c],
             photoLv[c], PHOTO_PATCH);
}

static void photoRepaint() { photoPaintCard(); tuneClear(); tunePaint(); }

static bool runPhotoCurveStore(EPD_Painter::Quality q) {
  if (!scPort) { Serial.println("[photo] no scanner selected"); return false; }
  const Set s = (q == EPD_Painter::Quality::QUALITY_HIGH) ? SET_HIGH : SET_NORMAL;
  if (!tunedPresent(epd.driver(), s)) {
    Serial.printf("[photo] no stored %s blob to update - tune first\n",
                  tunedSetName(s));
    return false;
  }
  epd.setQuality(q);
  tuneLen = epd.driver().g16TrainLen();
  if (!epd.driver().setGreyLevels(16)) return false;
  if (!tuneAdoptInstalled()) {
    Serial.println("[photo] no installed trains to derive erases from");
    return false;
  }
  if (!tgeo.valid && !tuneRegister()) return false;

  Serial.printf("[photo] %s: %d rounds of 16 patches over the splash "
                "artwork...\n", tunedSetName(s), PHOTO_ROUNDS);
  randomSeed(micros());
  float sum[16] = {0};
  int   nread[16] = {0};
  for (int round = 0; round < PHOTO_ROUNDS; round++) {
    photoLayout();
    photoPaintCard();
    tuneClear();
    tunePaint();
    tuneRepaint = photoRepaint;
    if (!tuneScan(tgeo.x0mm, tgeo.y0mm, tgeo.wmm, tgeo.hmm)) return false;
    Serial.printf("[photo] round %d:", round);
    const int m = 14;                    // stay well inside the patch
    for (int c = 0; c < 16; c++) {
      int sx0, sx1, sy0, sy1;
      tunePanelRect(photoPx[c] + m, photoPx[c] + PHOTO_PATCH - m,
                    photoPy[c] + m, photoPy[c] + PHOTO_PATCH - m,
                    &sx0, &sx1, &sy0, &sy1);
      const float v = tuneRectMean(sx0, sx1, sy0, sy1);
      sum[photoLv[c]] += v;
      nread[photoLv[c]]++;
      Serial.printf(" L%d=%.0f", photoLv[c], v);
    }
    Serial.println();
    evoUnpaintAll();                     // charge-matched erase between rounds
  }

  float meas[16];
  for (int g = 0; g < 16; g++)
    meas[g] = nread[g] ? sum[g] / nread[g] : 0.0f;
  uint8_t lum[16];
  tuneCurveFrom(meas, 16, lum, "photo");
  // A photo-context curve is allowed SMALL inversions: the white/black/
  // photo probe (29 Jul) showed adjacent levels genuinely swapping under
  // dithered content, and the cure is to stop pretending there is
  // contrast between them. Pool each run of violators to its weighted
  // mean (isotonic regression), so the dither treats those levels as the
  // equals the glass says they are. A LARGE inversion is still a broken
  // measurement - a lifted lid, a failed scan - not physics: refuse it.
  {
    float val[16]; int wgt[16]; int np = 0;
    for (int g = 0; g < 16; g++) {
      val[np] = (float)lum[g]; wgt[np] = 1; np++;
      while (np > 1 && val[np - 1] > val[np - 2]) {   // violates decreasing
        val[np - 2] = (val[np - 2] * wgt[np - 2] + val[np - 1] * wgt[np - 1])
                      / (float)(wgt[np - 2] + wgt[np - 1]);
        wgt[np - 2] += wgt[np - 1];
        np--;
      }
    }
    float worstAdj = 0;
    int idx = 0;
    uint8_t pooled[16];
    for (int b = 0; b < np; b++)
      for (int k = 0; k < wgt[b]; k++, idx++) {
        const float adj = fabsf(val[b] - (float)lum[idx]);
        if (adj > worstAdj) worstAdj = adj;
        pooled[idx] = (uint8_t)lroundf(val[b]);
      }
    if (worstAdj > 12.0f) {
      Serial.printf("[photo] inversion needs a %.0f-lum pool - measurement "
                    "broken, not storing\n", worstAdj);
      return false;
    }
    if (worstAdj > 0) {
      Serial.print("[photo] pooled inverted neighbours:");
      for (int g = 0; g < 16; g++) {
        if (pooled[g] != lum[g]) Serial.printf(" L%d %d->%d", g, lum[g], pooled[g]);
        lum[g] = pooled[g];
      }
      Serial.println();
    }
  }
  if (!tunedStoreCurve(epd.driver(), s, lum)) {
    Serial.println("[photo] could not update the stored blob");
    return false;
  }
  Serial.printf("[photo] %s blob now carries the photo-context curve\n",
                tunedSetName(s));
  return true;
}

// Standalone staircase check against whatever tables are currently
// installed - the flash-tuned set if the board has one, the preset or
// formula library otherwise. Nothing is changed; this only measures.
static bool runStaircaseCheck(EPD_Painter::Quality q) {
  if (!scPort) { Serial.println("[evo] no scanner selected"); return false; }
  epd.setQuality(q);
  tuneLen = epd.driver().g16TrainLen();
  if (!epd.driver().setGreyLevels(16)) {
    Serial.println("[evo] setGreyLevels(16) refused");
    return false;
  }
  if (!tuneAdoptInstalled()) {
    Serial.println("[evo] no installed trains to derive erases from");
    return false;
  }
  Serial.printf("[evo] staircase check: %s, %d passes at %d us\n",
                q == EPD_Painter::Quality::QUALITY_HIGH ? "HIGH" : "NORMAL",
                tuneLen,
                q == EPD_Painter::Quality::QUALITY_HIGH
                    ? epd.driver()._config.g16_pass_us_high
                    : epd.driver()._config.g16_pass_us_normal);
  if (!tgeo.valid && !tuneRegister()) return false;

  const uint8_t *stored = (q == EPD_Painter::Quality::QUALITY_HIGH)
                              ? EPD_PainterTuned::levelLuminanceHigh()
                              : EPD_PainterTuned::levelLuminance();
  uint8_t lum[16];
  if (stored) memcpy(lum, stored, 16);
  else for (int i = 0; i < 16; i++) lum[i] = (uint8_t)(255 - i * 255 / 15);
  Serial.printf("[evo] comparing against %s curve\n",
                stored ? "the stored (flash-tuned)" : "an assumed even");
  return evoVerifyStaircase(lum) >= 0;
}

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// The incumbent — what this board is already running.
// ---------------------------------------------------------------------------
// Seeding every level from the same synthetic genome throws away a table that
// cost forty scans to produce and is already within single-digit units. The
// search is (1+3) mutation from an elite, so the seed IS the starting point:
// begin at the incumbent and generation 1 is refining a good answer; begin at
// the formula and the first several generations are spent walking back to
// where the board already was.
//
// Cross-quality seeding is where the slot count matters. A HIGH train has 20
// slots and a NORMAL one 13, so there is no straight copy — but a drive code
// means the same thing in either as long as the PASS PERIOD is the same,
// because that is what fixes the dose a slot delivers. So NORMAL's tuned
// train goes into HIGH's first 13 slots and the remaining 7 are left float:
// the level lands exactly where it did before, and evolution gets seven spare
// slots to shape with rather than seven passes to discover from scratch.
//
// If the two qualities run DIFFERENT periods the mapping is approximate — the
// shape is still right but each slot's dose is not — so runEvoTune says so
// rather than letting it pass silently.
static uint8_t     evoIncumbent[16][TUNE_LEN_MAX];
static uint8_t     evoIncumbentRm[16][TUNE_LEN_MAX];
static bool        evoIncumbentHasRm = false;
static int         evoIncumbentLen  = 0;         // 0 = nothing worth seeding from
static const char *evoIncumbentWhat = "the formula library";

static void evoCaptureIncumbent(EPD_Painter::Quality target) {
  evoIncumbentLen  = 0;
  evoIncumbentHasRm = false;
  evoIncumbentWhat = "the formula library";
  memset(evoIncumbent, 0, sizeof(evoIncumbent));
  memset(evoIncumbentRm, 0, sizeof(evoIncumbentRm));

  const bool high = (target == EPD_Painter::Quality::QUALITY_HIGH);
  const auto &tr = epd.driver()._config.trains;
  const bool haveTarget = high
      ? (tr.g16_apply_high != nullptr || tunedPresent(epd.driver(), SET_HIGH))
      : (tr.g16_apply      != nullptr || tunedPresent(epd.driver(), SET_NORMAL));

  // Best case: this quality has its own table. Read what is INSTALLED rather
  // than the preset directly, so a flash blob (a previous tune) wins over the
  // shipped table, which is the whole point of having tuned it.
  if (haveTarget) {
    // From level 1: level 0 is paper white and is never driven, so it has no
    // train and decisionTrain() rightly refuses id 0. Asking for it and then
    // treating the refusal as "no tables here" is how this silently fell back
    // to the formula on a board that had a perfectly good tuned set.
    for (int g = 1; g < 16; g++) {
      const uint8_t *t = epd.driver().decisionTrain((uint8_t)((g << 1) | 0));
      if (!t) return;
      memcpy(evoIncumbent[g], t, tuneLen);
    }
    // The installed removes seed the erase half of each pair. They are
    // charge-matched by construction wherever they came from; a missing
    // one falls back to the code-swap mirror in evoSeed().
    evoIncumbentHasRm = true;
    for (int g = 1; g < 16; g++) {
      const uint8_t *t = epd.driver().decisionTrain((uint8_t)((g << 1) | 1));
      if (!t) { evoIncumbentHasRm = false; break; }
      memcpy(evoIncumbentRm[g], t, tuneLen);
    }
    evoIncumbentLen  = tuneLen;
    evoIncumbentWhat = "this quality's own installed table";
    return;
  }

  // NORMAL with no table: the formula is genuinely all there is.
  if (!high) return;

  // HIGH with no HIGH table, which is every board today. Borrow NORMAL's,
  // which means briefly rebuilding the library at NORMAL to read it.
  if (tr.g16_apply == nullptr && !tunedPresent(epd.driver(), SET_NORMAL)) return;
  epd.setQuality(EPD_Painter::Quality::QUALITY_NORMAL);
  bool ok = true;
  for (int g = 1; g < 16 && ok; g++) {   // level 0 is never driven — see above
    const uint8_t *t = epd.driver().decisionTrain((uint8_t)((g << 1) | 0));
    if (!t) { ok = false; break; }
    memcpy(evoIncumbent[g], t, EPD_Painter::DEC_WF_LEN16);
  }
  epd.setQuality(EPD_Painter::Quality::QUALITY_HIGH);
  if (!ok) { memset(evoIncumbent, 0, sizeof(evoIncumbent)); return; }
  evoIncumbentLen  = EPD_Painter::DEC_WF_LEN16;
  evoIncumbentWhat = "NORMAL's installed table in the first 13 of 20 slots";
}

static void evoSeed() {
  for (int g = 0; g < 16; g++) {
    EvoLv &L = ev[g];
    memset(&L, 0, sizeof(L));
    memset(L.elite, 0, TUNE_LEN_MAX);
    if (evoIncumbentLen) {
      // The tail past evoIncumbentLen is already float from the capture.
      memcpy(L.elite, evoIncumbent[g], TUNE_LEN_MAX);
    } else {
      int p = 0, placed = 0;
      while (placed < evoSeedDark() && p < tuneLen) {
        if (evoSlotUsable(p)) { L.elite[p] = 1; placed++; }
        p++;
      }
      placed = 0;
      while (placed < evoSeedLight() && p < tuneLen) {
        if (evoSlotUsable(p)) { L.elite[p] = 2; placed++; }
        p++;
      }
    }
    // The erase half of the pair: the installed remove when there is one,
    // else the code-swap mirror of the apply (11d+6w seeds as 11w+6d) —
    // net exactly negated by construction, order for evolution to find.
    if (evoIncumbentLen && evoIncumbentHasRm) {
      memcpy(L.eliteRm, evoIncumbentRm[g], TUNE_LEN_MAX);
    } else {
      for (int i = 0; i < TUNE_LEN_MAX; i++)
        L.eliteRm[i] = (L.elite[i] == 1) ? 2 : (L.elite[i] == 2) ? 1 : 0;
    }
    evoBalanceRemove(L.elite, L.eliteRm, tuneLen);
    memcpy(L.best,     L.elite,   TUNE_LEN_MAX);
    memcpy(L.second,   L.elite,   TUNE_LEN_MAX);
    memcpy(L.bestRm,   L.eliteRm, TUNE_LEN_MAX);
    memcpy(L.secondRm, L.eliteRm, TUNE_LEN_MAX);
    L.eliteErr    = 1e9f;
    L.eliteResid  = 0.0f;
    L.bestScore   = 1e9f;
    L.secondScore = 1e9f;
    // Mutate gently when starting from a measured table: the job is to refine
    // an answer that is already close, and 2.0 expected edits would knock a
    // good train apart faster than selection can put it back. From the
    // formula there is nothing to protect, so explore at the old strength.
    L.sigma     = evoIncumbentLen ? 1.0f : 2.0f;
  }
}

static void evoPrintTrain(const uint8_t *t) {
  for (int i = 0; i < tuneLen; i++) Serial.printf("%d%s", t[i], i < tuneLen - 1 ? "," : "");
}

static bool runEvoTune(EPD_Painter::Quality quality, int generations) {
  if (!scPort) { Serial.println("[evo] no scanner selected"); return false; }
  const bool high = (quality == EPD_Painter::Quality::QUALITY_HIGH);
  const Set set = high ? SET_HIGH : SET_NORMAL;
  Serial.printf("[evo] ===== EVOLUTIONARY TUNE (%s) =====\n", tunedSetName(set));
  Serial.println("[evo] send q to abort between generations");

  epd.setQuality(quality);
  tuneLen = epd.driver().g16TrainLen();
  int &periodCfg = high ? epd.driver()._config.g16_pass_us_high
                        : epd.driver()._config.g16_pass_us_normal;
  // What the board was running before the run pins its own timing, so a
  // SKIP can put ALL of it back — preset tables at evo-pinned timing would
  // be a mismatch that lasts until reboot.
  const int savedPeriodCfg = periodCfg;
  const int savedLeHold    = epd.driver()._config.le_hold_ns;
  if (high && EVO_PERIOD_US_HIGH) periodCfg = EVO_PERIOD_US_HIGH;
  if (tunePeriodUs) periodCfg = (int)tunePeriodUs;
  const uint32_t period = (uint32_t)periodCfg;
  // Pin the row timing for this quality before measuring anything.
  int &rowExtra = high ? epd.driver()._config.row_extra_us_high
                       : epd.driver()._config.row_extra_us_normal;
  const int savedRowExtra = rowExtra;
  rowExtra = EVO_ROW_EXTRA_US;
  // And the latch width, for exactly the same reason. Installing a blob
  // restores the timing it was tuned at, and every blob written before
  // le_hold_ns existed reads back 0 - so a tune started after boot would
  // silently calibrate against an uncontrolled latch and bake the result
  // in. Pin it explicitly rather than inheriting whatever loaded.
  epd.driver()._config.le_hold_ns = EVO_LE_HOLD_NS;
  Serial.printf("[evo] timing pinned: period %d us, row charge %d us, LE %d ns "
                "(allowance %d us, row cost %d us)\n",
                periodCfg, EVO_ROW_EXTRA_US, EVO_LE_HOLD_NS,
                periodCfg - epd.driver()._config.g16_settle_us,
                EVO_ROW_EXTRA_US * epd.height());
  if (!epd.driver().setGreyLevels(16)) {
    Serial.println("[evo] setGreyLevels(16) refused");
    return false;
  }
  Serial.printf("[evo] %d passes, %lu us period, %d us row charge, "
                "%d bands x %d levels = %d candidates per scan\n",
                tuneLen, (unsigned long)period, rowExtra,
                EVO_BANDS, EVO_LAST_L - EVO_FIRST_L + 1,
                EVO_BANDS * (EVO_LAST_L - EVO_FIRST_L + 1));
  Serial.printf("[evo] seed genome: %d darkens then %d whitens, every level; "
                "black anchored at %d\n",
                evoSeedDark(), evoSeedLight(), evoAnchorDark());
  Serial.printf("[evo] patch positions: %s\n",
                evoRandomPos ? "SHUFFLED every generation"
                             : "FIXED (column g = level g, elite always band 0)");
  if (evoSettleMask) {
    int nu = 0;
    for (int i = 0; i < tuneLen; i++) if (evoSlotUsable(i)) nu++;
    Serial.printf("[evo] settle mask ON: slots 1-%d free, then every even slot "
                  "pinned float - %d usable of %d\n",
                  EVO_FREE_PREFIX, nu, tuneLen);
  }

  // One extra second over the standard gap. The printer's BJNP stack does
  // refuse connections under sustained rate - that, not the panel or the
  // algorithm, ended most runs today - but this mode already cuts the scan
  // count by 4x for the same information, which is the bigger lever.
  const int savedGap = tuneScanGapMs;
  tuneScanGapMs = savedGap + 1000;

  randomSeed(micros());
  if (!evoAllocState()) {
    Serial.println("[evo] no PSRAM for the evolution state");
    tuneScanGapMs = savedGap;
    return false;
  }
  evoCaptureIncumbent(quality);
  evoSeed();
  Serial.printf("[evo] seeded from %s\n", evoIncumbentWhat);
  if (evoIncumbentLen && evoIncumbentLen != tuneLen) {
    // Borrowed across qualities: only exact if a slot delivers the same dose
    // in both, which means the same pass period.
    const int other = epd.driver()._config.g16_pass_us_normal;
    Serial.printf("[evo] seed borrowed from a %d-slot table into %d slots; "
                  "periods %d us -> %d us%s\n",
                  evoIncumbentLen, tuneLen, other, (int)period,
                  (int)period == other
                      ? " (same, so each seeded level lands where it did)"
                      : " (DIFFER, so the seed is a shape not a landing point)");
  }
  if (!evoIncumbentLen)
    Serial.printf("[evo] no tuned table to start from - seeding %d darkens "
                  "then %d whitens on every level\n",
                  evoSeedDark(), evoSeedLight());
  if (!tgeo.valid && !tuneRegister()) { tuneScanGapMs = savedGap; return false; }

  // Static: this task's stack is 8 KB and the save path below adds a
  // kilobyte of blob on top of everything live here. A stack canary trip
  // during the save is how that was found.
  static float ref[16], nat[EVO_BANDS][16], err[EVO_BANDS][16];

  // Start from the plain density ladder every run: generation 0 measures the
  // compression curve with it, and inheriting a previous run's corrected
  // ladder would calibrate a correction on top of a correction.
  evoRefThrLinear();

  for (int gen = 0; gen < generations; gen++) {
    if (tuneAbortRequested()) { Serial.println("[evo] aborted"); break; }

    evoShufflePositions();
    for (int g = EVO_FIRST_L; g <= EVO_LAST_L; g++) evoBreed(g);
    evoPaintCard();
    if (!evoMeasure(ref, nat)) { tuneScanGapMs = savedGap; return false; }
    evoErrors(ref, nat, err);
    // Generation 0 is measured with the plain density ladder, and its only
    // job is to supply the density->luminance curve. Invert it here and
    // every later generation is selected against evenly spaced targets.
    // Done from the real card rather than a stored curve because the
    // compression is the panel's and the scanner's together, and both
    // differ per board.
    if (gen == 0 && evoRefLadderFix) {
      Serial.print("[evo] ladder as measured (linear density)  gaps:");
      for (int g = 0; g < 15; g++) Serial.printf(" %.1f", ref[g] - ref[g + 1]);
      Serial.println();
      evoRefThrFromMeasured(ref);
      Serial.print("[evo] ladder corrected, thresholds:");
      for (int g = 0; g < 16; g++) Serial.printf(" %d", (int)evoRefThr[g]);
      Serial.printf("  (even target gap %.2f)\n", (ref[0] - ref[15]) / 15.0f);
      Serial.println("[evo] targets are now evenly spaced in LUMINANCE, not ink area");
    }
    // Keep the newest references: tuneStep(), the results screen's full-scale
    // span and the level curve are all built from these, and a set measured
    // generations ago would carry the session's drift into all three.
    for (int i = 0; i < 16; i++) probeRef[i] = ref[i];
    evoEraseCard();      // each band erased by its candidates' OWN removes

    // The erase is half the experiment (Tony's design): the card had to be
    // taken back to white anyway, each band by its candidates' own paired
    // removes — so one scan of the now-white card reads all 56 erase
    // residuals at once. Immediately, which is the only time a ghost can
    // be measured: residuals decay within minutes. Column 0 is never
    // driven, so it is each band's own paper-white reference.
    static float refW[16], natW[EVO_BANDS][16];
    if (!evoMeasure(refW, natW)) { tuneScanGapMs = savedGap; return false; }
    static float resid[EVO_BANDS][16];
    for (int b = 0; b < EVO_BANDS; b++)
      for (int g = 0; g < 16; g++)
        resid[b][g] = natW[b][g] - natW[b][0];

    int improved = 0;
    float worst = 0, worstR = 0;
    for (int g = EVO_FIRST_L; g <= EVO_LAST_L; g++) {
      EvoLv &L = ev[g];
      // Band 0's home IS the elite, measured in this frame. Selection uses
      // these numbers only — never a value carried over from a previous
      // scan. Fitness is evoPairFitness() — the ghost-weighted round trip
      // — plus the spacing term: banding is a function of the step between
      // neighbours, not of any one level's absolute error, so a candidate
      // whose error differs from its neighbours' is penalised against
      // their current elites.
      auto score = [&](float e, float r) {
        float sp = 0;
        int   n  = 0;
        if (g > EVO_FIRST_L && ev[g - 1].eliteErr < 1e8f) {
          const float d = e - ev[g - 1].eliteErr; sp += d * d; n++;
        }
        if (g < EVO_LAST_L && ev[g + 1].eliteErr < 1e8f) {
          const float d = e - ev[g + 1].eliteErr; sp += d * d; n++;
        }
        return evoPairFitness(e, r) + (n ? EVO_SPACING_W * sp / n : 0.0f);
      };
      const int home = evoEliteBand[g];
      // Robustness penalty on the INCUMBENT only. A challenger has no
      // history yet, so it gets the benefit of the doubt; an incumbent that
      // has been measured several times and keeps landing somewhere
      // different is fragile, and this makes it easier to displace.
      float rsd = 0.0f;
      if (L.rN >= 4) rsd = sqrtf(L.rM2 / (L.rN - 1));
      float bestErr = err[home][g];
      float bestRes = resid[home][g];
      float bestSc  = score(err[home][g], resid[home][g]) + EVO_ROBUST_W * rsd * rsd;
      int   bestB   = home;
      for (int b = 0; b < EVO_BANDS; b++) {
        if (b == home) continue;
        L.trials++;
        const float sc = score(err[b][g], resid[b][g]);
        if (sc < bestSc) { bestSc = sc; bestErr = err[b][g]; bestRes = resid[b][g]; bestB = b; }
      }
      if (bestB != home) {
        memcpy(L.elite,   L.cand[bestB],   TUNE_LEN_MAX);
        memcpy(L.eliteRm, L.candRm[bestB], TUNE_LEN_MAX);
        L.wins++;
        L.stale = 0;
        improved++;
        L.rMean = L.rM2 = 0.0f;      // new pair: its history starts now
        L.rN = 0;
      } else {
        L.stale++;
      }
      { // Welford on the retained elite's error
        L.rN++;
        const float d = bestErr - L.rMean;
        L.rMean += d / L.rN;
        L.rM2   += d * (bestErr - L.rMean);
      }
      L.eliteErr   = bestErr;
      L.eliteResid = bestRes;
      // Hall of fame is scored on the pair's own round trip alone —
      // spacing is contextual and cannot travel between frames.
      const float raw = evoPairFitness(bestErr, bestRes);
      if (raw < L.bestScore) {
        // Demote the old champion rather than discard it - two independent
        // pairs to re-test is a better hedge against a lucky read than one.
        if (memcmp(L.best, L.elite, tuneLen) ||
            memcmp(L.bestRm, L.eliteRm, tuneLen)) {
          L.secondScore = L.bestScore;
          memcpy(L.second,   L.best,   TUNE_LEN_MAX);
          memcpy(L.secondRm, L.bestRm, TUNE_LEN_MAX);
        }
        L.bestScore = raw;
        memcpy(L.best,   L.elite,   TUNE_LEN_MAX);
        memcpy(L.bestRm, L.eliteRm, TUNE_LEN_MAX);
      } else if (raw < L.secondScore &&
                 (memcmp(L.best, L.elite, tuneLen) ||
                  memcmp(L.bestRm, L.eliteRm, tuneLen))) {
        L.secondScore = raw;
        memcpy(L.second,   L.elite,   TUNE_LEN_MAX);
        memcpy(L.secondRm, L.eliteRm, TUNE_LEN_MAX);
      }
      if (fabsf(bestErr) > worst)  worst  = fabsf(bestErr);
      if (fabsf(bestRes) > worstR) worstR = fabsf(bestRes);

      // Self-adaptation, Rechenberg's 1/5th success rule: if more than a
      // fifth of mutations are being accepted the search is in easy country
      // and should take bigger steps; if fewer, it is near an optimum (or
      // stuck) and should refine. The stall term on top pushes a level that
      // has gone nowhere for several generations out of its basin rather
      // than letting it grind, which is precisely the failure the grammar
      // converger showed on L9.
      if (L.trials >= 12) {
        const float rate = (float)L.wins / (float)L.trials;
        L.sigma *= (rate > 0.2f) ? 1.3f : 0.85f;
        L.wins = L.trials = 0;
      }
      if (L.stale >= 4) { L.sigma *= 1.5f; L.stale = 0; }
      L.sigma = constrain(L.sigma, 1.0f, 6.0f);
    }

    Serial.printf("[evo] gen %2d  worst %5.1f  ghost %4.1f  improved %2d/14  |",
                  gen, worst, worstR, improved);
    for (int g = EVO_FIRST_L; g <= EVO_LAST_L; g++) Serial.printf("%+3.0f", ev[g].eliteErr);
    Serial.print("  rm|");
    for (int g = EVO_FIRST_L; g <= EVO_LAST_L; g++) Serial.printf("%+3.0f", ev[g].eliteResid);
    Serial.println();

    // The dither references themselves, every few generations. These are the
    // TARGETS - if they bunch together at either end, no train can separate
    // the levels there and the ramp will compress however well the search
    // converges. The dark end is the suspect: a scanner's dynamic range runs
    // out long before the panel's does, and a 14/15-density dither may be
    // optically indistinguishable from a 15/15 one through a JPEG.
    if ((gen % 8) == 0) {
      Serial.print("[evo] refs:");
      for (int g = 0; g < 16; g++) Serial.printf(" %3.0f", ref[g]);
      Serial.print("  gaps:");
      for (int g = 0; g < 15; g++) Serial.printf(" %.1f", ref[g] - ref[g + 1]);
      Serial.println();
    }
  }

  // ---- confirmation: was the best-ever real, or a lucky read? --------------
  //
  // Selection compares four candidates in one frame, which cancels
  // common-mode noise (lamp, panel drift) but not per-patch noise. Once the
  // true differences fall to a couple of units - below the scan's own noise
  // - a mutant can displace a genuinely better elite on a lucky read, and
  // the incumbent random-walks away from shapes the run already found. That
  // is drift dominating selection, and no amount of extra generations fixes
  // it; the population is four.
  //
  // So put the candidates on the glass TOGETHER and read each twice:
  //
  //     band 0  incumbent      band 1  challenger
  //     band 3  incumbent      band 2  challenger
  //
  // Averaging the pair halves the per-patch noise on both, and the
  // incumbent's two bands differ ONLY by noise - which measures it directly
  // rather than leaving it as an assumption.
  {
    static float nat2[EVO_BANDS][16], err2[EVO_BANDS][16];
    for (int round = 0; round < 2; round++) {
      const bool useSecond = (round == 1);
      int adopted = 0;
      evoShufflePositions();
      for (int g = EVO_FIRST_L; g <= EVO_LAST_L; g++) {
        // A challenger that was never actually MEASURED is not a
        // challenger. best[]/second[] start out holding the seed genome
        // with a sentinel score, and the seed is the same mid-grey for
        // every level - so putting it up against a converged near-black
        // elite and adopting it on a lucky read is a ~59 unit error. That
        // is precisely what happened before this guard existed.
        const bool haveChal = useSecond ? (ev[g].secondScore < 1e8f)
                                        : (ev[g].bestScore   < 1e8f);
        const uint8_t *chal   = useSecond ? ev[g].second   : ev[g].best;
        const uint8_t *chalRm = useSecond ? ev[g].secondRm : ev[g].bestRm;
        if (!haveChal) { chal = ev[g].elite; chalRm = ev[g].eliteRm; }
        // Incumbent in bands 0 and 3, challenger in 1 and 2 - deliberately
        // interleaved rather than blocked, so each gets one outer band and
        // one inner one and any top-to-bottom trend affects both equally.
        memcpy(ev[g].cand[0],   ev[g].elite,   TUNE_LEN_MAX);
        memcpy(ev[g].cand[3],   ev[g].elite,   TUNE_LEN_MAX);
        memcpy(ev[g].cand[1],   chal,          TUNE_LEN_MAX);
        memcpy(ev[g].cand[2],   chal,          TUNE_LEN_MAX);
        memcpy(ev[g].candRm[0], ev[g].eliteRm, TUNE_LEN_MAX);
        memcpy(ev[g].candRm[3], ev[g].eliteRm, TUNE_LEN_MAX);
        memcpy(ev[g].candRm[1], chalRm,        TUNE_LEN_MAX);
        memcpy(ev[g].candRm[2], chalRm,        TUNE_LEN_MAX);
      }
      evoPaintCard();
      if (!evoMeasure(ref, nat2)) break;
      evoErrors(ref, nat2, err2);
      for (int i = 0; i < 16; i++) probeRef[i] = ref[i];
      evoEraseCard();
      // The pair is judged on its whole round trip, so the confirmation
      // needs the erase residuals too — same extra scan as a generation.
      static float refW2[16], natW2[EVO_BANDS][16];
      if (!evoMeasure(refW2, natW2)) break;
      static float res2[EVO_BANDS][16];
      for (int b = 0; b < EVO_BANDS; b++)
        for (int g = 0; g < 16; g++)
          res2[b][g] = natW2[b][g] - natW2[b][0];

      float noise = 0;
      int   nn = 0;
      for (int g = EVO_FIRST_L; g <= EVO_LAST_L; g++) {
        const float eliteE = 0.5f * (err2[0][g] + err2[3][g]);
        const float chalE  = 0.5f * (err2[1][g] + err2[2][g]);
        const float eliteR = 0.5f * (res2[0][g] + res2[3][g]);
        const float chalR  = 0.5f * (res2[1][g] + res2[2][g]);
        noise += fabsf(err2[0][g] - err2[3][g]);
        nn++;
        const bool haveChal = useSecond ? (ev[g].secondScore < 1e8f)
                                        : (ev[g].bestScore   < 1e8f);
        if (haveChal && evoPairFitness(chalE, chalR) <
                        evoPairFitness(eliteE, eliteR)) {
          memcpy(ev[g].elite,   useSecond ? ev[g].second   : ev[g].best,   TUNE_LEN_MAX);
          memcpy(ev[g].eliteRm, useSecond ? ev[g].secondRm : ev[g].bestRm, TUNE_LEN_MAX);
          ev[g].eliteErr   = chalE;
          ev[g].eliteResid = chalR;
          adopted++;
        } else {
          // Average both reads of the incumbent - when it ran unopposed all
          // four bands hold it, so this is a four-read mean.
          ev[g].eliteErr   = haveChal ? eliteE
                                      : 0.25f * (err2[0][g] + err2[1][g] +
                                                 err2[2][g] + err2[3][g]);
          ev[g].eliteResid = haveChal ? eliteR
                                      : 0.25f * (res2[0][g] + res2[1][g] +
                                                 res2[2][g] + res2[3][g]);
        }
      }
      Serial.printf("[evo] confirm %d (%s): adopted %d/14, per-patch noise "
                    "%.2f units (duplicate reads of the same train)\n",
                    round, useSecond ? "2nd-best" : "best-ever", adopted,
                    nn ? noise / nn : 0.0f);
    }
    float w = 0;
    for (int g = EVO_FIRST_L; g <= EVO_LAST_L; g++) w = max(w, fabsf(ev[g].eliteErr));
    Serial.printf("[evo] after confirmation: worst %.1f\n", w);
  }

  tuneScanGapMs = savedGap;

  Serial.println("[evo] final elite pairs:");
  for (int g = EVO_FIRST_L; g <= EVO_LAST_L; g++) {
    Serial.printf("  L%-2d err %+5.1f ghost %+5.1f (best %5.1f)  {",
                  g, ev[g].eliteErr, ev[g].eliteResid, ev[g].bestScore);
    evoPrintTrain(ev[g].elite);
    Serial.print("} rm {");
    evoPrintTrain(ev[g].eliteRm);
    Serial.println("}");
  }

  // ---- hand over to the shared pipeline ------------------------------------
  // The evolved trains are raw drive sequences, which is exactly what
  // TuneLv::raw exists to hold. Publishing them there means the
  // charge-matched removes, the optical erase verify, the reports, the
  // results screen and the blob writer all work unchanged - they were
  // already built to cope with rescue-owned levels that the grammar could
  // not represent, and every level is now one of those.
  float mx = 0;
  for (int g = EVO_FIRST_L; g <= EVO_LAST_L; g++) {
    tl[g] = {};
    tl[g].hasRaw = true;
    memcpy(tl[g].raw, ev[g].elite, TUNE_LEN_MAX);
    // The evolved remove travels with its apply: tuneRemoveOf() returns it
    // verbatim, so the verify below, the blob and every later erase use
    // the pair that was actually selected — not a formula derivation.
    tl[g].hasRawRm = true;
    memcpy(tl[g].rawRm, ev[g].eliteRm, TUNE_LEN_MAX);
    tl[g].err = ev[g].eliteErr;
    mx = max(mx, fabsf(ev[g].eliteErr));
  }
  tl[0].err = 0;
  tl[15].err = 0;
  evoPinAnchor();
  tuneUploadApplies();

  Serial.println("[evo] verifying the evolved removes (anchor stays formula-matched)...");
  memset(removeExtra, 0, sizeof(removeExtra));
  float worst = -1;
  for (int round = 0; round < 3; round++) {
    tuneUploadRemoves();
    worst = tuneVerifyRemoves();
    if (worst < 0) { Serial.println("[evo] remove verify scan failed"); break; }
    if (worst <= 6.0f) break;
    Serial.println("[evo] widening ghosting removes and re-checking...");
  }

  static uint8_t ap[16][TUNE_LEN_MAX], rm[16][TUNE_LEN_MAX];
  memset(ap, 0, sizeof(ap));
  memset(rm, 0, sizeof(rm));
  for (int g = 1; g <= 15; g++) {
    tuneTrainOf(g, ap[g]);
    tuneRemoveOf(g, rm[g]);
  }

  // The measured level curve, from the SAME frame the elites were selected
  // in: probeRef holds that scan's dither references and tl[].err its
  // residuals, so ref + err is the gradient-corrected landing directly.
  uint8_t lum[16];
  {
    float meas[16];
    for (int i = 0; i < 16; i++) meas[i] = probeRef[i] + tl[i].err;
    tuneCurveFrom(meas, 16, lum, "evo");
  }
  tuneDumpTables();

  // Independent check on content the tables were NOT tuned against, before
  // anything is written. If this disagrees with the match card, the tables
  // are card-specific and should not be kept.
  uint8_t stairLum[16];
  uint16_t invMask = 0;
  float stair = evoVerifyStaircase(lum, stairLum, &invMask);

  // SECOND-PLACE REPAIR (Tony's idea, 30 Jul).
  //
  // A staircase inversion condemns the whole table, and until now that meant
  // throwing away a run whose other fourteen levels were fine — the LilyGo's
  // HIGH set confirmed at 2.7, the best of the day, and was binned for one
  // reversed pair. But the hall of fame already holds a RUNNER-UP pair per
  // level, kept precisely because it scored nearly as well by a different
  // shape. If the leader inverts against its neighbour, the runner-up is the
  // obvious thing to try, and it costs one scan pair rather than a re-run.
  //
  // Only the flagged levels are swapped; everything else keeps its winner.
  // The swap is kept only if the staircase genuinely improves — fewer
  // inversions, or the same count with a better worst — so a repair that
  // makes things worse is reverted and the run is rejected as before.
  for (int attempt = 0; stair >= 0 && invMask && attempt < EVO_REPAIR_TRIES; attempt++) {
    int swapped = 0;
    for (int g = EVO_FIRST_L; g <= EVO_LAST_L; g++) {
      if (!(invMask & (1u << g))) continue;
      if (ev[g].secondScore >= 1e8f) continue;          // no runner-up held
      memcpy(ap[g], ev[g].second,   TUNE_LEN_MAX);
      memcpy(rm[g], ev[g].secondRm, TUNE_LEN_MAX);
      tuneTrainOf(g, ap[g]);
      tuneRemoveOf(g, rm[g]);
      swapped++;
    }
    if (!swapped) {
      Serial.println("[evo] repair: no runner-up available for the inverted "
                     "levels - keeping the winners");
      break;
    }
    Serial.printf("[evo] repair attempt %d: swapped %d inverted level(s) to "
                  "their runner-up, re-measuring\n", attempt + 1, swapped);

    uint8_t tryLum[16];
    uint16_t tryMask = 0;
    const float tryStair = evoVerifyStaircase(lum, tryLum, &tryMask);
    if (tryStair < 0) break;                            // scan failed, stop

    const int wasInv = __builtin_popcount(invMask);
    const int nowInv = __builtin_popcount(tryMask);
    if (nowInv < wasInv || (nowInv == wasInv && tryStair < stair)) {
      Serial.printf("[evo] repair KEPT (inverted levels %d -> %d, worst "
                    "%.1f -> %.1f)\n", wasInv, nowInv, stair, tryStair);
      memcpy(stairLum, tryLum, 16);
      stair = tryStair;
      invMask = tryMask;
      if (!invMask) break;                              // monotonic: done
    } else {
      Serial.printf("[evo] repair REJECTED (inverted levels %d -> %d, worst "
                    "%.1f -> %.1f) - restoring the winners\n",
                    wasInv, nowInv, stair, tryStair);
      for (int g = EVO_FIRST_L; g <= EVO_LAST_L; g++) {
        if (!(invMask & (1u << g))) continue;
        memcpy(ap[g], ev[g].best,   TUNE_LEN_MAX);
        memcpy(rm[g], ev[g].bestRm, TUNE_LEN_MAX);
        tuneTrainOf(g, ap[g]);
        tuneRemoveOf(g, rm[g]);
      }
      break;
    }
  }

  if (stair >= 0) {
    mx = max(mx, stair);
    // Store the STAIRCASE curve, not the match card's. The curve exists
    // for one purpose — image dithering — and a photograph is full-height
    // mixed content like the staircase, not half-dither rows like the
    // card. The two disagreed by up to 7 lum on this board, and error
    // diffusion against a curve that is a level-step wrong at the
    // mid-tones is exactly what photo banding looks like.
    memcpy(lum, stairLum, 16);
    Serial.println("[evo] storing the staircase-measured curve "
                   "(photo-like content), not the match card's");
  }

  const bool keep = tuneResultsScreen(mx, worst < 0 ? 0.0f : worst);
  bool saved = false, loaded = false;
  if (keep) {
    saved = tunedSave16(epd.driver(), set, period, tuneLen, ap, rm, lum);
    loaded = saved && tunedLoad(epd.driver(), set);
    Serial.printf("[evo] flash save %s, reload %s\n",
                  saved ? "OK" : "FAILED", loaded ? "OK" : "FAILED");
    char sub[96];
    snprintf(sub, sizeof(sub), "%s, %d passes at %lu us - loaded on every boot",
             tunedSetName(set), tuneLen, (unsigned long)period);
    tuneConfirmScreen(loaded ? "SAVED" : "SAVE FAILED",
                      loaded ? "Your panel now uses these greys" : "Nothing was stored",
                      loaded ? sub : "Could not write to storage.");
  } else {
    // Tables AND timing: the run pinned period, row charge and latch width
    // for its own measurements, and the restored tables are only valid at
    // the timing they came with.
    periodCfg = savedPeriodCfg;
    rowExtra  = savedRowExtra;
    epd.driver()._config.le_hold_ns = savedLeHold;
    epd.driver().rebuildDecisionTrains();
    Serial.println("[evo] discarded - built-in tables and timing restored");
    tuneConfirmScreen("SKIPPED", "Nothing was changed",
                      "The built-in tables are still in charge.");
  }

  Serial.printf("[evo] ===== DONE (%s: worst %.1f, erase %.1f, %s) =====\n",
                tunedSetName(set), mx, worst, keep ? "saved" : "discarded");
  return keep && saved && loaded;
}

#endif  // TUNEUP_HAVE_JPEGDEC
