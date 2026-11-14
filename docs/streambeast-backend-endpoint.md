# StreamBeast SRT Switcher — Backend Telemetry Endpoint

The OBS plugin supports **Hybrid Mode**:
1. **Local OBS Media Source monitoring** (always works out of the box with zero server calls).
2. **StreamBeast API Telemetry** (`POST https://streambeast.app/api/srt/plugin-status`) for instant offline/recovery detection (even when OBS has *"Close file when inactive"* enabled) and live bitrate / low-signal scene switching.

Because OBS users should not have to re-authenticate with Firebase OAuth inside OBS every hour, the plugin sends the user's StreamBeast OBS Bridge key (`secondary_stream_key`, automatically extracted from their OBS Media Source URL `srt://srt.streambeast.app:8890?streamid=read:<key>`) in a `POST` JSON body so the key never appears in URL query strings or reverse-proxy access logs.

## Express Route for `streambeast.app` (`routes/srt.ts`)

Add the following route to `routes/srt.ts` (which already owns `activeBridgeMonitors`, `startSrtBridgeMonitor`, `stopSrtBridgeMonitor`, and `buildTelemetrySnapshot`):

```ts
/**
 * POST /api/srt/plugin-status
 *
 * Lightweight telemetry endpoint for the StreamBeast OBS SRT Switcher plugin.
 * Authenticates via the user's OBS Bridge key (`secondary_stream_key`) in the
 * JSON body so the key never appears in URL query logs.
 */
router.post("/api/srt/plugin-status", async (req: Request, res: Response) => {
  const rawKey = typeof req.body?.streamKey === "string" ? req.body.streamKey.trim() : "";
  if (!rawKey || rawKey.length < 8 || rawKey.length > 128) {
    return res.status(400).json({ error: "Invalid streamKey" });
  }

  const db = getAdminFirestore();
  if (!db) {
    return res.status(500).json({ error: "Database unavailable" });
  }

  try {
    const snapshot = await db
      .collection("users")
      .where("secondary_stream_key", "==", rawKey)
      .limit(1)
      .get();

    if (snapshot.empty) {
      return res.status(401).json({ error: "Invalid stream key" });
    }

    const userDoc = snapshot.docs[0];
    if (userDoc.data()?.role === "disabled") {
      return res.status(403).json({ error: "Account disabled" });
    }

    const uid = userDoc.id;
    const existing = activeBridgeMonitors.get(uid);
    if (existing && existing.streamKey !== rawKey) {
      await stopSrtBridgeMonitor(uid);
    }

    const active = activeBridgeMonitors.get(uid);
    if (!active) {
      const lastProbe = lastProbeTimestamps.get(uid) || 0;
      if (Date.now() - lastProbe > 8000) {
        lastProbeTimestamps.set(uid, Date.now());
        startSrtBridgeMonitor(uid, rawKey, {
          initialDelayMs: 50,
          markLiveImmediately: false,
        }).catch((err) => {
          console.error(`[OBS Plugin] Probe error for uid=${uid}:`, err);
        });
      }
      return res.json({
        isLive: false,
        startedAt: null,
        telemetry: null,
      });
    }

    return res.json({
      isLive: active.isLive,
      startedAt: active.isLive ? active.startedAt : null,
      telemetry: active.isLive ? buildTelemetrySnapshot(active) : null,
    });
  } catch (err) {
    console.error("[OBS Plugin] Status check error:", err);
    return res.status(500).json({ error: "Failed to check SRT status" });
  }
});
```

### Rate Limiting Note in `server.ts`

In `server.ts`, `srtRouter` is mounted **before** `app.use('/api/', apiLimiter)` so MediaMTX auth hooks aren't rate-limited. Consider applying `apiLimiter` (or a generous per-IP limiter like 120 req/min) specifically to `/api/srt/plugin-status` if needed.
