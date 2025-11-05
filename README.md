# Clay Lua (Lua wrapper for Clay)

A small **Lua wrapper** module around the single‑header layout engine **Clay** (written in C).  
This is **not** an FFI binding; it is a compiled Lua C module that exposes a Lua‑friendly API for declaring elements, running layout, and iterating the resulting render commands.

> ⚠️ **Important difference (Lua clip + scroll):**  
> When creating a scrollable container, **omit** `clip.childOffset` in Lua if you want Clay’s current scroll position to be used. The wrapper will automatically read `Clay_GetScrollOffset()` for you whenever `clip.horizontal` or `clip.vertical` is set and `childOffset` is missing. In vanilla C you would typically pass `childOffset = Clay_GetScrollOffset()` explicitly.

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

## Minimal Example

```lua
local clay = require("clay")

function layout(dt, mx, my, mouseDown, wheelY)
  clay.beginLayout()

  clay.createElement(clay.id("Root"), {
    layout = {
      layoutDirection = clay.TOP_TO_BOTTOM,
      sizing = { width = clay.sizingGrow(), height = clay.sizingGrow() },
      padding = clay.paddingAll(16), childGap = 8
    }
  }, function()

    -- Scrollable viewport: omit clip.childOffset (wrapper uses Clay_GetScrollOffset)
    clay.createElement(clay.id("ListViewport"), {
      layout = { sizing = { width = clay.sizingGrow(), height = clay.sizingGrow() } },
      clip   = { horizontal = false, vertical = true }
    }, function()
      for i=1,100 do
        clay.createElement(clay.id("Row", i), {
          layout = { sizing = { width = clay.sizingGrow(), height = clay.sizingFixed(28) }, padding = clay.paddingAll(8) },
          backgroundColor = { r=40, g=40, b=60, a=255 }
        }, function()
          clay.createTextElement(("Item %03d"):format(i), { fontId=1, fontSize=16 })
        end)
      end
    end)

  end)

  -- drive input
  clay.setPointerState({x = mx, y = my}, mouseDown)
  clay.updateScrollContainers(true, {x = 0, y = wheelY}, dt)

  -- iterate draw commands
  for cmd in clay.endLayoutIter() do
    local t = cmd:type()           -- integer enum (see constants)
    local id = cmd:id()            -- element id (integer)
    local x,y,w,h = cmd:bounds()   -- numbers
    -- Your renderer: inspect t and call cmd-specific accessors (below).
  end
end
```

## **Open/Configure/Close API (explicit element blocks)**

In addition to createElement(id, config?, childrenFn?), you can drive layout with three explicit calls:
```clay.open(clay.id("MyBox"))        -- begin an element block
clay.configure({
  layout = {
    layoutDirection = clay.TOP_TO_BOTTOM,
    sizing = { width = clay.sizingGrow(), height = clay.sizingFixed(160) },
    padding = clay.paddingAll(12),
    childGap = 8,
  },
  backgroundColor = { r=28, g=32, b=40, a=255 },
  cornerRadius = { topLeft=8, topRight=8, bottomLeft=8, bottomRight=8 },
})
-- (declare children here with more open/configure/close or createTextElement, etc.)
clay.close()                       -- end the element block
```

## Why use it?
- **Clarity & control:** Separates ID, configuration, and the child body. Handy for step-wise config (e.g., conditionally toggling `clip`, `floating`, borders, etc.).
- **Interleave logic:** You can compute sizes or colors between `open` and `configure` based on runtime state.

## Call order rules
1. `clay.open(id)` starts a new element.
2. `clay.configure(tbl?)` is optional and can be called once for that open element.
3. `clay.close()` must be called exactly once to end the element.
4. You may nest elements by calling `open` again before closing the parent; just ensure every `open` has a matching `close`.
5. You cannot call `configure` after `close`, and you cannot `open` a sibling without closing the current element first.
> Tip: If you prefer a single call with inlined children, keep using `createElement`. The explicit API is equivalent, just less overhead.

## Scroll/clip behavior
- Unlike with `createElement`, it should be okay to use `getScrollOffset` with `clip` through `configure`. The issue with `createElement` is because the element is not yet open when calling `getScrollOffset`.

