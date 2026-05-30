# claude-desktop-buddy — M5Stack Fire fork

This is a fork of [`anthropics/claude-desktop-buddy`](https://github.com/anthropics/claude-desktop-buddy) ported to the **M5Stack Fire** (the upstream example targets the M5StickC Plus). Work happens on the `fire-port` branch; `origin` is `rushabh91/claude-desktop-buddy` and `upstream` is the Anthropic original.

## Staying in sync with upstream

At the start of a session, check the upstream repo for new changes that should be brought into this fork:

```bash
git fetch upstream && git log --oneline HEAD..upstream/main
```

If there are upstream commits, review them and merge/cherry-pick the relevant ones into `fire-port` (resolving conflicts against the Fire-specific changes in `include/M5StickCPlus.h`, `platformio.ini`, and `src/main.cpp`).
