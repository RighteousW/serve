# serve

A small POSIX C CLI for serving `.serve` projects as **systemd user services**, one instance per port. Requires Linux with `systemd --user` and `python3` or `npm` for the selected server.

## Install

```sh
make
make install-user
```

Make sure `~/.local/bin` is in `PATH`. The installed user-service template calls `~/.local/bin/serve`, so keep the default `PREFIX` unless you also update the template.

## Configure

```ini
title = Local files
description = Local static file server
root = .
server = python-http
exposure = local
port = 8765
```

All six fields are required. `root` is relative to the `.serve` file. Supported servers are `python-http`, `npm-dev` and `npm-preview` (npm modes use Vite-compatible flags). `local` binds to `127.0.0.1`. `lan`, `wan`, and `internet` bind to `0.0.0.0` and **do not** implement firewall, routing, or public exposure rules.

## Use

```sh
serve --discover [directory]
serve example.serve
serve --list
serve --status 8765
serve --pause 8765
serve --restart 8765
serve --stop 8765
```

- Starting a configuration registers its absolute file path in `~/.local/state/serve/` (or `$XDG_STATE_HOME/serve`) and starts `serve@PORT.service` with `systemctl --user`.
- `--list` shows registered services (including failed or inactive units, until stopped), with host IPv4 addresses and ports; `--status` and successful starts show them too. Addresses are derived from the host's current network interfaces each time, so DHCP/network changes are reflected without restarting `serve`.
- `--pause` sends `SIGSTOP` to **all processes** in the service cgroup, preserving already-open listening sockets. Only pause after the server is ready.
- `--restart` resumes a **paused** unit using `SIGCONT`; it does **not** restart a running or stopped unit.
- `--stop` resumes a paused unit if necessary, stops the systemd service and removes its registration. It only stops processes managed by this service. It does not kill unrelated processes on that port.
- Config edits take effect on the next fresh start, not while the process is running.
- Services are not automatically enabled for reboot. User services ordinarily stop when the user manager exits; `loginctl enable-linger "$USER"` (with appropriate authorization) allows the user manager to continue after logout.
- For logs, run `journalctl --user -u serve@8765.service -f`.

Address display: `local` shows `127.0.0.1:PORT`; `lan`, `wan`, and `internet` show loopback plus active non-loopback IPv4 interfaces (including VPN/virtual interfaces). These are **candidate addresses**, not confirmed reachable URLs or public IP addresses. They have no URL scheme because `server=command` may launch a non-HTTP service. Binding, firewall, proxy, NAT, and actual process readiness still determine connectivity. In particular `wan` and `internet` do not automatically discover a router's public address.

Security note: `.serve` files can launch npm scripts. Only register trusted configurations. Python's simple HTTP server is for development and trusted networks, not direct public Internet deployment.
