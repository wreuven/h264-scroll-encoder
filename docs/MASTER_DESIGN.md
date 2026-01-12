# Specialized UI-Aware Hybrid Encoder (Scrolling UI + Small Dynamic Region)

## 1) Goal

Efficiently encode a **Netflix-like UI video stream** where most pixels are:

* **deterministic motion** (primarily vertical scrolling of thumbnail grids; occasional horizontal scrolling of a single row)
* **static UI chrome**
* **small dynamic content** (e.g., a preview video playing inside a rectangle)

Avoid full-frame pixel encoding by:

1. encoding UI motion as **inter prediction** (motion-compensated prediction from reference frames)
2. encoding only a **small dynamic region** with a conventional encoder
3. **stitching** the dynamic encoded region into the full-frame bitstream at the correct location

In practice, this is **UI-aware composition at the bitstream level**.

---

## 2) Core idea

Each output frame is composed of two classes of regions:

### A) Motion-only regions (UI scroll / thumbnail movement)

These regions do not require pixel re-encoding. We emit **inter-coded** macroblocks (often **SKIP**) where:

* The block is **predicted from a reference frame** (inter prediction)
* The **motion vector(s)** point to the location of the corresponding thumbnails/UI in the reference frame
* The **residual** is ideally zero (or near-zero)

> Note: **Inter-coded** is the coding type/mode. A **motion vector** is a parameter inside inter coding that says where to fetch the prediction from.

### B) Dynamic region (preview video etc.)

A bounded rectangle contains genuinely changing pixels.

* Crop this rectangle (plus margin) and encode it with a conventional encoder (**Dynamic Encoder**)
* Insert (“splice”) the resulting encoded macroblocks/slices into the final frame at the correct macroblock addresses

---

## 3) Assumptions and constraints

### Output stream

* Example base resolution: **1280×720** (720p)
* Block structure: assume H.264 with **16×16 macroblocks** (extendable to HEVC CTUs, etc.)

### Dynamic region size bound

* UI guarantees dynamic content never exceeds **360×360**
* Include margin around dynamic content: **+16 px on each side** (configurable)

  * Dynamic encoder input max: **(360+32)×(360+32) = 392×392**

### UI hinting

The application provides per-frame hints describing:

* which rectangles are motion-only and their displacement vectors (or source rectangles)
* where the dynamic region rectangle is located
* (optional) occlusion/layering order if needed

---

## 4) Architecture

There are effectively two encoders:

### 4.1 Composer Encoder (Primary Encoder)

Produces the final full-frame encoded stream, but avoids full-frame pixel encoding.

Responsibilities:

* Consume UI hints and emit **inter-coded** macroblocks for motion-only regions
* Splice in **pre-encoded** dynamic-region macroblocks/slices
* Maintain and select reference frames
* Output a valid H.264 stream (SPS/PPS/IDR/P-slices, etc.)

### 4.2 Dynamic Encoder (Secondary Encoder)

Conventional encoder used only for the dynamic rectangle.

Responsibilities:

* Encode cropped dynamic region at bounded resolution
* Output macroblocks/slices aligned with the region so they can be inserted

### 4.3 Reference/Thumbnail Encoder (Optional)

Encodes “reference UI frames” (thumbnail grids / UI atlases).

In a test harness:

* Use **two** static reference frames (RefA/RefB) representing thumbnail screens
* Scroll is achieved by referencing those screens via motion vectors

---

## 5) Per-frame data flow

### Inputs

* Reference UI frame(s) (thumbnail screens / atlas)
* Dynamic-region pixels for frame N (cropped from renderer)
* Hint metadata for frame N:

  * `motion_regions[]`: rectangles + displacement or source rects
  * `dynamic_rect`: rectangle location in full frame
  * optional: z-order / occlusion rules

### Output

* Full-frame encoded H.264 frame N

---

## 6) Composition algorithm (macroblock stitching)

Traverse output macroblocks in scan order (left→right, top→bottom).

For each macroblock MB(x,y):

1. Determine region classification:

   * motion-only region?
   * dynamic region?
   * static background region?
2. Emit macroblock accordingly:

### 6.1 Motion-only macroblock emission

* Emit **inter-coded** macroblock (often **SKIP**)
* Set reference index (which ref frame)
* Set motion vector(s) (dx,dy) per partition as needed
* Residual ideally zero
* Ensure referenced pixels exist (edge padding rules)

### 6.2 Dynamic macroblock insertion

* Take corresponding encoded macroblock/slice payload from dynamic encoder output
* Insert into output stream at this macroblock location
* Preserve correct macroblock addressing / slice structure

---

## 7) Dynamic region handling

### 7.1 Rectangle alignment

Align dynamic rect to macroblock boundaries (simplifies stitching).

Given `dynamic_rect` and `margin`:

* Expand by margin
* Align to 16-pixel boundaries:

  * `x0 = floor((x - margin)/16)*16`
  * `y0 = floor((y - margin)/16)*16`
  * `x1 = ceil((x+w+margin)/16)*16`
  * `y1 = ceil((y+h+margin)/16)*16`

### 7.2 Encoding format (embed-friendly)

Options:

* Encode dynamic region as slice(s) whose macroblock addresses map directly into the full frame
* Or encode in its own coordinate system and transplant macroblock payloads while rewriting addresses (harder)

---

## 8) Reference frame strategy

### Test mode (simplified)

* Create 2 reference screens:

  * RefA: thumbnail grid layout A
  * RefB: thumbnail grid layout B
* Keep them available as reference frames
* Motion-only regions reference them via motion vectors

### Production generalization

* Maintain a long-lived **UI atlas reference**
* Update the atlas only when UI assets change
* Treat scroll as motion over the atlas

---

## 9) Why it saves resources

Eliminates most of:

* motion search
* mode decision over the full frame
* transforms/quantization for largely static/moved UI

Complexity scales roughly with:

* `area(dynamic_region) / area(full_frame)`

Example:

* 1280×720 ≈ 921,600 px
* 392×392 ≈ 153,664 px (~16.7%) worst case

---

## 10) Edge cases / implementation notes

* **Occlusion:** dynamic region overrides motion regions underneath
* **Margins:** reduce boundary artifacts and handle adjacent UI elements
* **Slice boundaries:** easiest if dynamic region maps to whole slices or MB-row-aligned slices
* **Deblocking across boundary:** trade-off between simplicity and artifacts
* **Rate control:** dynamic encoder does RC; outside region mostly SKIP
* **Fallback:** if hints missing/inconsistent → full conventional encode
