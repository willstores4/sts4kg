# Test NULL_store on your PS4

## Known test environment

Console testing has been performed on PS4 system software **13.00** with HEN active. The exact HEN/GoldHEN build was not recorded. Other firmware and HEN combinations have not been verified. Catalogue browsing, foreground transfers, and in-app PKG installation have been tried on-console. The Background Service's transfer continuity after closing NULL_store needs further testing across console setups.

Install the latest NULL_store package with HEN active and keep the PS4 awake during transfer checks. [Latest packages](../../releases/latest)

## Test checklist

| Check | What to do | What to look for |
| --- | --- | --- |
| Browse and search | Open the catalogue. Press **Triangle** to search, then **Square** to save a favorite. | Search narrows results and the saved title appears in Favorites. |
| Try a download | Press **R3** for Downloads, then **L3** and choose **Test torrent connection (one piece only)**. | The test stops after about 256 KB. Peer availability varies; no peers may be found on some networks. |
| Resume a transfer | Start an authorized transfer long enough to pause. Pause it, close the app, reopen it, and resume. | The same job remains listed and continues from saved progress. |
| Test Background Service | In Downloads, press **L3** and select **Start / Install service**. Alternatively, open the optional **NULL_store Background Service** app and press **X**. Wait until Downloads shows **Connected**, start a transfer, close NULL_store, wait, then reopen it. Keep the console awake and HEN active. | The transfer should advance while NULL_store is closed. This is the feature that needs the most additional testing across firmware/HEN versions. |
| Install a PKG | Highlight a completed download with a compatible, complete PKG. Press **Triangle**, select the package, and confirm. | The status should reach **Installed**. The PS4 may omit percentages; check Notifications for the system result. |

## Known limitations

- A title can appear more than once when separate catalogue providers list the same game or release.
- The Background Service does not support downloads in rest mode or automatic startup after reboot.
- NULL_store installs compatible complete PKG files; archives and split packages need to be extracted or joined first.

## Report a problem

Include your PS4 firmware and HEN version, the steps you tried, and the exact message shown. On a fresh install, logs are in `/data/null-store/`.

| Problem | Log |
| --- | --- |
| Torrent stays on connecting or fails | `torrent.log` |
| Background Service cannot start | `service-setup.log` and `service.log` |
| PKG installation fails | `installer.log` or `installer-<uid>.log` |
