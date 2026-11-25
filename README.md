# nautilusctl

Linux controller for the **Corsair Nautilus LCD Cap** (`1b1c:0c57`), written in C from scratch.
It talks directly to the device through `/dev/hidraw`, with no iCUE and no OpenLinkHub.

![nautilusctl window](docs/screenshot.png)

## Features

- Show images and animated GIFs (JPG, PNG, GIF) on the round 480x480 LCD
- CPU temperature screen with any installed font, custom text color and optional label,
  over a library image/GIF or a solid color
- Brightness and rotation control
- Background service (`systemd --user`) that keeps the screen fed, restores the last state on
  login and reconnects after USB resets or suspend
- Minimal dark GTK 4 window and a scriptable command line, both talking to the service

**Tested on:** Nautilus LCD Cap `1b1c:0c57`, firmware 0.3.0.5, Arch Linux, Intel i9-12900K
(`coretemp`). Product ID `0c55` is accepted but untested.

> This project is not affiliated with or endorsed by Corsair. It is provided as-is, without
> warranty; use it at your own risk.

## Architecture

```
nautilusctld  (systemd --user service; starts at login; SOLE owner of the LCD)
  │ player.c   thread that keeps resending frames and reconnects if USB drops
  │ nautilus.c LCD protocol   →  hid.c  write()/ioctl() on /dev/hidrawN
  │ image.c    JPG/PNG/GIF → 480x480 JPEG
  │ render.c   CPU temperature screen (Cairo + Pango)
  ▲
  │ Unix socket $XDG_RUNTIME_DIR/nautilusctl.sock (0600), text protocol
  │
  ├── nautilusctl-gui  GTK 4 window (remote control; closing it doesn't affect the LCD)
  └── nautilusctl      command line (also a remote control)
```

**Why a service exists:** the LCD firmware falls back to the Corsair default screen when it
stops receiving frames. This was verified: closing the window, back when the window was the one
sending frames, made the image disappear. A process has to keep feeding the screen at all times,
and that job doesn't belong to a window.

| File             | Responsibility                                                        |
|------------------|-----------------------------------------------------------------------|
| `src/hid.*`      | find the hidraw node via sysfs, `write()` and feature reports         |
| `src/nautilus.*` | packet format: frame, brightness, rotation, firmware                  |
| `src/image.*`    | decode JPG/PNG/GIF, resize (bilinear, "cover"), encode JPEG           |
| `src/sensors.*`  | CPU temperature via hwmon (`coretemp` "Package id 0" / `k10temp` "Tctl") |
| `src/render.*`   | temperature screen drawn with Cairo + Pango (any installed font)      |
| `src/player.*`   | continuous sending thread, reconnection, device mutex                 |
| `src/daemon.c`   | service: socket (GThreadedSocketService), commands, config            |
| `src/ipc.*`      | socket client (used by the GUI and the CLI)                           |
| `src/gui.c`      | GTK 4 window with its own dark theme                                  |
| `src/main.c`     | command line                                                          |

### Socket protocol

Each message is one line of text. The service replies `OK ...` or `ERR message`.

```
PING | STATUS | SHOW /absolute/path
CPU font=..<TAB>fg=#..<TAB>bg=#..<TAB>label=..<TAB>image=..
BRIGHTNESS 0-100 | ROTATION 0-3 | STOP | HARDWARE | INFO
```

State is saved in `~/.config/nautilusctl/config.ini` and restored when the service starts.

## LCD protocol

**From the device itself:** the HID report descriptor declares output report `0x02` with 1 + 1023
bytes (frames) and feature reports `0x03` to `0x16` with 1 + 31 bytes (commands).

**From the open source OpenLinkHub driver** (studied, not copied), the meaning of the bytes:

```
Frame:    02 05 00 <last?> <index> 00 <len lo> <len hi> <up to 1016 bytes of JPEG>
Commands: 03 0b <0..100> 01 (brightness) · 03 0c <0..3> 01 (rotation)
          03 1e 01 01 + 03 1d 00 01 (back to hardware mode)
Firmware: GET feature report 0x05, ASCII version in bytes 6..12
```

