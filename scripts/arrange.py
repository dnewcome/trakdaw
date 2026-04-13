"""
arrange.py — Phase 5 test
Generates a 16-bar MIDI arrangement on the Melody track and plays it back.
"""
import daw

print(f"[python] BPM = {daw.bpm()}")
print(f"[python] Tracks: {[t.name for t in daw.tracks()]}")

# Clear any existing timeline clips on both tracks
for t in daw.tracks():
    t.clear_clips()

# --- Bass: 4 bars of quarter notes ---
bass = daw.track("Bass")
clip = bass.new_midi_clip(start_bar=1, length_bars=4)
# Root / fifth / octave pattern
pattern = [36, 36, 43, 36,  # C2 C2 G2 C2
           36, 36, 43, 41,  # C2 C2 G2 F2
           36, 38, 40, 41,  # walk up
           43, 41, 40, 38]  # walk down
for i, pitch in enumerate(pattern):
    clip.add_note(pitch=pitch, start_beat=i, length=0.9, velocity=90)

# Extend 8 more bars with same pattern shifted up a fifth
clip2 = bass.new_midi_clip(start_bar=5, length_bars=8)
for bar in range(2):
    for i, pitch in enumerate(pattern):
        clip2.add_note(pitch=pitch + 7, start_beat=bar * 16 + i,
                       length=0.9, velocity=80)

# --- Melody: pentatonic 16-bar run ---
melody = daw.track("Melody")
pentatonic = [60, 62, 64, 67, 69, 72, 74, 76]  # C major pentatonic x2
mel_clip = melody.new_midi_clip(start_bar=1, length_bars=16)
beat = 0.0
for rep in range(8):
    scale = pentatonic if rep % 2 == 0 else list(reversed(pentatonic))
    for pitch in scale:
        vel = 70 + (rep * 5) % 30
        mel_clip.add_note(pitch=pitch, start_beat=beat, length=0.45, velocity=vel)
        beat += 0.5

print("[python] arrangement written — starting playback")
daw.play()
