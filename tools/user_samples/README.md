# User samples

Drop your own WAV files here, one sub-folder per chip:

```
tools/user_samples/nes/*.wav
tools/user_samples/snes/*.wav
tools/user_samples/genesis/*.wav
```

`python tools\gen_samples.py` reads them (8/16/24/32-bit PCM, mono or multi-channel),
mixes to mono, resamples to the chip rate (NES 33144 Hz, SNES 32000 Hz, Genesis
22050 Hz), removes DC, normalises to -1 dBFS and writes them to
`assets/samples/<chip>/<name>.wav`, where `<name>` is the file stem lowercased with
non-alphanumeric characters replaced by `_`. They are listed in
`assets/samples/index.json` with category `user`. A user file whose name matches a
built-in sample replaces it. Another folder with the same layout can be used with
`--user-dir DIR`.
