# pspkit-sidecar

**Use a PSP as a second Linux monitor over USB.** Sidecar gives your desktop a 480 × 272 display. It can also run alongside PSPLink so you can keep using `pspsh` and `host0:` on the same cable.

Tested on **PSP-1000** and **PSP-2000**. Press **SELECT** on the PSP to adjust brightness and idle dimming.

## Linux

You need a USB data cable, a PSP with custom firmware that supports kernel modules, and a Linux kernel with the `gud` driver. From the repository root, build both PSP files with the pspdev toolchain on your `PATH`.

```sh
make -C psp && make -C psp/app
```

**Start from the PSP menu.** Copy `psp/app/EBOOT.PBP` and `psp/sidecar.prx` into `ms0:/PSP/GAME/SIDECAR/`. Launch **Sidecar**, connect the USB cable and enable the 480 × 272 output in your desktop's display settings. **HOME** exits Sidecar.

<details>
<summary><b>Run alongside PSPLink</b></summary>

Start PSPLink on the console. Install `usbhostfs_pc` and `pspsh` on the host. The udev rule binds the PSP's display interface to GUD and gives the active desktop user access to its framebuffer.

```sh
sudo install -m 0644 linux/51-pspkit-sidecar.rules /etc/udev/rules.d/
sudo udevadm control --reload
```

Reconnect the PSP, then use the scripts from the repository root.

```sh
./tray/bench.sh start    # start or reuse usbhostfs and load the display
./sidecar.sh status      # check PSPLink and the display driver
./tray/bench.sh stop     # unload the display and keep PSPLink running
```

On Hyprland, Sidecar assigns the display its own workspace. If Hyprland skips the physical USB output, Sidecar streams a virtual output to the PSP using `wf-recorder`, Python 3 and a systemd user service. Other desktops can arrange the physical output if their compositor supports USB displays.

</details>

## Development

The [USB diagnostic](tests/gudtest.c) sends test patterns and measures throughput. The [DRM diagnostic](tests/primetest.c) checks framebuffer import and scanout. Both include build instructions. An optional [Aquamarine patch](linux/aquamarine-display-only-usb.patch) adds native handling for display-only USB devices.

Sidecar uses Noralf Trønnes's [GUD protocol](https://github.com/notro/gud) and USB code modelled on [PSPLink](https://github.com/pspdev/psplinkusb). Related projects include [PSPdisp](https://www.jjs.at/software/pspdisp.html) and [pspdisp-linux](https://github.com/ImNoammm/pspdisp-linux). See the [MIT license](LICENSE) and [PSPLink notice](THIRD_PARTY_PSPLINK_LICENSE).
