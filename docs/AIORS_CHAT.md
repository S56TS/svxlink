# AI voice for AIORS / SvxLink

`ChatLogic` is an optional linked logic core, independent of the active module.
It supports RF alone, RF with ModuleFrn active, and audio from linked reflector
logics. A separate local Python service performs wake detection and calls the
OpenAI Realtime API. It does not automate the ChatGPT app.

## Radio operation

| Control | Action |
| --- | --- |
| `981#` | Start listening; release PTT and wait for the short ready tone. |
| `980#` | End the conversation and discard pending AI playback. |
| `982#` | Clear conversation history; remain active if already active. |
| “Hey Jarvis” | Activate by voice; say the phrase and question in one transmission, then release PTT. |

After activation, subsequent transmissions on the linked network are questions
until stopped or idle for 45 seconds. There is one **shared conversation**, not
separate histories for each callsign. End it with `980#` before returning to a
normal QSO. A low error tone returns the assistant to standby after a failed
request. With the helper unavailable, commands are logged but no tone is played.

Voice activation uses the supplied English `hey_jarvis` model; no enrollment of
your voice is required. It is not speaker authentication. Threshold 0.6 is a
starting point; test with actual RF audio, accents, FRN compression and noise.
Higher thresholds reduce accidental activations but can miss more calls.
DTMF remains available if wake inference or its model fails.

Send a DTMF command as its own transmission. RF uses SvxLink's existing command
collector, so normal terminators, squelch-close processing and any configured
command timeout still apply. Only the three exact commands above are consumed.
Bare `#`, module selection and unrelated commands retain the existing callback
and its return value. Reserve these numbers against site commands and module IDs.

For FRN/reflector audio, ChatLogic decodes the three commands from in-band tones;
use a trailing `#`, clear tones and less than three seconds between digits.
Network DTMF commands take effect at the end of that transmission. They are not
forwarded to the radio's general command handler by ChatLogic. Codecs or other
nodes may strip tones; voice activation is another option in that case.

## Audio routing

```text
RF RX ------- RepeaterLogic ------- RF TX
                    |  |
              active ModuleFrn ------- FRN server
                    |
             SvxLink logic links ------ ReflectorLogic
                    |
                ChatLogic
                    |
          127.0.0.1:5301, framed PCM
                    |
             svxlink-chat service ----- OpenAI Realtime API
```

Received RF and active FRN audio already feed the repeater's link output. Link
input already feeds both RF TX and the active module's input. ChatLogic uses
these paths, so it does not activate/deactivate FRN or take the module slot.
Reflector participation follows the existing enabled links and selected
talkgroup. Other active modules may mute links and are not supported as AI input
sources in this version.

SvxLink links form a connected group: linking ChatLogic to a repeater which is
already linked to a reflector also connects ChatLogic to that reflector. Replies
are heard by **all currently connected destinations**, not privately by the
originating station. Linking FRN and a reflector also carries ordinary traffic
between them. Keep only the intended networks connected.

New incoming audio interrupts pending/generating AI answers. The plugin discards
its remaining playback and rejects replies from earlier turns. A small amount
already handed to a transmitter or network codec may still drain. An interrupted
exchange clears the API session so an unheard answer is not retained in history.
Changing the selected talkgroup in ReflectorLogic also stops the conversation.
ReflectorV2 has no equivalent selected-talkgroup notification; treat link/network
reconfiguration as a reason to stop the conversation first.

The default is a half-duplex exchange: one question per transmission, up to 45
seconds, followed by one reply up to 30 seconds. A reply is fully buffered before
transmission to avoid gaps from the Internet connection. This adds latency.
SvxLink's normal squelch, TX delay, repeater, link, FRN arbitration and reflector
talkgroup rules remain in charge. A simplex radio cannot receive a stop command
while physically transmitting. Network DTMF and RF interruption therefore depend
on audio actually reaching SvxLink. Local wake detection does not require the
Internet; AI answers do.

## Install

Use Python **3.11 or newer** on Linux (64-bit ARM or x86). Rebuild/install SvxLink
from this source revision, including `ChatLogic.so`, updated `ReflectorLogic.so`,
`events.tcl`, `events.d/ChatControl.tcl`, the `chat` directory and the
`svxlink-chat.service` unit. These are included by CMake installation. Existing
prebuilt archives do not gain the feature until rebuilt. Paths below assume
the usual `/usr` prefix; adjust for a different installation prefix.

Install the Python environment on the repeater computer:

