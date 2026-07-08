# TinySSH on SmallOS

TinySSH is built from upstream commit `3d93382cde06c109d5d274fa2dddea064e543c85`
as a static SmallOS ELF. The same multi-call binary is staged as:

- `/usr/sbin/tinysshd`
- `/usr/sbin/tinysshd-makekey`
- `/usr/sbin/tinysshd-printkey`

The service is started by default during boot on TCP port `22`. It can also be
started manually with:

```sh
/usr/sbin/tinyssh-start
```

The helper creates `/etc/tinyssh/sshkeydir` on first run by invoking
`tinysshd-makekey -q`, then execs:

```sh
/usr/bin/busybox tcpsvd 0 22 /usr/sbin/tinysshd -v /etc/tinyssh/sshkeydir
```

When QEMU uses user-mode networking, expose the guest SSH port with host
forwarding. The smoke target does this automatically; a manual run can use:

```sh
make run-headless SERIAL_CONSOLE=1 QEMU_NET_HOSTFWD=',hostfwd=tcp::2222-:22'
```

From the host, connect through the forwarded port:

```sh
ssh -tt -o IdentitiesOnly=yes -p 2222 -i .state/tinyssh-smoke-ed25519 root@127.0.0.1
```

On Windows, use the normal OpenSSH path syntax for your private key, for
example:

```bat
ssh -tt -o IdentitiesOnly=yes -p 2222 -i "%USERPROFILE%\.ssh\smallos-tinyssh-ed25519" root@127.0.0.1
```

Authentication uses public keys. The image stages a generated test public key
at `/.ssh/authorized_keys`; its private half is kept on the host as
`.state/tinyssh-smoke-ed25519` for `make tinyssh-smoke`.

To authorize another key, replace or edit `/.ssh/authorized_keys` inside the
guest. TinySSH requires `/.ssh` and `/.ssh/authorized_keys` to be owned by root
or the target uid and not writable by group or others. The image builder
preserves the staged `authorized_keys` file mode as `0600`.

SmallOS currently supports root login only, matching the libc account shim:

- user: `root`
- home: `/`
- shell: `/bin/shell`

TinySSH no-command sessions default to `/bin/shell`, so an interactive login
does not need a remote command argument. Forced-PTY sessions should use `ssh
-tt` so the SmallOS shell or BusyBox `ash` gets a PTY-backed stdin/stdout pair.

The kernel TCP stack does not apply a generic idle timeout to established
streams. It only reaps FIN_WAIT sockets after the close path has been idle, so
an SSH terminal can sit quiet without relying on client keepalives.

`make tinyssh-smoke` boots QEMU with `hostfwd=tcp::<port>-:22`, configures DHCP
inside the guest if needed, verifies root public-key command execution, verifies
a forced-PTY command, then opens the default shell, leaves it idle beyond the
old TCP idle cutoff, sends a command, and exits.

SFTP, forwarding, agent forwarding, X11 forwarding, and multi-user account
expansion are intentionally out of scope for this pass. Password authentication
is not implemented by upstream TinySSH.
