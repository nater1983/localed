# Slackware integration checks

These checks exercise the installed daemon against the real Slackware system
D-Bus and Polkit services. Run them from a maintenance window or test VM.
The bus-restart script uses a private bus and does not restart the host bus.

## 1. Real pending Polkit authorization and daemon shutdown

This check requires a logged-in, active local session, `pkttyagent`, and a
test account that is **not** in the configured privileged group (normally
`wheel`). The installed Polkit rule intentionally grants members of that
group authorization without prompting.

1. Confirm the test account is not in `wheel`:

   ```sh
   id testuser
   ```

2. In terminal A, start a text authentication agent for this login session:

   ```sh
   pkttyagent --process "$$" &
   agent_pid=$!
   trap 'kill "$agent_pid" 2>/dev/null || :' EXIT
   ```

3. Still in terminal A, call SetLocale interactively as the non-wheel account.
   Use a valid test locale value. Do not authenticate yet:

   ```sh
   gdbus call --system \
     --dest org.freedesktop.locale1 \
     --object-path /org/freedesktop/locale1 \
     --method org.freedesktop.locale1.SetLocale \
     "['LANG=fr_FR.UTF-8']" true
   ```

   The call must remain pending and the Polkit agent must display an
   authentication prompt. If it returns immediately, stop: the test account
   was authorized without interaction, or the agent/session setup is wrong.

4. In terminal B, identify the test daemon PID and terminate it while the
   prompt is still open:

   ```sh
   pgrep -a localed
   kill -TERM "$(pgrep -o -x localed)"
   ```

5. The D-Bus call must finish with a disconnect/error, not success. The
   locale file must remain unchanged. Start localed again using the normal
   Slackware service mechanism and verify it owns
   `org.freedesktop.locale1`.

6. Repeat the call, this time cancel at the Polkit prompt (Ctrl+C or the
   agent's cancel action). The operation must fail and the locale file must
   remain unchanged.

Do not run this against a production desktop session if terminating localed
would disrupt active administration. Back up the locale file before testing.
This specifically tests cancellation caused by daemon shutdown; step 6 tests
the agent/user cancellation path.

## 2. Real system-bus name reacquisition and AccountsService reconnection

The following harness launches localed against a **private** D-Bus daemon,
kills and restarts that bus, then verifies both the locale1 name and the
AccountsService name watch are re-established. It does not touch the host
system bus or installed configuration files.

Build localed with tests enabled, then run:

```sh
sh tests/test-slackware-dbus-restart.sh "$PWD/build/src/localed"
```

Expected final lines:

```
PASS: locale1 name reacquired after system-bus restart
PASS: AccountsService connection and name watch re-established
```

The test requires `dbus-daemon` and `gdbus`. It starts localed with
`--debug` and captures its output in a temporary directory.

## 3. AccountsService reconnection on the actual Slackware system bus

The private-bus harness above verifies the same connection/watch logic in a
controlled environment. To verify the installed AccountsService daemon on
Slackware without restarting the system bus:

1. Start localed with debug logging enabled (or temporarily enable
   `daemon.debug` in syslog).
2. Confirm the log reports `subscribed to AccountsService`.
3. Restart AccountsService using the service manager appropriate to the
   installed package. Do not restart D-Bus.
4. Confirm localed logs `AccountsService disappeared` followed by
   `subscribed to AccountsService`.
5. Change the Language property of a test user who is in the configured
   privileged group and confirm the locale bridge processes the update.
   Restore the original locale after the check.

Do not infer successful reconnection solely from the daemon remaining alive:
the second `subscribed to AccountsService` message is the observable
confirmation that the name watch fired and a PropertiesChanged subscription
was installed again.
