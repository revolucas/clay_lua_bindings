# Clay Lua (Lua wrapper for Clay)

A small **Lua wrapper** module around the single‑header layout engine **Clay** (written in C).  
This is **not** an FFI binding; it is a compiled Lua C module that exposes a Lua‑friendly API for declaring elements, running layout, and iterating the resulting render commands.

This file documents the native binding. Technetium layers retained roots,
themes, and reusable widgets on top of it; see Technetium's
`ClayBuilderAPI.md` for those higher-level APIs. For new UI, prefer the fluent
builder methods and automatic IDs described below over older named/indexed
element patterns.

---

## Features

- Lightweight Lua API mirroring Clay’s core functionality (context/init, element declaration, scrolling, pointer state).
- Allocation-light fluent builders for element layout, text, sizing, padding, colors, and floating overlays.
- A **layout end iterator** that yields typed `ClayCommand` objects per render command (rectangles, text, images, borders, scissor start/end, custom).

---

## Getting Started

### Requirements
- LuaJIT (recommended) or Lua 5.1+
- A renderer that consumes the iterated render commands to actually draw
- Build the C wrapper as a shared library so `require("clay")` works.

### Build & Load

```bash
# Example (adjust for your OS / compiler)
cc -O2 -shared -fPIC -I. -o clay.so clay_lua_bindings.c
```

```lua
local clay = require("clay")

-- Initialize Clay (arena capacity, width, height)
local arena_mem, ctx = clay.initialize(clay.minMemorySize(), 1280, 720)
```

Call `clay.setLayoutDimensions(width, height)`, `clay.setPointerState(x, y, isDown)`, and `clay.updateScrollContainers(enableDrag, scrollX, scrollY, dt)` once per frame as appropriate.

### Text measurement callbacks

Register one callback to provide text dimensions. Clay passes text and its style
fields as scalar arguments, avoiding a configuration table allocation for each
measurement:

```lua
clay.setMeasureTextFunction(function(
  text, fontId, fontSize, letterSpacing, lineHeight,
  wrapMode, textAlignment, r, g, b, a
)
  return width, height
end)
```

Passing `nil` clears the callback and uses the default measurement.

---

## Minimal frame loop (typical usage)

A typical frame using Clay looks like this:

1. Set layout dimensions (window size)
2. Set pointer state (mouse)
3. Update scroll containers (if used)
4. Begin layout
5. Build UI tree (builders)
6. End layout and iterate render commands

```lua
clay.setLayoutDimensions(w, h)
clay.setPointerState(mx, my, mouse_down)
clay.updateScrollContainers(true, scroll_dx, scroll_dy, dt)

clay.beginLayout()

clay.element()
  :wGrow()
  :hGrow()
  :open()
clay.text("Hello World")
  :fontSize(16)
  :close()
clay.close()

for command in clay.endLayoutFast() do
  -- renderer consumes command
end
```

---

## Fluent element builder: `clay.element(...)`

### Signatures

```lua
-- Default: an anonymous element gets an automatic hierarchical ID.
local spacer = clay.element()
local generatedId = spacer:id()
spacer:close()

-- Give it a named ID only when another call must address it.
local viewportId = clay.id("AssetViewport")
clay.element(viewportId):wGrow():hGrow():clip(true, true):open()
clay.close()

-- Scope anonymous IDs by stable data identity when rows may reorder.
for i = 1, #items do
  local item = items[i]
  clay.keyed(item.id, function()
    clay.element():wGrow():h(28):open()
      clay.text(item.name):close()
    clay.close()
  end)
end
```

Use `clay.element()` without arguments for ordinary layout. Its ID is derived
from the current parent and declaration order; read it with `:id()` when needed.
Use `clay.id(name, ...)` for an ID that must be created before its element or
referenced outside its declaration. For repeated anonymous children whose order
can change, wrap each item in `clay.keyed(stableKey, fn)`.

The fluent builder API is the element declaration API. Use `:open()` plus one
matching `clay.close()` for parents, `:close()` for leaves, and `:floating()` for
floating-only settings. There is no `clay.element(nil)` form, callback-style
`:children()`, standalone `clay.autoId()`, or hidden `clay.getLastElementId()`
lookup. The older named/indexed `clay.element(name, index, isLocal)` signature
is still supported for compatibility; prefer automatic IDs and keyed scopes in
new code.

---

### Lifecycle rules (important)

Calling `clay.element(...)` **immediately opens an element** in Clay.

