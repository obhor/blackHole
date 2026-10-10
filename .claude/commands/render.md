---
description: Render the Gargantua black hole — pick a still image or an orbit video
argument-hint: "[still | video] [frames]"
allowed-tools: Bash, AskUserQuestion, Read
---

Render the Kerr black hole from `kerr.cpp`. One source file, both outputs — do
not create separate files for still vs video.

Arguments given: `$ARGUMENTS`

Do this:

1. **Build** if `kerr` is missing or older than `kerr.cpp`:
   ```bash
   g++ -std=c++17 -O3 -march=native -fopenmp kerr.cpp -o kerr
   ```

2. **Pick the mode.** If `$ARGUMENTS` says "video"/"orbit", do a video. If it says
   "still"/"image", do a still. If it's empty or unclear, ask with AskUserQuestion:
   - *Still image* — one frame, ~80 s, fast to check a change
   - *Orbit video* — camera fly-around, ~20 s/frame, ~1.5 h for 300 frames

3. **Still:**
   ```bash
   ./kerr                      # -> kerr_hamiltonian.ppm
   ```
   Convert to PNG:
   ```bash
   python3 -c "from PIL import Image; Image.open('kerr_hamiltonian.ppm').save('kerr_render.png')"
   ```
   (or `ffmpeg -i kerr_hamiltonian.ppm kerr_render.png` if PIL isn't around)

4. **Video** (default 300 frames, or the number in `$ARGUMENTS`):
   ```bash
   ./kerr orbit <frames>       # -> frames/f_%04d.ppm
   ffmpeg -framerate 30 -i frames/f_%04d.ppm -vf "hflip,vflip" \
          -c:v libx264 -crf 18 -pix_fmt yuv420p -movflags +faststart \
          -y gargantua_orbit.mp4
   ```
   The `hflip,vflip` is the required 180° rotation; the camera renders
   upside-down without it. Delete `frames/` afterward (it's hundreds of MB).

5. **Show the result.** For a still, Read the PNG so the user sees it. Report the
   output path and the wall-clock time.