## CPU temperature screen

- `cpu` mode: every second the service reads the temperature and re-renders the screen only if
  it changed. `render.c` draws it at 480x480 and it goes through the same JPEG pipeline as images.
- Customizable: font (any fontconfig family or style), text color, background color and an
  optional label. The temperature is always shown. The style is saved in the `[cpu]` section of
  `config.ini`.
- **In the window:** the background is whatever is selected in the library. The switch on the
  **CPU TEMPERATURE** card turns the temperature on or off over the selected image. The color
  button next to **Default screen** puts the temperature over a solid color; in that mode the
  switch is locked on, because a solid color without the temperature is not an option.
- **Background image** (`image=` in the CLI): any library item, animated GIFs included. The text is
  drawn on every frame, with a radial darkening in the center and a shadow for legibility.
  Measured cost: about 8 ms per frame, i.e. 0.7 s for a 92-frame GIF. So with an **animated**
  background the screen is re-rendered at most every 10 s. With a static image or a solid color,
  it is re-rendered on every temperature change.
- The window preview is drawn by the same `render.c`, so it shows exactly what goes to the LCD.
- Detail: names like "Sans" or "JetBrains Mono" are aliases that fontconfig approximates. GTK's
  font button only knows real families, so the window loads the font and uses the name Pango
  reports (`pango_font_describe`).
- Tested with ASan: 3 renders and 300 renders leave the same 287 KB "leaked", which is the fixed
  fontconfig/Pango cache. There is no per-render leak.

## Install

Dependencies: a C11 compiler, `make`, `pkg-config`, libjpeg-turbo, libpng, giflib, GLib/GIO,
Pango/Cairo and GTK 4.

```bash
sudo pacman -S --needed base-devel libjpeg-turbo libpng giflib gtk4
make install                                  # ~/.local/bin, menu entry, icon, systemd unit
systemctl --user enable --now nautilusctld    # start now and on every login

# /dev/hidraw permission (once)
sudo install -Dm644 udev/70-nautilusctl.rules /etc/udev/rules.d/70-nautilusctl.rules
sudo udevadm control --reload-rules && sudo udevadm trigger --action=add --subsystem-match=hidraw
```

The udev rule uses `TAG+="uaccess"`, which grants access to the user of the active local session.

## Usage

- **Window:** menu → **Nautilus LCD**, or `nautilusctl-gui`.
- **Terminal:**

```bash
nautilusctl status
nautilusctl show ~/Pictures/anim.gif
nautilusctl cpu font="JetBrains Mono Bold" fg=#5aa9ff bg=#000000 label=CPU
nautilusctl cpu image=anim.gif           # library background (image= clears it)
nautilusctl brightness 70
nautilusctl rotation 2
nautilusctl stop | hardware | info
nautilusctl convert anim.gif /tmp/x     # preview the frames without the LCD
```

- **Service:** `systemctl --user status|restart|stop nautilusctld` and `journalctl --user -u nautilusctld`.

## Tests performed

- Builds with no warnings (`-Wall -Wextra`).
- Service and CLI run under AddressSanitizer and UBSan through every command, validation errors,
  raw protocol, 5 simultaneous clients, an attempt to start a second service and SIGTERM
  shutdown: no memory errors and no leaks of our own (the only one found was fixed).
- Running with a 79-frame GIF: under 1% CPU and about 15 MB of RAM.
- The window was closed while a GIF was showing, and the service kept sending frames.

## License

Copyright (C) 2026 lu

This program is free software: you can redistribute it and/or modify it under the terms of the
GNU General Public License as published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version. See [LICENSE](LICENSE).

The LCD packet format was learned by studying the open source
[OpenLinkHub](https://github.com/jurkovic-nikola/OpenLinkHub) driver (GPL-3.0); no code was copied.