Finish configuring it, then use one of the following:

- `:close()` — closes immediately (leaf element)
- `:open()` — finishes the element declaration and keeps it on Clay's stack while you declare children. Close it later with `clay.close()`.

If an element is garbage-collected while still open, the builder’s `__gc` will attempt a best-effort close to keep Clay’s internal stack consistent. This is a safety net, not a control flow mechanism.

---

## Element builder methods

All builder methods return `self` so they can be chained.

---

### Layout

Common shorthand methods:

```lua
:row() :col()
:gap(px)                   -- alias for childGap
:align(x, y)               -- alias for childAlignment
:alignLT() :alignCC() :alignRB() -- all nine alignment combinations are available
```

#### `:layoutDirection(dir)`

- `clay.LEFT_TO_RIGHT`
- `clay.TOP_TO_BOTTOM`

#### `:childGap(px)`

Sets spacing between children.

#### `:childAlignment(ax, ay)`

Horizontal alignment:
- `clay.ALIGN_X_LEFT`
- `clay.ALIGN_X_CENTER`
- `clay.ALIGN_X_RIGHT`

Vertical alignment:
- `clay.ALIGN_Y_TOP`
- `clay.ALIGN_Y_CENTER`
- `clay.ALIGN_Y_BOTTOM`

#### `:padding(...)`

Overloads:

```lua
:padding(all)
:padding(x, y)
:padding(left, top, right, bottom)
```

Values are stored internally as unsigned integers.

---

### Sizing

#### `:width(type, ...)` and `:height(type, ...)`

Sizing types:
- `clay.SIZING_FIXED`
- `clay.SIZING_GROW`
- `clay.SIZING_FIT`
- `clay.SIZING_PERCENT`

Usage examples:

```lua
:width(clay.SIZING_FIXED, 220)
:height(clay.SIZING_GROW)
:width(clay.SIZING_PERCENT, 100)
:width(clay.SIZING_GROW, 50, 400)
```

Argument rules:

- **FIXED**: `(size)`
- **PERCENT**: `(percent)`
- **GROW / FIT**:
  - no args → min=0, max=0
  - one arg → min
  - two args → min, max

Paired shorthand methods configure both axes:

```lua
:size(width, height)                               -- fixed on both axes
:growSize(widthMin, widthMax, heightMin, heightMax)
:fitSize(widthMin, widthMax, heightMin, heightMax)
:percentSize(widthPercent, heightPercent)
```

For `size` and `percentSize`, a `nil` axis is left unchanged. For `growSize` and
`fitSize`, omitted or `nil` bounds default to zero, matching the per-axis
GROW/FIT defaults.

---

### Visuals

Element `:backgroundColor()` and `:borderColor()` take separate `r, g, b, a`
numbers (alpha may be omitted). Text `:textColor()` also accepts a packed
`0xRRGGBBAA` value or an `{r, g, b, a}` / keyed color table. A single `nil`
leaves the current value unchanged where supported.

```lua
:backgroundColor(48, 48, 48, 255)
:borderColor(255, 255, 255, 96)
:borderWidth(1)
```

#### `:backgroundColor(r, g, b [, a])`

RGBA values (floats).

#### `:cornerRadius(all)` or `:cornerRadius(tl, tr, bl, br)`

#### `:borderColor(r, g, b [, a])`

#### `:borderWidth(all)` or `:borderWidth(l, t, r, b)`

#### `:transition(duration, property, ...)`

Enables Clay's built-in ease-out transition handler for the named properties.
Elements need a stable ID and must be declared each frame for their transition
state to continue.

```lua
clay.element("AnimatedPanel")
  :width(clay.SIZING_FIXED, expanded and 280 or 220)
  :backgroundColor(48, 56, 72, 255)
  :transition(0.2, "width", "backgroundColor")
  :open()
-- Declare children here, then close the panel.
clay.close()
```

Supported property names are `x`, `y`, `position`, `width`, `height`,
`dimensions`, `boundingBox`, `backgroundColor`, `overlayColor`, `borderColor`,
`borderWidth`, and `border`. Entry/exit callbacks and custom easing handlers are
not exposed by the Lua builder yet. This upstream revision does not carry
corner-radius or text-color values in its transition data, so those cannot be
animated through this method.

---

### Clipping

#### `:clip(v)` or `:clip(horizontal, vertical)`

Enables scissor clipping.

