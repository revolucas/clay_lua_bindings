# Clay Lua (Lua wrapper for Clay)

A small **Lua wrapper** module around the single‑header layout engine **Clay** (written in C).  
This is **not** an FFI binding; it is a compiled Lua C module that exposes a Lua‑friendly API for declaring elements, running layout, and iterating the resulting render commands.

---

## Features

- Lightweight Lua API mirroring Clay’s core functionality (context/init, element declaration, scrolling, pointer state).
- Helpers for sizing, padding, IDs, and text configuration to keep layout declarations concise.
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

Call `clay.setLayoutDimensions(width, height)`, `clay.setPointerState({x,y}, isDown)`, and `clay.updateScrollContainers(enableDrag, {x=dx,y=dy}, dt)` once per frame as appropriate.

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

clay.element("Root")
  :width(clay.SIZING_FIXED, w)
  :height(clay.SIZING_FIXED, h)
  :children(function()
    clay.text("Hello World")
      :fontSize(16)
      :done()
  end)

for cmd in clay.endLayoutIter() do
  -- renderer consumes cmd
end
```

---

## Fluent element builder: `clay.element(...)`

### Signatures

```lua
-- Common usage
local e = clay.element("Name" [, index [, isLocal]])

-- Advanced: explicit id table
local eid = clay.id("Name", index, isLocal)
local e = clay.element(eid)
```

**Parameters**
- `Name` (string): base name used for hashing the element id
- `index` (number, optional): for repeated elements (e.g. list rows)
- `isLocal` (boolean, optional): if true, the id is hashed relative to the parent element id

---

### Lifecycle rules (important)

Calling `clay.element(...)` **immediately opens an element** in Clay.

You must close it using one of the following:

- `:children(function() ... end)` — runs children and auto-closes
- `:close()` — closes immediately (leaf element)

If an element is garbage-collected while still open, the builder’s `__gc` will attempt a best-effort close to keep Clay’s internal stack consistent. This is a safety net, not a control flow mechanism.

---

## Element builder methods

All builder methods return `self` so they can be chained.

---

### Layout

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

---

### Visuals

#### `:backgroundColor(r, g, b [, a])`

RGBA values (floats).

#### `:cornerRadius(all)` or `:cornerRadius(tl, tr, bl, br)`

#### `:borderColor(r, g, b [, a])`

#### `:borderWidth(all)` or `:borderWidth(l, t, r, b)`

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

Maps directly to `decl.floating.*`:

- `:attachTo(mode)`
- `:attachPoints(elementPoint, parentPoint)`
- `:offset(x, y)`
- `:expand(w, h)`
- `:parentId(idOrNumber)`
- `:zIndex(z)`
- `:pointerCaptureMode(mode)`
- `:clipTo(mode)`

---

### Closing and children

#### `:children(fn)`

Applies configuration, executes `fn()`, then closes the element.

Errors are wrapped with `debug.traceback` and the element is closed before rethrowing.

#### `:close()`

Applies configuration (if needed) and closes immediately.

---

## Fluent text builder: `clay.text(...)`

### Signatures

```lua
clay.text("Hello")
  :fontSize(16)
  :textColor(255,255,255,255)
  :done()

-- Callback form (auto-emits)
clay.text("Hello", function(t)
  t:fontSize(16)
  t:textAlignment(clay.TEXT_ALIGN_CENTER)
end)
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

---

## Migration guide

### Old API

```lua
clay.createElement(id, decl, function()
  clay.createTextElement("Hello", {fontSize=16})
end)
```

### Builder API

```lua
clay.element("MyElement")
  :children(function()
    clay.text("Hello")
      :fontSize(16)
      :done()
  end)
```

### Leaf elements

```lua
clay.element("Spacer")
  :width(clay.SIZING_GROW)
  :height(clay.SIZING_FIXED, 2)
  :close()
```

---

## Best practices

- Prefer `:children()` for elements with children
- Prefer `:close()` for leaf elements
- Do not rely on `__gc` to close elements
- Treat `cmd:imageData()`, `cmd:customData()`, and `cmd:userData()` as **one-shot** when using Lua values
- Use `clay.id(name, index)` for interactive or queryable elements
- Use `isLocal=true` in reusable components to avoid id collisions

---

## **The Layout End Iterator**

`for cmd in clay.endLayoutIter() do ... end`

### What it does
- Finalizes the current frame’s layout by calling Clay’s internal `Clay_EndLayout()`.
- Returns a **closure iterator** over a stable array of render commands for that frame.
- Each iteration yields a `ClayCommand` userdata with methods to inspect the command.

### Why this pattern?
- It keeps Lua code ergonomic and avoids copying the command array.
- It lets you write idiomatic `for ... in` loops to drive your renderer.

### Iterator lifecycle rules
1. **Call order**: Always `beginLayout()` → declare elements → `endLayoutIter()` (then iterate). You cannot add more elements after calling `endLayoutIter()` for that frame.
2. **Lifetime**: The yielded `ClayCommand` objects are only valid during the same frame. Don’t store them across frames; if you must cache, copy the primitive fields you need.
3. **One iterator per frame**: Call `endLayoutIter()` once per frame to consume the results. If you need multiple passes, store the commands you care about in your own structures during the first iteration.
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
  Use with `createElement` to produce stable IDs.  
- `clay.sizingFixed(w)`, `clay.sizingFit(min?, max?)`, `clay.sizingGrow(min?, max?)`, `clay.sizingPercent(p)`  
- `clay.paddingAll(p)`, `clay.paddingXY(x, y)`, `clay.paddingLTRB(l,t,r,b)`

---

## Pointer, Hover & Scrolling

- `clay.setPointerState({x, y}, pointerDown)`  
- `clay.hovered() -> boolean`  
- `clay.pointerOver(id) -> boolean`  
- `clay.updateScrollContainers(enableDrag, {x, y}, deltaTime)`  
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
