# OpenBOR Online relay on Cloudflare

A Cloudflare Worker with one Durable Object (`Hub`) that does exactly what `relay/relay.js` does (room codes, the public room list, ready-up, the automatic start when everyone is ready, and the ordered fan-out of each player's button presses) but over WebSocket, so phones and PCs reach it at `wss://<your-worker>.workers.dev/ws`. Free plan is enough.

## Set it up (no terminal; everything is in the browser)

1. **Cloudflare account ID.** dash.cloudflare.com > Workers & Pages. The Account ID is in the right-hand column.
2. **API token.** My Profile > API Tokens > Create Token > use the **Edit Cloudflare Workers** template > Create. Copy the token.
3. **GitHub secrets.** In this repo: Settings > Secrets and variables > Actions > New repository secret. Add `CLOUDFLARE_API_TOKEN` and `CLOUDFLARE_ACCOUNT_ID`.
4. **Deploy.** Actions tab > *Deploy Cloudflare relay* > Run workflow. When it finishes, the run summary shows `Relay address: wss://openbor-online-relay.<you>.workers.dev/ws`.
5. **Put the address in the app.** Settings > Secrets and variables > Actions > **Variables** > New: `RELAY_URL` = that `wss://.../ws` address. Then run *Build Android APK* again; the address is baked into the APK, so players type nothing.

Players can also override it with a `netplay.ini` containing `server=wss://your-worker.workers.dev/ws` (Android: in the app's files folder; PC: next to the game), or the `OPENBOR_RELAY` environment variable. Plain `server=host:7777` still talks to a Node `relay/relay.js`.

## Notes

- All rooms live in one Durable Object, so room codes work between any two players in the world. The cost is that the Durable Object sits in one region; on the other side of the planet add roughly one extra hop of latency (the game's 4-frame input delay absorbs about 80 ms).
- The connection is encrypted (TLS), but the game does not check the server certificate (phones have no CA store the engine can rely on). Only button presses, names and room codes travel through it.
- Test locally: `cd cloudflare && npm install && npx wrangler dev`, then run the game with `OPENBOR_RELAY=ws://127.0.0.1:8787/ws`.
