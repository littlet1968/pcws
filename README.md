# PCWS source port — WS310 and WS610

Version **0.3.1** is an experimental source implementation for the requested
customer display, power LED, GPIO and powered serial controls. It consists of
a DKMS kernel module and a small Python serial helper. It implements **31 of
143 original ioctl commands**. It is not a complete PCWSAPI 1.05A replacement,
and physical workstation validation is still required.

The original `../pcws.ko` and Ghidra exports remain untouched. No original
machine code is linked or executed. Original glyph and board data are recovered
as reviewable JSON with a reproducible extraction tool.

## Implemented and tested

- Customer display text (20x2), bitmap transfer, clear, on/off, contrast,
  backlight, pixel and reversal command framing. WS310 and WS610 have distinct
  primary protocols; the optional secondary display uses the legacy protocol.
- Power LED GPIO on/off and software blinking, with configurable polarity.
- Up to 16 configured GPIO inputs/outputs, plus cash drawers and board revision.
- WS610 motherboard COM1/COM2 voltage selection: 0/5/9/12V.
- WS310 revision D+ motherboard COM voltage selection: 0/5/9/12V.
- WS310 USB-dongle COM7/COM8 voltage control and validated readback, independent
  of motherboard revision, through its separate serial control port.

Tests use **real module ioctls**, Linux gpio-sim and PTYs on the Arch VM's
`7.3.4-arch1-2` kernel. The module also builds with `W=1` against Oracle Linux
`5.14.0-687.66.1.el9_8.x86_64`. A physical WS610 now also builds on kernel 7.2.14; its inventory and register
translation audit are in [the target notes](docs/hardware/tom-micros/README.md).
GPIO electrical state, LED wiring, UART-to-peripheral assignment, voltage
outputs and display responses still require physical validation. See
[VALIDATION.md](docs/VALIDATION.md) and [the command inventory](docs/IOCTLS.md).

## Build and DKMS

Matching prepared kernel headers, GCC and make are required. On a Linux target:

```sh
make -j2 W=1
# Or select a different prepared build tree:
make -j2 W=1 KDIR=/path/to/kernel/build

sudo install -d /usr/src/pcws-0.3.1
sudo cp Makefile dkms.conf pcws_main.c pcws_cashdrawer.c pcws_board.c \
  pcws_serial.c pcws_controls.c /usr/src/pcws-0.3.1/
sudo cp -r include /usr/src/pcws-0.3.1/
sudo dkms add -m pcws -v 0.3.1
sudo dkms build -m pcws -v 0.3.1
sudo dkms install -m pcws -v 0.3.1
```

Remove an older experimental DKMS registration before installing the new build
for the same kernel. Do not overwrite an existing source registration of this
version. Secure Boot machines need their usual module-key enrollment.

For a hardware-free metadata smoke test:

```sh
sudo modprobe pcws test_mode=1
sudo python3 tests/smoke_ioctl.py
sudo modprobe -r pcws
```

The installed WSL `6.18.58.1-microsoft-standard-WSL2` build symlink points to an
include-only directory. That target still needs a complete matching prepared
build tree; the Makefile reports missing Makefile/auto.conf/Module.symvers.

## Physical workstation setup

First collect controller identity and pin-to-line information on each model:

```sh
sudo python3 tools/inspect_target.py > workstation-hardware.json
sh tools/collect_target.sh > workstation-hardware.txt
```

These tools read information; they do not request or drive GPIO lines. Then
verify GPIO chip labels/offsets and the display/dongle TTY identities against
the board evidence in [WS310-WS610.md](docs/WS310-WS610.md).

Load with `model=20` for WS310 or `model=18` for WS610. Add only verified GPIO
parameter groups from [CONTROLS.md](docs/CONTROLS.md). No GPIO is acquired when
its parameter group is absent. Explicitly configured output groups initialize
inactive. The observed WS610 DMI identity is recognized automatically. Automatic
board-to-Linux GPIO output mapping is not enabled.

## Serial helper

The kernel serial bridge retains the old `/dev/pcws` ioctl interface. The helper
owns configured TTYs through supported userspace interfaces and is required for
display and WS310 dongle operations. GPIO/LED/motherboard voltage operations
remain in the kernel and work without the helper.