## Mixed usage example
You can freely mix `open/configure/close` with `createTextElement` and other helpers:
```clay.open(clay.id("Card"))
clay.configure({
  layout = {
    layoutDirection = clay.TOP_TO_BOTTOM,
    sizing = { width = clay.sizingPercent(1.0), height = clay.sizingFit() },
    padding = clay.paddingAll(10), childGap = 6,
  },
  backgroundColor = { r=46, g=50, b=62, a=255 },
  cornerRadius = { topLeft=10, topRight=10, bottomLeft=10, bottomRight=10 },
})

  clay.open(clay.id("Header"))
  clay.configure({ layout = { sizing = { width = clay.sizingGrow(), height = clay.sizingFixed(28) } } })
    clay.createTextElement("Title", { fontId=1, fontSize=18 })
  clay.close()

  clay.open(clay.id("Body"))
  clay.configure({
    layout = { sizing = { width = clay.sizingGrow(), height = clay.sizingFit() }, childGap = 4 }
  })
    for i = 1, 3 do
      clay.open(clay.id("Row", i))
      clay.configure({
        layout = { sizing = { width = clay.sizingGrow(), height = clay.sizingFixed(24) }, padding = clay.paddingLTRB(8,4,8,4) },
        backgroundColor = { r=58, g=62, b=74, a=255 },
      })
        clay.createTextElement(("Item %d"):format(i), { fontId=1, fontSize=14 })
      clay.close()
    end
  clay.close()

clay.close()
```

---

## **The Layout End Iterator (robust overview)**

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

## How to set data on an element

```lua
-- image.imageData (for IMAGE commands)
clay.open(clay.id("Avatar"))
clay.configure({
  image = {
    -- Option A: pass a C/FFI pointer (stays lightuserdata)
    imageData = my_texture_ptr,

    -- You can still style the image via backgroundColor/cornerRadius/etc.
  },
})
clay.close()

-- custom.customData (for CUSTOM commands your renderer handles)
clay.open(clay.id("CustomBox"))
clay.configure({
  custom = {
    -- Option B: pass ANY Lua value (table, string, function, etc.)
    customData = { type = "sparkle", intensity = 0.8 },
  },
})
clay.close()

-- top-level userData (available on EVERY command produced by this element)
clay.open(clay.id("Row42"))
clay.configure({
  userData = { row = 42, key = items[42].id },  -- any Lua value OR a lightuserdata
})
  clay.createTextElement(items[42].label, { fontId = 1, fontSize = 14 })
clay.close()
```

- `image.imageData` → appears on `RENDER_IMAGE` commands
- `custom.customData` → appears on `RENDER_CUSTOM` commands
- `userData` (top-level) → copied onto **every** render command emitted for that element (rect, border, text, etc.)

## How to read data during rendering

Inside your draw loop (layout end iterator):

```lua
for cmd in clay.endLayoutIter() do
  local t = cmd:type()

  -- imageData for IMAGE
  if t == clay.RENDER_IMAGE then
    local payload = cmd:imageData()   -- may return lightuserdata OR the original Lua value
    -- NOTE: one-shot; calling cmd:imageData() again this frame returns nil
    draw_image(cmd, payload)
  end

  -- customData for CUSTOM
  if t == clay.RENDER_CUSTOM then
    local payload = cmd:customData()  -- one-shot
    draw_custom(cmd, payload)
  end

  -- userData for any command type
  do
    local u = cmd:userData()          -- one-shot
    if u then use_per_command_user_data(u) end
  end
end
```

### One-shot semantics (important!)
When you **set a non-lightuserdata Lua value**, the wrapper stores it as a **Lua registry reference** behind the scenes. On the **first** call to `cmd:imageData()`, `cmd:customData()`, or `cmd:userData()`:

- The wrapper **restores the original Lua value**,  
- **Unrefs** the registry entry to avoid leaks,  
- **Nulls out** the internal pointer for that command.

So a **second call** in the **same frame** will return `nil`. If you need the value in multiple places, cache it in a local variable on first read.

