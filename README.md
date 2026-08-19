# smbim OpenWrt feed

This repository is a self-contained OpenWrt custom feed for `smbim`, an
event-driven MBIM data connection manager built on `libmbim-glib`.

The feed provides the `smbim` netifd protocol and daemon, plus an optional
`luci-proto-smbim` package for LuCI protocol configuration.

## Add the feed

Add this line to the OpenWrt buildroot or SDK `feeds.conf`:

```text
src-git smbim https://github.com/pccr10001/smbim.git
```

Update the feeds and install the package metadata:

```sh
./scripts/feeds update -a
./scripts/feeds install -a
```

Enable the recommended C package:

```sh
make defconfig
make menuconfig
# Select Network > WWAN > smbim as <M> or <*>.
# Optionally select LuCI > Protocols > luci-proto-smbim.
make package/feeds/smbim/smbim/compile V=s
make package/feeds/smbim/luci-proto-smbim/compile V=s
```

The result is written below `bin/packages/`. Current OpenWrt releases produce
an `.apk`; older releases produce an `.ipk`.


## Local feed development

Before this repository is pushed, add it as a local feed:

```text
src-link smbim /home/pccr10001/works/smbim
```

Then run the same feed update, install, and compile commands shown above.

## Repository layout

```text
packages/
|-- luci/luci-proto-smbim/ # LuCI protocol definition
`-- net/smbim/             # Native C implementation
    |-- Makefile
    |-- src/
    `-- tests/
```

The native package copies its local `src/` directory into the OpenWrt build
directory, so the feed does not download a second source repository.

## C implementation safety model

The native implementation deliberately keeps mutable modem state on one GLib
main context. MBIM callbacks, command completion, event queue access, and
netifd updates are serialized; the POSIX signal handler only writes a
`volatile sig_atomic_t` shutdown flag.

Additional safeguards include bounded numeric and string validation, character
device verification, direct process spawning without a shell, bounded and
coalesced indication queues, explicit memory ownership, single-attempt PIN
submission, isolating helper processes from authentication environment
variables, wiping credential buffers before release, single-line sanitization of
device-controlled log text, and normalized IPv6 prefix comparison to avoid
unnecessary dynamic-interface rebuilds.

To reconnect the MBIM data session without restarting netifd, send `SIGHUP`
to the daemon:

```sh
kill -HUP "$(pidof smbimd)"
```

The signal handler only sets a `sig_atomic_t` flag. The GLib main loop then
serially performs MBIM deactivate, activate, and IP-configuration refresh. A
changed address or prefix is passed to netifd; an unchanged normalized
configuration leaves the dynamic interfaces intact.

## Host verification

The host needs the `libmbim-glib` development headers. Strict compile and
normalization test:

```sh
cc -std=c11 -D_POSIX_C_SOURCE=200809L \
  -Wall -Wextra -Wconversion -Wshadow -Wformat=2 -Werror -fanalyzer \
  $(pkg-config --cflags mbim-glib gio-2.0) \
  -o /tmp/smbimd-host packages/net/smbim/src/main.c \
  packages/net/smbim/src/mbim.c \
  $(pkg-config --libs mbim-glib gio-2.0)

cc -std=c11 -D_POSIX_C_SOURCE=200809L -O1 -g \
  -fno-omit-frame-pointer -fsanitize=address,undefined \
  -Wall -Wextra -Wconversion -Wshadow -Wformat=2 -Werror \
  $(pkg-config --cflags mbim-glib gio-2.0) \
  -o /tmp/test-smbim-normalization \
  packages/net/smbim/tests/test_normalization.c \
  $(pkg-config --libs mbim-glib gio-2.0)

ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 \
  /tmp/test-smbim-normalization

sh -n packages/net/smbim/src/files/lib/netifd/proto/smbim.sh \
  packages/net/smbim/src/files/usr/libexec/smbim-netifd
```
