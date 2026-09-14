# Oscilloscope music on the panel

Oscilloscope music is sound made to be seen. Feed a stereo track to an
oscilloscope in XY mode and the left channel moves the beam across while the
right one moves it up and down: the sound draws pictures, and the pictures
move with the music. Jerobeam Fenderson's album *Oscilloscope Music* is the
owner's example, and the first two clips on the panel came from it
(Blocks, 2:15 to 2:29, and Planets, 1:00 to 1:14).

This directory draws such a track the way a scope would and turns it into a
clip the panel plays. It can be done on a computer with Python, or on a phone
from the panel's own page, with nothing but the panel and the phone.

The audio stays with the owner. It never goes into git: the `.gitignore` here
keeps out `*.wav`, `*.flac`, `*.mp3`, and the `*.pca` and `*.gif` made from
them. The page reads the file on the phone and sends only the finished clip.

## How a track is drawn

`scope_render.py` is the reference, and the page is a port of it that draws
the same pixels (checked below).

- **Every sample pair is a point.** Left is X, right is Y, full scale is the
  edge of a 64 x 64 face. The script lands points on a 4x supersampled grid
  and sums it back to face pixels, so a pixel's value is the number of samples
  that fell in it.
- **Brightness is dwell.** Where the beam moves slowly more samples land, and
  the pixel glows brighter, as on a CRT. Counts are scaled by 48 kHz / rate, so
  a 192 kHz track is as bright as the same drawing at 48 kHz.
- **Afterglow.** Each 40 ms frame keeps 0.55 of the last one's glow and adds
  its own samples.
- **Light.** The glow goes through 1 - exp(-glow / 10), so about 10 samples on a
  pixel reach 63 %. A 3 x 3 bloom of [1, 2, 1] / 4 each way is added at 0.3.
- **The panel's colours.** The result is cut to 16 levels of a fixed green
  palette (PCA1 clips are 4 bits a pixel), with the face centred on 128 x 64,
  at 25 fps.

## On a computer: Python

```
python3 tools/oscmusic/scope_render.py sheet track.wav sheet.png --every 15
python3 tools/oscmusic/scope_render.py clip  track.wav clip.gif --start 135 --dur 14
python3 tools/oscmusic/scope_render.py pca   track.wav clip.pca --start 135 --dur 14
```

- **`sheet`** draws a frame every 15 s, to find the good parts.
- **`clip`** writes a GIF for `tools/gif2pca.py`, which re-quantises its
  palette.
- **`pca`** writes the clip itself, byte for byte what the page makes.

The script reads 16-bit WAV only (numpy and Pillow).

## On the phone: the panel's page

Effects & clips, then **Make a clip**.

1. **Pick a sound.** Any audio or video file the phone has.
   - **A WAV** is read where it lies: the page parses the RIFF header and
     reads only the second of sound it needs with `File.slice()`, so a
     350 MB track at 192 kHz is fine. PCM 16, 24 and 32-bit, float, and
     extensible WAVs (what afconvert writes) are all read.
   - **Anything else** (MP3, FLAC, M4A, the sound of an MP4 or MOV) goes to the
     browser's decoder. That holds the whole file and its decoded sound
     (resampled to 48 kHz) in memory, so above 50 MB the page asks first.
   - **More than two channels:** the first two are drawn.
   - **Mono, or channels that stay within half a pixel of each other** (0.022
     apart: a difference d puts a point 22.6 d pixels off the diagonal): the
     page says it draws only a diagonal line, and lets it be.
2. **Choose the part and the look.**
   - start, on a slider over the track, and length, or the whole track;
   - 25 or 20 fps;
   - phosphor green, amber, blue or white;
   - brightness, persistence and zoom (1.0 to 1.6);
   - the clip's name (1 to 24 of A-Z a-z 0-9 _ -).

   The preview shows the frame at the start, and **Preview 4 s** plays the
   first four seconds. The size line says how many frames and kilobytes the clip
   will be, and how much room there is where it goes. When there is not enough,
   it lists the clips there with Delete.
3. **Make the clip.** The page draws every frame, a second of sound at a time
   so the phone stays responsive, writes PCA1 and uploads it with a progress
   bar and the upload speed. Then **Play** puts it on the panel.

Where the page goes beyond the script, and why:

- **24-bit and float WAVs,** read exactly: 24-bit samples scale without loss,
  so they draw the same frames as 16-bit.
- **A run-in.** Up to 8 frames before the start are drawn and thrown away, so
  the clip's first frame has its afterglow and the loop does not blink. The
  script's clip starts cold.
- **20 fps** keeps the afterglow per second: 0.55 to the power 25/fps per frame.
- **Zoom** widens the face up to 102 pixels, and the height stays 64. It
  multiplies each sample's weight by the zoom, so a line spread over more
  pixels keeps its brightness.