> If you pass a **lightuserdata pointer**, the wrapper returns that pointer each time (it doesn’t consume it), and you won’t hit the one-shot behavior.

## Choosing between pointer vs Lua value

- **Use lightuserdata** when you already have a native pointer (e.g., a texture handle, shader, mesh). This keeps the hot path zero-allocation and avoids GC.  
- **Use Lua values** when you want to ship structured params to your renderer without building C structs (e.g., `{ kind="badge", color=... }`).

## Per-field behavior summary

| Field on element config | Command accessor        | Applies to command types           | Accepted input                       | Return value in renderer | Notes |
|---|---|---|---|---|---|
| `image.imageData`       | `cmd:imageData()`       | `RENDER_IMAGE`                     | lightuserdata **or** any Lua value   | pointer or original Lua  | Lua value is **one-shot** |
| `custom.customData`     | `cmd:customData()`      | `RENDER_CUSTOM`                    | lightuserdata **or** any Lua value   | pointer or original Lua  | Lua value is **one-shot** |
| `userData` (top-level)  | `cmd:userData()`        | **All commands** from that element | lightuserdata **or** any Lua value   | pointer or original Lua  | Lua value is **one-shot** |

## Practical patterns

- **Batching**: stash a small tag in `userData` (e.g., `{bucket="ui"}`) so your renderer can group commands without extra lookups.  
- **Images**: put your engine’s texture handle in `image.imageData` (lightuserdata) for direct use in the renderer.  
- **Custom draws**: encode draw parameters in `custom.customData` as a Lua table; your renderer reads it on the `RENDER_CUSTOM` and performs the specialized draw.  
- **Avoid accidental consumption**: call each accessor **once** per command and cache the result if multiple systems need it.

## Gotchas & tips

- **Don’t rely on multiple reads** of the same Lua payload in one frame—cache it.  
- **Be mindful of GC**: passing large tables per command can create pressure. Prefer small tables, interned strings, or pointers.  
- **Separate concerns**: use `userData` for per-element metadata; keep heavy per-draw payloads in `imageData`/`customData` only on commands that need them.  
- **Works with both `createElement` and `open/configure/close`** APIs. The behavior is identical.

---

# LuaJIT cdata support (`userData`, `imageData`, `customData`)

When you build the wrapper with **LuaJIT**, you can pass **cdata** (FFI pointers, structs, etc.) into the three payload fields. The wrapper treats cdata as a regular Lua value and preserves it end-to-end.

### What you can pass
- `image.imageData = <cdata or lightuserdata or any Lua value>`
- `custom.customData = <cdata or lightuserdata or any Lua value>`
- `userData = <cdata or lightuserdata or any Lua value>`

### How it behaves
- **cdata counts as a “Lua value” path** (like a table/string/number). Internally it’s stored as a registry ref and **restored exactly** on read.
- Therefore, reading via `cmd:imageData()`, `cmd:customData()`, or `cmd:userData()` is **one-shot per command per frame** (the wrapper unrefs the value and nulls the pointer after the first read).
- If you need the value multiple times, **cache it** the first time you read it.

### Example: pass an FFI pointer
```lua
local ffi = require("ffi")

-- Suppose your renderer created a native texture handle in C
-- and exposed it to Lua as an FFI pointer:
local tex = ffi.cast("void*", my_engine_get_texture("avatar.png"))

clay.open(clay.id("Avatar"))
clay.configure({
  image = {
    imageData = tex,               -- cdata pointer OK
  },
  cornerRadius = { topLeft=8, topRight=8, bottomLeft=8, bottomRight=8 },
})
clay.close()

for cmd in clay.endLayoutIter() do
  if cmd:type() == clay.RENDER_IMAGE then
    local payload = cmd:imageData()   -- returns the SAME cdata you passed
    -- NOTE: one-shot; calling cmd:imageData() again returns nil
    draw_image_with_pointer(cmd, payload)
  end
end
```