```sh
sudo apt-get install python3-venv
sudo python3 -m venv /opt/svxlink-chat
sudo /opt/svxlink-chat/bin/pip install -r /usr/share/svxlink/chat/requirements-wake.txt
sudo /opt/svxlink-chat/bin/pip install --no-deps openwakeword==0.6.0
sudo /opt/svxlink-chat/bin/python /usr/share/svxlink/chat/svxlink_chat.py \
  --config /etc/svxlink/chat.json --download-wake-model
```

The separate `--no-deps` install is intentional: openWakeWord 0.6 declares a Linux
TFLite dependency that lacks wheels for some newer Python versions. This service
uses ONNX; `requirements-wake.txt` installs the dependencies for that path.
The model download runs once during setup, not on service startup. The pretrained
models have their own [CC BY-NC-SA license](https://github.com/dscripka/openWakeWord#license).

For DTMF-only operation, install `requirements.txt` instead and set
`"wake_enabled": false` in `/etc/svxlink/chat.json`; omit the model download.

Create `/etc/svxlink/chat.env` with an editor on the repeater computer, containing:

```text
OPENAI_API_KEY=your_api_key_here
```

Set it to root ownership and mode `0600`. Systemd reads it before switching to
the configured SvxLink user. Do not put the key in Tcl, `svxlink.conf`, Git, or
radio messages. API access/billing is separate from the ChatGPT application.
The configurable model defaults to `gpt-realtime-2.1`, with voice `marin`; the
account must have access to that model. Audio stays in the local service until
activation; after activation the shared conversation is sent to OpenAI. The
service does not write recordings, transcripts or API keys to its logs.

## Merge into the existing SvxLink configuration

Append `ChatLogic` to `GLOBAL/LOGICS` and `ChatLink` to `GLOBAL/LINKS`. Keep the
existing entries and existing repeater/FRN/reflector configuration. For example,
if the radio logic is named `RepeaterLogic`:

```ini
[GLOBAL]
# Example only: keep all your existing logic and link entries here.
LOGICS=RepeaterLogic,ChatLogic
LINKS=ChatLink

[RepeaterLogic]
# Add to the existing section (retain its RX, TX, modules, event handler, etc.).
CHAT_LOGIC=ChatLogic

[ChatLogic]
TYPE=Chat
SERVICE_PORT=5301
DTMF_START=981
DTMF_STOP=980
DTMF_RESET=982
NETWORK_DTMF=1
TX_DELAY_MS=250
MAX_REPLY_SECONDS=30

[ChatLink]
CONNECT_LOGICS=RepeaterLogic,ChatLogic
DEFAULT_ACTIVE=1
```

In the **existing** `[ModuleFrn]` section set:

```ini
MUTE_LOGIC_LINKING=0
```

That setting is necessary for simultaneous FRN and linked AI audio. It also
permits other configured links while FRN is active. Existing reflector links can
remain in `GLOBAL/LINKS`; include their logic names in `GLOBAL/LOGICS` as before.
No FRN module is required for RF-only use.

For radios which require outgoing CTCSS, include `LOGIC` in the radio logic's
existing `TX_CTCSS` list (for example `TX_CTCSS=SQL_OPEN,LOGIC`), or use `ALWAYS`
if that is already the site's policy. Chat replies are linked audio, so
`SQL_OPEN` alone does not cover them. The outgoing tone frequency and level are
configured separately in the actual TX section; keep them matched to the
listening radios. The receiver's CTCSS frequency is the incoming tone.

If the radio logic has a different name (for example `SimplexLogic`), use that
name in the section and `CONNECT_LOGICS`. Configure just one ChatLogic for a
linked group. The helper accepts one plugin connection and binds loopback only.
Keep `SERVICE_PORT` equal to `port` in `chat.json`. DTMF values exclude the final
`#` and must be distinct, 1–16 characters from `0–9` and `A–D`.

The updated standard `events.tcl` installs ChatControl **after** site overrides.
If using a custom top-level event handler, source
`/usr/share/svxlink/events.d/ChatControl.tcl` after its existing DTMF handler and
overrides have been loaded. The per-radio `CHAT_LOGIC` setting enables the wrapper.
Do not replace the site's `dtmf_cmd_received` after installing the wrapper.

Start the service, then restart SvxLink during a suitable maintenance window:

```sh
sudo /opt/svxlink-chat/bin/python /usr/share/svxlink/chat/svxlink_chat.py \
  --config /etc/svxlink/chat.json --check-config
sudo systemctl daemon-reload
sudo systemctl enable --now svxlink-chat
sudo systemctl restart svxlink
sudo journalctl -u svxlink-chat -u svxlink -n 80 --no-pager
```

Running `--check-config` outside systemd will normally report the key absent,
because the shell has not loaded `chat.env`. This check validates the JSON only;
it does not contact OpenAI. Look for `wake=True` in the helper log and
`Voice service connected` in SvxLink's log. If wake setup fails the log says
DTMF remains enabled. API failures log selected error identifiers, HTTP status
or WebSocket close codes. Connection startup failures can also show a bounded,
credential-redacted close reason. Audio, transcripts, full API error messages
and HTTP headers are not dumped into logs.

If activation plays an error tone and returns to standby, test the API connection
without transmitting audio. This uses systemd to load the same key file and run
as the service user; it does not bind the local bridge port, so the normal
helper can keep running:

```sh
sudo systemd-run --wait --pipe --collect \
  --property=User=svxlink \
  --property=EnvironmentFile=/etc/svxlink/chat.env \
  /opt/svxlink-chat/bin/python /usr/share/svxlink/chat/svxlink_chat.py \
  --config /etc/svxlink/chat.json --check-api
```

Use the configured service username if different. Success prints
`Realtime connection OK; no audio sent`. Failure includes the startup error
metadata: `http_status=401` indicates an authentication rejection,
`code=model_not_found` identifies a model/access error, and
`code=insufficient_quota` identifies a quota/billing rejection. A close code alone
does not establish the cause; include the diagnostic output when reporting it.
The connection check does not test question upload or speech generation.

The helper reads the initial `session.created` or `error` event before sending
`session.update`, so an early server rejection is not hidden by a failed write
to an already closed connection. `Realtime session ready` is logged only after
the server acknowledges the session configuration.

Updates confined to `svxlink_chat.py` can be installed by replacing
`/usr/share/svxlink/chat/svxlink_chat.py` with the file from the updated checkout,
then restarting `svxlink-chat`. A C++ rebuild and wake-model download are not
required for those Python-only changes.

Validate RF-only first, then active FRN, then the intended reflector talkgroup:
start, ask a short question, hear a reply, follow up, reset, stop, and exercise
unrelated existing DTMF commands. Check first-syllable clipping and tune
`TX_DELAY_MS` along with the existing transmitter's delay. Test interruption and
wake false positives on the actual radios before leaving voice activation on.

To disable the feature, remove `ChatLogic`/`ChatLink` from the global lists,
remove `CHAT_LOGIC` from the radio section, restart SvxLink and disable the
`svxlink-chat` service. No FRN module replacement is involved.

## Developer verification

Offline service tests use mocked API responses and require the base Python
dependencies. Tcl tests exercise exact commands, imported site handlers, module
states and disabled configuration:

```sh
python3 -B -m unittest discover -s tests -p test_svxlink_chat.py -v
tclsh tests/chat_control.tcl
# Include real ONNX inference after downloading the wake model into this Python environment:
CHAT_TEST_WAKE_MODEL=1 python3 -B -m unittest discover -s tests -p test_svxlink_chat.py -v
```

For native integration, enable `-DBUILD_CHAT_TESTS=ON` in a configured build and
build `svxlink`, `ChatLogic` and `ChatTestLogic`. The fixture is never installed:

```sh
python3 -B tests/chat_native.py --build /path/to/build
# Cross-built ARM64 binaries can be exercised through qemu-aarch64-static:
python3 -B tests/chat_native.py --build /path/to/arm64-build --sysroot /path/to/arm64-sysroot
```

Use `--rate 8000` for an 8 kHz build. The test feeds generated audio through the
real LinkManager and DTMF decoder, checks exact matching, rejects stale replies,
captures PCM playback, interrupts playback, and sends malformed control JSON.
It also changes talkgroups synchronously during audio delivery to exercise cancellation.
It does not validate radio hardware, FRN server behavior, reflector access,
wake accuracy or a live paid API exchange.

The service uses [manual Realtime turns](https://developers.openai.com/api/docs/guides/realtime-conversations)
over [WebSockets](https://developers.openai.com/api/docs/guides/voice-websockets),
with mono PCM16 resampled to the API's 24 kHz format. Native audio remains at
SvxLink's compiled 8 or 16 kHz rate. API sessions are renewed after 30 minutes;
stop/reset, an interrupted reply, idle timeout or helper restart also clears
conversation history.