After loading the selected model, start the helper with **verified** TTY paths:

```sh
sudo python3 userspace/pcws_display.py --model ws310 --port /verified/display/tty
# WS610 uses --model ws610; optional secondary display:
#   --secondary-port /verified/secondary/tty
# WS310 USB-dongle powered ports:
#   --dongle-port /verified/dongle-control/tty
```

A WS310 helper can run with only `--dongle-port` if no display is required.
The old driver's ttyS names are recorded in the model profile; they are not
assumed stable across kernel or controller changes. Opening a configured
primary display runs its on/clear initialization and WS310 EC normal-mode
command. `--no-initialize` skips this setup for diagnostic tests.

The helper claims exclusive TTY access. Stop it with Ctrl-C or SIGTERM **before
unloading the module**. Its open bridge descriptor intentionally pins the module.
There is no installed boot autoload rule or helper service: deployment needs the
verified physical mappings and device paths first.

For a system installation, preserve the helper's relative data layout:

```sh
sudo install -d /usr/lib/pcws/userspace /usr/lib/pcws/data
sudo install -m 755 userspace/pcws_display.py userspace/pcwsctl.py /usr/lib/pcws/userspace/
sudo install -m 644 data/fonts.json /usr/lib/pcws/data/
```

## Operator commands

With the corresponding backends configured:

```sh
sudo python3 userspace/pcwsctl.py display-text 'Hello from PCWS'
sudo python3 userspace/pcwsctl.py display-clear
sudo python3 userspace/pcwsctl.py display-power on
sudo python3 userspace/pcwsctl.py led 1
sudo python3 userspace/pcwsctl.py led-blink 500
sudo python3 userspace/pcwsctl.py gpio
sudo python3 userspace/pcwsctl.py gpio 0 1
sudo python3 userspace/pcwsctl.py com 1 5
sudo python3 userspace/pcwsctl.py com 1
sudo python3 userspace/pcwsctl.py dongle 0 5
sudo python3 userspace/pcwsctl.py dongle 0
```

COM ports use 1/2; WS310 dongle ports use 0/1 (old COM7/COM8). Voltage values
are 0,5,9,12. Settings are runtime-only: this version does not write the old
EEPROM or registry configuration. Voltage readback reports the GPIO selection
or validated dongle response; it does not measure analog voltage.

## Recovery and verification

```sh
python3 tools/recover_ctf.py
python3 tools/recover_data.py
python3 -m unittest discover -s tests -v
# Disposable VM, with pcws unloaded:
sudo python3 tests/gpio_sim.py
sudo python3 tests/controls_vm.py
sudo python3 tests/display_bridge_vm.py
```

Recovery preserves all 143 ioctl constants, 24 model values, 1,872 CTF type
records and 292 variable declarations. CTF here does not map function symbols
to signatures. It includes obsolete kernel types as analysis evidence only;
the source uses current kernel headers. Font extraction recovers 608 glyphs
and three board-control tables, with original ELF size checks.

The metadata version queries retain the legacy `1.05A`/`01 00 05 00` values for
ABI compatibility; actual source version `0.3.1` is shown by `modinfo` and sysfs.
Display ACK contents are not defined by the original driver, so display success
means completed host-side transmission. Dongle power replies are validated.
MSR, RFID, tamper, watchdog, UPS and remaining unlisted commands are unported.

The VM is accessed from WSL using:

```sh
ssh -i /home/tliebsch/.ssh/id_ed25519 -p 2222 tliebsch@localhost
cd ~/pcws-reconstruction
```

The VM last had DKMS 0.3.0 installed and unloaded; its SSH port became unavailable
during the physical-target follow-up. Version 0.3.1 is built on tom-micros for
7.2.14 and 7.3.4, awaiting privileged physical validation. The helper
has not been configured to start at boot. The original module and normalized
analysis binary were never loaded.

Source license: GPL-2.0-only; see COPYING. The original binary reports Dual
BSD/GPL. References: [Kbuild](https://docs.kernel.org/kbuild/modules.html),
[GPIO mappings](https://docs.kernel.org/driver-api/gpio/board.html),
[gpio-sim](https://docs.kernel.org/admin-guide/gpio/gpio-sim.html),
[CTF format](https://sourceware.org/binutils/docs-2.43/ctf-spec.html).
