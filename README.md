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