#### `:clipHorizontal(bool)` / `:clipVertical(bool)`

#### `:childOffset(x, y)`

Sets the clip child offset explicitly.

If clipping is enabled but `childOffset` is never set, the builder defaults to `Clay_GetScrollOffset()` at configuration time.

---

### Aspect ratio

#### `:aspectRatio(ratio)`

---

### Userdata and payloads

The following methods accept either **lightuserdata** or **any Lua value**:

- `:imageData(value)`
- `:customData(value)`
- `:userData(value)`

If a non-lightuserdata Lua value is passed, it is stored in the Lua registry and transferred to Clay as a tagged pointer.

**Lifetime rules**:

- After configuration, the builder detaches these pointers so its `__gc` does not free them
- The registry reference is released when the render command accessor is called
- These accessors are **one-shot** per render command

---

### Floating / overlays

Floating settings have their own builder. `:floating()` defaults to attaching to
the parent; choose `:root()` or `:to(id)` when the overlay belongs elsewhere.
Call `:done()` to return to the element builder before setting its layout or
render properties.

```lua
clay.element("Tooltip")
  :floating()
    :to(clay.id("SettingsButton"))
    :attach("lt", "rb")
    :offset(8, 6)
    :zIndex(100)
    :done()
  :width(clay.SIZING_FIXED, 160)
  :height(clay.SIZING_FIT)
  :padding(6, 4)
  :open()
clay.text("Settings"):close()
clay.close()
```

The floating builder supports `:parent()`, `:root()`, `:to(idOrInteger)`,
`:attach(elementPoint, parentPoint)`, `:offset(x, y)`, `:expand(w, h)`,
`:zIndex(n)`, `:pointerCapture(bool)`, `:pointerCaptureMode(mode)`,
`:clipTo(mode)`, `:clipToParent(bool)`, and `:done()`.

Keep floating placement inside `:floating()` so its attachment, offset, clipping,
pointer capture, and z-order are configured as one nested group.

---

### Closing and nesting

Use `:open()` on a parent, declare its children, then call `clay.close()` to close
the parent. Use `:close()` for a leaf element.

#### `:close()`

Applies configuration (if needed) and closes immediately.

---

## Fluent text builder: `clay.text(...)`

Use the fluent builder for new code:

```lua
clay.text("Hello")
  :fontSize(16)
  :textColor(255,255,255,255)
  :close()
```

The callback form still works for existing callers and emits the text after the
callback returns. Positional font/style arguments are also supported for
compatibility; fluent setters are easier to extend and read:

```lua
clay.text("Hello", function(t)
  t:fontSize(16)
  t:textAlignment(clay.TEXT_ALIGN_CENTER)
end)

clay.text("Hello", fontId, fontSize, color, alignment, wrapMode):close()
```

### Defaults

- `fontId = 1`
- `fontSize = 16`
- `textColor = {255,255,255,255}`
- `wrapMode = clay.TEXT_WRAP_WORDS`
- `textAlignment = clay.TEXT_ALIGN_LEFT`
- `letterSpacing = 0`
- `lineHeight = 0`

### Methods

- `:fontId(id)`
- `:fontSize(px)`
- `:textColor(r,g,b[,a])`
- `:wrapMode(mode)`
- `:textAlignment(align)`
- `:letterSpacing(px)`
- `:lineHeight(px)`
- `:userData(value)`
- `:close()` / `:done()`

Text builders emit exactly once.

---

## Render command iteration

After layout:

```lua
for cmd in clay.endLayoutIter() do
  local t = cmd:type()

  if t == clay.RENDER_RECTANGLE then
    local x,y,w,h = cmd:bounds()
    local r,g,b,a = cmd:color()
  elseif t == clay.RENDER_TEXT then
    local text, fontId, fontSize = cmd:text()
  elseif t == clay.RENDER_IMAGE then
    local data = cmd:imageData() -- one-shot
  elseif t == clay.RENDER_CUSTOM then
    local data = cmd:customData() -- one-shot
  end
end
```

If the payload was a Lua value (not lightuserdata), calling the accessor releases the registry reference and clears the pointer.

For render loops that read several properties from every command, `endLayoutFast()` returns the common render properties as a fixed tuple in one C call and does not allocate one `ClayCommand` userdata per command. Use `endLayoutIter()` when you also need command IDs, z-indices, or user data:

