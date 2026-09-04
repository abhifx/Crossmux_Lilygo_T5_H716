# web_browser — how a browser feels on e-paper

A hand-built article page in a fake browser chrome that you scroll with your
thumb. There is no network and no HTML; the point is the **feel** of a
scrolling viewport on this hardware, which is the thing worth knowing before
anyone writes a real browser against this library.

Drag to scroll — the page follows your thumb. Flick and let go and it carries
on briefly, then settles. Tap the top bar to jump back to the top.

Serial: `j` / `k` page down/up, `t` top, `r` redraw.

## Why these settings

**4-grey NORMAL, not 16-grey.** Scrolling is the worst case for a delta-update
engine. Every other demo here wins by repainting only what changed; when a page
slides, essentially every pixel changes, so what is left is the raw cost of a
full frame. Four levels at 4-level NORMAL are far cheaper than sixteen at
13×20 ms, and four greys are enough for text, chrome and dithered photographs.

**Direct grey-to-grey trains** (`setDirectTransitions(true)`). This is the
feature that makes it watchable. Without it, a pixel going from one grey to
another must pass through white first, so a moving page is visibly erased and
redrawn. With it, that pixel is driven straight to its new level in one paint —
and grey-to-grey is exactly what scrolling consists of. If the call is refused
for want of internal RAM the sketch says so on serial, and you will see the
difference immediately.

**Portrait.** A page wants to be taller than it is wide. 4-level mode supports
`ROTATION_CW`, so the canvas is 540×960. (16-grey is `ROTATION_0` only.)

**`paintLater()`, not `paint()`.** `paintLater()` submits a frame and returns
instead of blocking. If the thumb moves again before the panel has finished,
the newer frame replaces the queued one and the stale render is dropped, so the
page tracks the finger as closely as the hardware allows rather than falling
behind a queue of frames nobody wants to see any more.

**Images are dithered once, at startup.** `drawGray8()` error-diffuses an 8bpp
greyscale source against `Config::level_lum` — the levels as *measured on the
glass* — into level codes, which are then blitted per frame. Re-dithering every
frame would be slower and would make the grain crawl as the scroll offset
changed, because clipping the image alters which rows the error propagates
through. The two images are generated procedurally, so the example carries no
assets.

## The photograph

`hero_photo.h` is an AI-generated image (ChatGPT), not a photograph of anyone
real — as is the article around it: an invented head of state of an invented
country, which is what the seal on the lectern says too. It is here because a
face with a bright subject against a dark backdrop is a hard case for a
one-bit halftone, not to depict anybody.

Stored as 8bpp luminance and halftoned at startup, so the source is reusable
if you want to try a different dither.

## Honest limits

The panel cannot follow a fast flick the way glass can; it settles a beat
later. Reading does not actually need to keep up with a flick — it needs to
land somewhere sensible and be legible when it stops — but you should see this
for yourself rather than take the claim on trust, which is why the momentum is
deliberately short.

Text uses the built-in Adafruit 6×8 font at integer scales. A real browser
would want proportional fonts; the wrapping helper here breaks on spaces at a
fixed character width and is not trying to be a layout engine.

## Requirements

- Adafruit GFX library
- `gt911-arduino` (for touch; without it the serial keys still work)
- Board auto-probed — M5PaperS3, LilyGo T5 S3 GPS/H752, LilyGo EPD47 H716

Deliberately **not** LVGL. The interesting engineering in a scrolling e-paper
browser is deciding when and what to repaint, and LVGL owns invalidation and
frame pacing itself — it would hide the very thing this example exists to show.
The library does ship LVGL integration (`EPD_Painter_LVGL.h`) and the
`examples/lvgl/` sketches use it where widgets matter more than repaint
control.
