# External green-tint preview (proposed v1 protocol)

This is an **unofficial contribution build**, based on **Glass Cockpit DLSS Fix by IFreemz**:
https://github.com/IFreemz/msfs2024-glass-cockpit-dlss-fix. It is not part of the published v1.0.1 DLL.
The code remains GPL-3.0-only with the additional terms in `NOTICE.md`.

An installer/settings window can control the temporary green coverage tint without sending keyboard shortcuts,
loading the engine into its own process, injecting code, changing the INI, or opening the original settings window.
The native window, ReShade tab and Ctrl+Shift+F11 continue to use the same tint state. No author defaults change.

## Contract

For the **simulator's PID**, use these Windows kernel-object names (decimal PID, no leading zeroes):

| Name | Kind | Lifetime and meaning |
|---|---|---|
| `Local\GlassCockpitDlssFix.Tint.v1.<PID>.Ready` | Manual-reset event | Engine creates and holds it, signalled, when its worker supports this protocol. Probe with `OpenEvent(SYNCHRONIZE)`; never create it in the client. |
| `Local\GlassCockpitDlssFix.Tint.v1.<PID>.Owner` | Mutex | Controller creates/acquires it and its UI thread keeps ownership throughout the preview session. Only one controller owns a session. |
| `Local\GlassCockpitDlssFix.Tint.v1.<PID>.On` | Manual-reset event | Controller creates it initially nonsignalled. Signalled = tint on, nonsignalled = off. Keep the handle until detaching. |

Use non-inheritable handles, the default Windows security descriptor, and the same logged-in user/session as the
simulator. There is no global object, permissive custom ACL, elevated helper, arbitrary command or file path. Access
failure means the external control is unavailable; do not relax permissions or enable it by guessing a DLL version.
The PID identifies the target: do not broadcast to all games. Also observe the simulator process lifetime; on exit,
close the client handles and rediscover the process/capability before reconnecting (PIDs can be reused).

1. Open `Ready` and check that it is signalled. If absent, hide/disable the external tint checkbox. Older DLLs keep
   working normally through their own controls. Close this probe handle after checking.
2. Create/open `Owner` and acquire it on the UI thread with a zero timeout. `WAIT_OBJECT_0` or `WAIT_ABANDONED` is
   success; `WAIT_TIMEOUT` means another controller owns it. **Keep it owned**, rather than releasing after each edit.
3. Create/open `On`, reset it, and hold its handle. If creation fails, release/close `Owner` and leave the control disabled.
4. Set/reset `On` for checkbox edits. Poll its signalled state to reflect changes made in the native window/hotkey.
   Do not echo a polled state back as a new user command. Concurrent edits are last-writer-wins.
5. On closing the settings window, reset and close `On`, then release and close `Owner` on its owning thread.

The engine polls at its existing ~33 ms housekeeping interval. It does not wait on the controller or GPU and does
not hold the controller's handles between polls. It only reads `On` while another thread owns `Owner`. Once it has
observed a controller, loss/release/abandonment of that ownership clears the tint, including abnormal controller
termination. Native controls still work afterwards. A hung controller that keeps its owner thread alive is not an
exit: closing/terminating that controller ends its preview. State is never saved in the INI.

The engine serializes its own polling and native toggle operations. A native toggle also sets/resets `On` when a
controller is attached, so native **Off** is effective; this is not an OR of two independent tint requests.

## Tests

`tests/external_tint.cpp` uses real child processes and the production protocol implementation. It checks capability
discovery, on/off in both directions, a second controller being refused, PID isolation, clean shutdown, forced
termination, reconnect, and native controls after detachment. `scripts/build.ps1 -Test` builds it;
`scripts/test.ps1` runs it. No MSFS installation is used.