### Example: pass a struct with draw params
```lua
local ffi = require("ffi")
ffi.cdef[[
typedef struct { float radius; float softness; } Glow;
]]
local glow = ffi.new("Glow", { radius = 12.0, softness = 0.6 })

clay.open(clay.id("GlowBox"))
clay.configure({
  custom = { customData = glow },   -- cdata struct OK
})
clay.close()

for cmd in clay.endLayoutIter() do
  if cmd:type() == clay.RENDER_CUSTOM then
    local g = cmd:customData()      -- Glow* (cdata) on first call
    if g then render_glow(cmd, g.radius, g.softness) end
  end
end
```

### Choosing cdata vs lightuserdata
- **cdata (LuaJIT FFI)**  
  - Pro: you can pass typed pointers or structs directly; great for rich params.  
  - Con: treated as a Lua value → **one-shot accessor**; also participates in GC like any other Lua object.
- **lightuserdata (plain Lua pointer)**  
  - Pro: **not** consumed on read; the accessor can be called repeatedly without turning `nil`.  
  - Con: untyped; you carry type info out-of-band in your renderer.

> If you want “persistent pointer” semantics (multiple reads, zero GC), prefer **lightuserdata**. If you want rich typed parameters or a convenient struct, use **cdata** and cache on first read.

### Tips & pitfalls
- **Cache once**: multiple systems need the payload? Read it once and reuse the local variable.  
- **Keep it small**: for lists with thousands of commands, prefer a small struct or pointer (avoid huge tables).  
- **Mix & match**: use `userData` for per-element tags (e.g., row index), and `imageData` / `customData` for per-command draw inputs.  
- **Safety**: ensure your renderer doesn’t dereference stale pointers; the wrapper doesn’t retain or copy your native memory.

### Constants exported for convenience
- Render types: `RENDER_RECTANGLE`, `RENDER_BORDER`, `RENDER_TEXT`, `RENDER_IMAGE`, `RENDER_SCISSOR_START`, `RENDER_SCISSOR_END`, `RENDER_CUSTOM`.
- Layout direction: `LEFT_TO_RIGHT`, `TOP_TO_BOTTOM`.
- Alignment: `ALIGN_X_LEFT`, `ALIGN_X_CENTER`, `ALIGN_X_RIGHT`, `ALIGN_Y_TOP`, `ALIGN_Y_CENTER`, `ALIGN_Y_BOTTOM`.
- Text alignment: `TEXT_ALIGN_LEFT`, `TEXT_ALIGN_CENTER`, `TEXT_ALIGN_RIGHT`.
- Text wrap: `TEXT_WRAP_NONE`, `TEXT_WRAP_WORDS`, `TEXT_WRAP_NEWLINES`.
- Sizing kinds: `SIZING_FIT`, `SIZING_GROW`, `SIZING_FIXED`, `SIZING_PERCENT`.
- Floating attach points: a family of `ATTACH_*` / `ATTACH_POINT_*` constants (see code).

### Performance tips
- Do as little work as possible inside the iterator loop—prefer precomputed textures/fonts and batched draw calls by type/z‑index.
- If your renderer needs a flat list first, you can collect commands into arrays by type in a single pass: one iterator, multiple buckets.

---

## Element & Text Declarations (Lua)

- `clay.createElement(id, config?, childrenFn?)`  
  Declares an element with optional `childrenFn`. Key config fields include:
  - `layout = { layoutDirection, sizing = { width = SizingAxis, height = SizingAxis }, padding, childGap, childAlignment, aspectRatio }`
  - `backgroundColor = {r,g,b,a}`
  - `cornerRadius = { topLeft, topRight, bottomLeft, bottomRight }`
  - `clip = { horizontal, vertical, /* childOffset? omit for auto scroll */ }`
  - `border = { color={r,g,b,a}, width={left,right,top,bottom,betweenChildren} }`
  - `floating = { offset={x,y}, expand={width,height}, zIndex, attachPoints, attachTo, clipTo, parentId }`
  - `custom = { customData }`

- `clay.createTextElement(text, textConfig?)`  
  `textConfig` supports `fontId`, `fontSize`, `textColor`, `alignment`, `letterSpacing`, `lineHeight`, `wrapMode`.

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