- **Recording** the microphone or another tab is offered only on a secure
  (https) page. Browsers leave `navigator.mediaDevices` undefined elsewhere
  (MDN, getUserMedia "Privacy and security"), and the panel serves plain http.
  Record on the phone instead and pick the file.
- **Streaming services are not a source.** The page does not download from
  YouTube, Spotify or any streaming URL: their terms forbid it, and Spotify's
  audio is DRM-protected. A file the owner has, or records, is the way in.

## Where clips live

- **The panel's flash** (LittleFS, `/api/anim/*`, upstream) holds up to 360
  frames and 1.5 MB, or less as it fills. That is 14.4 s at 25 fps.
- **The TF card** (`CLIPS_SD_ENABLED`, `src/clips/`) holds whole tracks, in
  `/clips`. When a card is in, new clips go there.
  - **Mounting.**
    - SD 1-bit on CLK GPIO1, CMD GPIO44 and D0 GPIO17, at 20 MHz.
    - Mounted on the panel 2026-09-14: a 32 GB SDHC card, FAT.
    - The card must be FAT32: this ESP-IDF is built without exFAT.
  - **The cap: 12000 frames**, 8:00 at 25 fps or 10:00 at 20 fps, 49.2 MB.
    - PCA1's frame count is a u16, so the format allows 65535 frames (43:41,
      268.6 MB) and a whole track needs no extension.
    - FAT32's 4 GiB file limit is far away.
    - What binds is the phone's memory and the upload. The owner's longest
      track, *Deconstruct*, is 7:40.5, 11512 frames. At the 1221 KB/s written
      to the card on the panel, 49.2 MB is about 40 s of writing, plus Wi-Fi.
  - **Playback streams.**
    - A reader task on core 0 keeps the clip open and reads frames in order
      into a ring of 25 frames, one second, in PSRAM.
    - The render loop takes the next frame when its delay is up.
    - Measured on the panel 2026-09-14, a 4096-byte read took 2.59 ms on average
      and 6.44 ms at worst, against 40 ms a frame. So the ring rides out a
      stall of about 150 worst reads, and a slower one holds the frame on screen.
    - At the end of the clip it seeks back to frame 0.
  - **Selecting a clip** in the gallery plays it from the card, and only
    then is it read.

### Checking the card over HTTP

`PANEL` is the panel's address. Make a test clip with `scope_render.py pca`
(above), or with the page.

```
curl -s http://PANEL/api/clips | python3 -m json.tool          # card, cap, clips, stream timing
curl -s -F "anim=@clip.pca" "http://PANEL/api/clips/upload?name=blocks"
curl -s -H 'Content-Type: application/json' -d '{"play":"blocks"}' http://PANEL/api/clips
curl -s http://PANEL/api/clips | python3 -c 'import json,sys; print(json.load(sys.stdin)["stream"])'
curl -s -o frame0.bin "http://PANEL/api/clips/frame?name=blocks&i=0"   # header, palette, frame 0
curl -s -H 'Content-Type: application/json' -d '{"delete":"blocks"}' http://PANEL/api/clips
curl -s -H 'Content-Type: application/json' -d '{"mount":true}' http://PANEL/api/clips  # after putting a card in
```

The `stream` object reports the following, all since the clip was opened:
- `readAvgMs` and `readMaxMs`: the time each frame read took;
- `underruns`: frames that came late and were held;
- `loops`: returns to frame 0;
- `queued`: frames in hand.

## Checks

```
python3 tools/oscmusic/test_clip_maker.py "track.wav@2:15+14" ...
```

It runs on macOS with the system's JavaScriptCore shell, python3 with numpy
and Pillow, and c++. Tracks given as arguments are only read, and everything
it makes goes to a temporary directory.

1. **Frames.** The page's renderer draws exactly the same frames as
   `scope_render.py`: on synthetic signals at 192 and 96 kHz, and on real
   spans of 16-bit and 24-bit tracks with 2, 3 and 4 channels.
2. **Encodings.** 24-bit, float and extensible WAVs draw the same frames as
   16-bit.
3. **Partial reads.** Every slice the WAV reader asks for is logged. It reads
   the header and the span only.
4. **PCA bytes.** The page's writer, gif2pca's `build_pca` and
   `scope_render.py pca` produce identical files.
5. **Validation.** Every clip passes the firmware's own validator, compiled
   for the host. The flash store stops at 360 frames, the card at 12000, and
   the format at 65535.
6. **The card's stream.** It runs with a reader thread and a render thread:
   - with every read taking the measured worst of 6.44 ms, nothing is late and
     frames and delays stay in order across the loop;
   - a 700 ms stall is absorbed;
   - a 2.5 s stall is counted and held, and no frame is skipped.
