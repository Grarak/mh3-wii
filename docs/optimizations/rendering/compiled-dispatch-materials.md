# Compiled materials for model dispatches

## What the original does

Before each shape it draws, NW4R g3d runs the material's scope steps
(0x80076220): textures and palettes, the TEV pipeline, pixel and colour
state, channels, texture coordinate generation. Each step writes GX
registers and g3d's CPU caches, and what one draw leaves behind is the
starting state of the next. A model with many materials repeats this per
shape every frame, mostly with the same results.

## What the port does instead

The render thread replays these steps from captured inputs
([deferred model preparation](deferred-model-preparation.md)). On top of
that, each material record (the captured static inputs of one material
scope) compiles its steps once:

- **The image.** At a record's first dispatch with a given genmode, the
  steps run from nothing known on a scratch owner, and their result (the
  prepared fields, the pixel and colour group versions) is merged over an
  all-zero material: the record's image (`ModelPrepDispatch`). The fields
  the image leaves unknown are the ones earlier draws would have supplied;
  none of them changes what a draw derives once the dispatch's own colours,
  channels, indirect operator and matrix selectors are on it.
- **The redirect.** Later dispatches run the steps without their
  publications (only the bookkeeping, texture binds and depth mode), then
  point the render owner's material at the record's draw material
  (`RedirectNativeRenderMaterial`): the image with the live inputs it was
  last rebuilt for. Nothing is copied unless the live inputs differ; a
  partial colour restore, an uncompiled dispatch or a deleted record copies
  the draw material back first. A full material restore ends the redirect.
- **In-place compares.** The record key and the live inputs are compared
  against the dispatch's inputs as words where they are (`memcmp` is a byte
  loop in the Vita's C library), without building key structs first.
- **Texture memos.** A record's texture slot keeps a memo of its resolved
  texture; while no texture revision was produced since and an indexed
  image's palette is unchanged, the bind skips the cache lookup, the lock
  and the sampler search.
- A heap created or destroyed (an area change) releases the records
  captured from its range.

## Why

The village draws about 90 distinct material instances a frame, each with
its own textures, so merging draws never paid; making each dispatch cheap
did. Vita, village walk, the render thread's model preparation per packet:

| Change | ms/packet |
| --- | ---: |
| Per-word merge of the publications | 5.33 |
| Merge per dispatch, record-level | 4.78 |
| Compiled image and redirect | 4.23 |
| In-place compares, memoized rebinds | 3.19 |

The render packet went from about 15.8 to 14.8 ms. A development check
compared every compiled dispatch's draw material with one built for it
alone (500,000 dispatches in the village and at the beach) and every memo
rebind with a full resolution (200,000): no difference.

## Example

`runtime/src/model_render_prep.cpp` keeps, per record, the image and the
live inputs it was rebuilt for; a dispatch whose inputs compare equal word
for word draws straight from the record's draw material, and the shape
step's top fields and matrix selectors publish onto it.

## Applying it to another Wii game

Generic: any renderer that replays GX material setup per draw can cache the
result per material structure and redirect instead of re-merging, provided
it can tell which live values a draw still contributes. Prove the image
rule with a check that builds every draw both ways before relying on it.

Game-specific: which inputs vary per dispatch. In MH3 they are the colours,
channels, indirect operator and matrix selectors; a game that animates
other material state per draw needs those in the live set.