```lua
for t, x, y, w, h, r, g, b, a, tl, tr, bl, br, left, right, top, bottom,
    text, fontId, fontSize, letterSpacing, lineHeight, payload, clipH, clipV
    in clay.endLayoutFast() do
  if t == clay.RENDER_RECTANGLE then
    -- Use bounds, color, and corner radii.
  elseif t == clay.RENDER_TEXT then
    -- Use bounds, color, text, and font fields.
  elseif t == clay.RENDER_IMAGE then
    -- Use bounds, corner radii, and payload.
  elseif t == clay.RENDER_SCISSOR_START then
    -- Use bounds and clipH/clipV.
  end
end
```

The slots are always in this order: `type, x, y, width, height, r, g, b, a, topLeft, topRight, bottomLeft, bottomRight, borderLeft, borderRight, borderTop, borderBottom, text, fontId, fontSize, letterSpacing, lineHeight, payload, clipHorizontal, clipVertical`. Fields that do not apply to a command are `nil`. For image and custom commands, `payload` follows the same one-shot Lua-value transfer behavior as `cmd:imageData()` and `cmd:customData()`.

---

## Builder patterns

```lua
-- Parent with nested children
clay.element():col():open()
clay.text("Hello")
  :fontSize(16)
  :close()
clay.close()

-- Leaf element
clay.element()
  :wGrow()
  :h(2)
  :close()
```

---

## Best practices

- Use `:open()` and `clay.close()` for parent elements
- Prefer `:close()` for leaf elements
- Do not rely on `__gc` to close elements
- Treat `cmd:imageData()`, `cmd:customData()`, and `cmd:userData()` as **one-shot** when using Lua values
- Let the builder assign IDs for normal layout elements; use `builder:id()` when a later call needs that generated ID
- Use `clay.id(name, ...)` only when code needs a stable named ID before declaration or external reference
- Use `clay.keyed(stableKey, fn)` for anonymous repeated children that may be inserted, removed, or reordered
- Avoid positional `index` / `isLocal` ID schemes in new reusable components; their identity rules are less clear than hierarchical IDs and keyed scopes

---

## **The Layout End Iterators**

`for cmd in clay.endLayoutIter(dt) do ... end` or `for ... in clay.endLayoutFast(dt) do ... end`

### What it does
- Finalizes the current frame’s layout by calling Clay’s internal `Clay_EndLayout(deltaTime)`.
- `dt` is seconds since the previous frame and advances Clay transitions. It is optional for compatibility and defaults to `0`.
- Returns a **closure iterator** over a stable array of render commands for that frame.
- `endLayoutIter()` yields a `ClayCommand` userdata with methods to inspect the command.
- `endLayoutFast()` yields a fixed tuple of render fields without allocating a command userdata.

### Why this pattern?
- It keeps Lua code ergonomic and avoids copying the command array.
- It lets you write idiomatic `for ... in` loops to drive your renderer.

### Iterator lifecycle rules
1. **Call order**: Always `beginLayout()` → declare elements → either `endLayoutIter()` or `endLayoutFast()` (then iterate). You cannot add more elements after calling either iterator for that frame.
2. **Lifetime**: The yielded `ClayCommand` objects are only valid during the same frame. Don’t store them across frames; if you must cache, copy the primitive fields you need.
3. **One iterator per frame**: Call one iterator once per frame to consume the results. If you need multiple passes, store the commands you care about in your own structures during the first iteration.
4. **Culling & scissor**: If you enable culling (`clay.setCullingEnabled(true)`), expect fewer commands. Scissor commands (clip start/end) will appear and should be respected by your renderer.

### `ClayCommand` API
These methods exist on each yielded `cmd` (use method syntax `cmd:method()`):

- `cmd:type() -> integer`  
  Compare with exported constants:  
  `RENDER_RECTANGLE`, `RENDER_BORDER`, `RENDER_TEXT`, `RENDER_IMAGE`, `RENDER_SCISSOR_START`, `RENDER_SCISSOR_END`, `RENDER_CUSTOM`.

- `cmd:id() -> integer`  
  The element’s unique ID for the frame.

- `cmd:bounds() -> x, y, width, height`  
  Bounding box in layout coordinates.

- `cmd:zIndex() -> integer`  
  Higher values draw on top.

- `cmd:color() -> r, g, b, a`  
  - Rectangle/Text/Image only. Returns command-appropriate color (text color for text, background for rectangles, image tint if applicable). Returns nothing for types without a color.

