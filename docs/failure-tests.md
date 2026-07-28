# LumaHome Failure-Test Matrix

This matrix is for repeatable physical and integration testing. Result boxes
remain intentionally unchecked until each scenario is run against the target
hardware and local network.

| # | Test | Initial state | Failure injected | Expected local behavior | Expected LEDs | Expected backend behavior | Expected event behavior | Recovery behavior | Result |
|---:|---|---|---|---|---|---|---|---|---|
| 1 | Invalid Wi-Fi credentials at boot | Valid persisted config | Build/upload with invalid SSID or password | Restored Manual/Night behavior and Serial remain usable | Red on, Blue off | No heartbeat/report | No synthetic disconnect; local changes queue | Correct credentials and reset/upload; reconnect starts | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 2 | Router unavailable at boot | Valid credentials | Power router off before Arduino boot | Local modes and sensor continue | Red on, Blue off | Device remains offline | Local changes queue; no startup snapshot | Power router on; automatic connection and heartbeat | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 3 | Wi-Fi disconnect while online | Fully online | Disable access point | Local light remains responsive | Red on, Blue off after detection | Last report retained; online becomes false after timeout | One `wifi_changed=false`; later changes queue | Restore AP; reconnect backoff recovers | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 4 | Wi-Fi recovery | Previously disconnected | Restore access point | No reset or mode change | Blue on/Red off after healthy heartbeat | New report received | One `wifi_changed=true`, pending events batched | Queue drains through ACKs | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 5 | Backend unavailable at boot | Wi-Fi available | Backend process stopped | Local behavior continues | Red on, Blue off | No valid report | Wi-Fi connect event and local changes remain pending | Start backend; next scheduled heartbeat recovers | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 6 | Backend stops while Arduino online | Fully online | Stop backend | Manual/Night logic continues | Red on, Blue off after failed heartbeat | Last report remains; online times out | New events remain queued | Restart backend; next heartbeat retries | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 7 | Backend restart | Backend stopped after prior use | Start backend again | No Arduino reset | Blue on/Red off after success | Desired state/events restored from SQLite; runtime report starts fresh | Retries deduplicate | Heartbeat restores report and drains queue | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 8 | TCP connection failure | Wi-Fi connected | Wrong backend host/closed port | Local work continues outside bounded call | Red on, Blue off | No transaction | All events retained | Correct host/port; next heartbeat succeeds | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 9 | HTTP timeout | Wi-Fi connected | Server accepts but withholds response | Local work pauses only within documented 1.5 s bound | Red on, Blue off | May receive request but firmware sees failure | Events retained and resent | Restore timely response; retry/ACK drains | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 10 | Non-2xx heartbeat | Wi-Fi connected | Return 4xx/5xx | No local state mutation | Red on, Blue off | Error response only | No removal | Restore 2xx valid response | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 11 | Invalid JSON | Wi-Fi connected | Return malformed body | Existing mode/light/version preserved | Red on, Blue off | Transaction considered failed by firmware | No ACK processing/removal | Return valid JSON | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 12 | Missing Desired State | Wi-Fi connected | Return 2xx without `desired` | No partial mutation | Red on, Blue off | Firmware rejects response | No removal even if ACK exists | Return complete Desired object | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 13 | Invalid Desired mode | Wi-Fi connected | Return mode outside `manual`/`night` | Current state preserved | Red on, Blue off | Firmware rejects response | No removal | Return valid mode | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 14 | Equal `config_version` | Online at version N | Return version N with different values | Local override remains | Normal online LEDs | Heartbeat valid | Valid ACK may still remove sent events | A version greater than N may update state | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 15 | Stale `config_version` | Online at version N | Return version below N | Current state remains | Normal online LEDs | Heartbeat valid | ACK handled independently | Newer version can update | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 16 | Newer `config_version` | Online at version N | Return valid version N+1 | Desired fields apply atomically; config becomes dirty | Normal online LEDs | Desired snapshot returned | Actual mode/light changes enqueue events | Deferred save persists accepted config | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 17 | Lost response after event storage | Pending events | Drop response after backend commit | No local mode/light change | Backend marked offline after failure | Events stored transactionally | Same identities remain and retry | Backend deduplicates retry and ACKs | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 18 | Null event ACK | Pending events sent | Return `events_ack_seq:null` | Local behavior unaffected | Normal online LEDs for otherwise valid response | Heartbeat accepted | No event removed | Later numeric ACK removes eligible entries | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 19 | Partial ACK | Multiple pending events | ACK an earlier sent sequence | Local behavior unaffected | Normal online LEDs | Response valid | Remove only `seq <= ACK`; retain later entries | Later heartbeat resends remainder | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 20 | Duplicate ACK | ACK already processed | Repeat same ACK in a valid context | Local behavior unaffected | Normal online LEDs | Response valid | Idempotent; no corruption | Normal scheduling continues | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 21 | ACK beyond sent sequence | Pending batch sent | Return ACK greater than request’s highest sequence | Local behavior unaffected | Normal online LEDs if response otherwise valid | Desired response may still be valid | ACK rejected; queue preserved | Correct ACK on later heartbeat | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 22 | More than 32 pending events | Backend unavailable; >32 queued | Restore backend | Local work continues | Recovers after valid heartbeat | Accepts batches of at most 32 | Oldest 32 first; later events remain | Multiple heartbeat/ACK cycles drain queue | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 23 | Ring-buffer overflow | Backend unavailable; 64 pending | Generate additional transitions | Lighting continues without crash | Reflect network health only | No backend activity required | Drop oldest, increment dropped count, retain newest | Restore backend; remaining events sync | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 24 | Reset with pending RAM events | Offline with pending queue | Reset Arduino | Persisted config restores; local modes resume | Startup network-fault state | New boot report later | Old RAM events lost; empty queue; seq=1, new boot ID | New transitions synchronize normally | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 25 | Reset after persistent Desired save | Accepted newer config and `config_dirty=false` | Stop backend and reset | Mode/manual/version restore; Night remains sensor-driven | Red on, Blue off until recovery | No backend required for restoration | Queue starts empty | Backend recovery continues from stored version | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 26 | Night Mode during total outage | Night Mode active | Disable Wi-Fi and backend | Sensor filtering and 250/350 hysteresis continue | Red on, Blue off | Device offline | Light/mode/Wi-Fi events queue within RAM limits | Reconnect and ACK synchronize remaining events | ☐ PASS ☐ FAIL ☐ NOT RUN |
| 27 | Manual Mode during total outage | Manual Mode active | Disable Wi-Fi and backend | Serial `light on/off` immediately controls RGB | Red on, Blue off | Device offline | Actual transitions queue | Reconnect and ACK synchronize remaining events | ☐ PASS ☐ FAIL ☐ NOT RUN |

## Evidence to capture

For each run, record firmware build/version, boot ID, Serial output, backend log,
dashboard screenshot where useful, database event identities, and the observed
result. Never infer a PASS from a build-only validation.
