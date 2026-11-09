# mini-browser

A minimal web browser in C++: one address/search bar on top, the page below.
Built on GTK 4 and WebKitGTK 6.0.

Pages are shown as **HTML and CSS only** by default. Everything else is opt-in
from the ☰ menu at the top right, and every switch there starts off.

## Features

- **Search or address bar.** Text that looks like an address (`example.com`,
  `https://…`) opens directly; anything else is searched on DuckDuckGo's
  script-free HTML version.
- **Scripts off by default.** `<script>` tags, `on…` handler attributes and
  `javascript:` links are dropped while the page is parsed.
- **Allow scripts (JavaScript)** switch. Flipping it reloads the page.
- **This page loads scripts from:** lists the sites the current page pulls
  script files from, grouped by site (`cdn.foo.com` → `foo.com`), with a
  **Block** button for each. Works with scripts off too, by reading the page's
  original HTML.
- **Block scripts from these addresses:** a blocklist that covers each
  address and its subdomains. Script files from blocked sites are stopped by
  WebKit's content blocker; if the page itself is on a blocked site, all its
  scripts are off. The list is saved to
  `~/.config/mini-browser/blocked-script-domains`.
- **Hardware acceleration (GPU)** switch, off by default because GPU rendering
  crashes on some driver setups. For example, Mesa's nouveau driver crashed
  WebKit on an RTX 3060 Ti on heavy pages (`u_blitter: Caught recursion`);
  the same pages ran fine with NVIDIA's own driver.
- **Crash protection.** If GStreamer has no audio output (`gst-plugins-good`
  missing), audio and video are turned off so pages can't crash the browser,
  and the menu says so. If a page process dies anyway, a short message is
  shown instead of a blank view.

## Building

Arch Linux:

```bash
sudo pacman -S gtk4 webkitgtk-6.0 gstreamer
make
./browser
```

For audio and video, also install `gst-plugins-base gst-plugins-good gst-libav`.

`make run` builds and starts the browser; `make clean` removes the binary.

## Files

- `main.cpp`: the whole browser.
- `Makefile`: builds `browser` with `pkg-config`
  (`gtk4 webkitgtk-6.0 libsoup-3.0 gstreamer-1.0`).