- `cmd:text() -> string, fontId, fontSize, letterSpacing, lineHeight`  
  - Only on `RENDER_TEXT` commands.

- `cmd:cornerRadius() -> tl, tr, bl, br`  
  - Only on `RENDER_RECTANGLE` with rounded corners.

- `cmd:borderWidth() -> left, right, top, bottom`  
  - Only on `RENDER_BORDER`.

- `cmd:imageData() -> lightuserdata`  
  - Only on `RENDER_IMAGE`; pointer you passed via element config (your renderer should know how to use it).

- `cmd:clip() -> horizontal:boolean, vertical:boolean`  
  - Only on `RENDER_SCISSOR_START` / `RENDER_SCISSOR_END`.

**Important**: A method that doesn’t apply to the current command type returns nothing (`nil` in Lua). Always branch on `cmd:type()` before calling type-specific accessors.

# Passing data through render commands (`userData`, `imageData`, `customData`)

You can attach **arbitrary payloads** to elements and read them back from the **render commands** in your draw loop. The wrapper supports two forms:

1) **Lightuserdata pointer** (zero-copy, stays a pointer)  
2) **Any Lua value** (table/string/number/function/cdata/etc.) — stored internally as a registry ref and restored on read

### One-shot semantics (important!)
When you **set a non-lightuserdata Lua value**, the wrapper stores it as a **Lua registry reference** behind the scenes. On the **first** call to `cmd:imageData()`, `cmd:customData()`, or `cmd:userData()`:

- The wrapper **restores the original Lua value**,  
- **Unrefs** the registry entry to avoid leaks,  
- **Nulls out** the internal pointer for that command.

So a **second call** in the **same frame** will return `nil`. If you need the value in multiple places, cache it in a local variable on first read.

> If you pass a **lightuserdata pointer**, the wrapper returns that pointer each time (it doesn’t consume it), and you won’t hit the one-shot behavior.

---

### Constants exported for convenience
- Render types: `RENDER_RECTANGLE`, `RENDER_BORDER`, `RENDER_TEXT`, `RENDER_IMAGE`, `RENDER_SCISSOR_START`, `RENDER_SCISSOR_END`, `RENDER_CUSTOM`.
- Layout direction: `LEFT_TO_RIGHT`, `TOP_TO_BOTTOM`.
- Alignment: `ALIGN_X_LEFT`, `ALIGN_X_CENTER`, `ALIGN_X_RIGHT`, `ALIGN_Y_TOP`, `ALIGN_Y_CENTER`, `ALIGN_Y_BOTTOM`.
- Text alignment: `TEXT_ALIGN_LEFT`, `TEXT_ALIGN_CENTER`, `TEXT_ALIGN_RIGHT`.
- Text wrap: `TEXT_WRAP_NONE`, `TEXT_WRAP_WORDS`, `TEXT_WRAP_NEWLINES`.
- Sizing kinds: `SIZING_FIT`, `SIZING_GROW`, `SIZING_FIXED`, `SIZING_PERCENT`.
- Floating attach points: a family of `ATTACH_*` / `ATTACH_POINT_*` constants (see code).

---

## IDs & Helpers

- `clay.id(label, index?, isLocal?) -> idTable`  
  Create an explicit ID for `clay.element(id)` or a floating target. Element declaration option tables are not part of the API.
- `builder:id() -> idTable` returns the ID assigned to that element, including IDs generated by `clay.element()`.
- `clay.keyed(key, fn)` scopes automatic IDs inside a repeated region so entries keep their IDs when reordered.

---

## Pointer, Hover & Scrolling

- `clay.setPointerState(x, y, pointerDown)`
- `clay.hovered() -> boolean`  
- `clay.pointerOver(id) -> boolean`  
- `clay.updateScrollContainers(enableDrag, scrollX, scrollY, deltaTime)`
- `clay.getScrollOffset() -> {x, y}` (current open container)  
- `clay.getScrollContainerData(id) -> table` (dimensions, content, config, position)  
- `clay.setScrollOffset(id, x, y)` (programmatic scrolling)

**Scrollable containers:** prefer omitting `clip.childOffset` so the wrapper wires in the correct per‑element scroll offset automatically.

---

## Error Handling

If initialization fails or Clay reports an error (e.g., arena exhausted, duplicate IDs, invalid percentages), the wrapper uses Clay’s error callbacks and surfaces failures as Lua errors where appropriate.

---

## License

Same as this repository’s source files (see headers).
