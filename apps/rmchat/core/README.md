# RMChat core — phase 0

This executable implements the IPC contract in `docs/rmchat-phase0.md` from the
repository root. It has no GUI, daemon, built-in account, or tablet deployment.
Authentication is supplied by the C++ parent over its private socket. The parent
owns the encrypted `SecretStore`; Go stores credentials only in memory.

Build with Go 1.25 or later:

```sh
CGO_ENABLED=0 go test ./...
CGO_ENABLED=0 go build -trimpath -o rmchat-core ./cmd/rmchat-core
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -trimpath -o rmchat-core-arm64 ./cmd/rmchat-core
```

Launch only from a parent that creates a private `0700` directory:

```sh
./rmchat-core --socket /absolute/private/core.sock --upload-root /absolute/pdf-directory
```

`--upload-root` is repeatable. Without it, `files.upload` is absent from the
capabilities. The socket itself has mode `0600`, accepts one client, and closes
after that client's EOF. An existing endpoint is never overwritten. A core
without an initial connection exits after ten seconds. Production endpoints
are HTTPS on `chatgpt.com`; test transport overrides are internal Go fields,
with no command-line or environment endpoint switch.

`auth.status` requires no network. `auth.import` validates an access token using
the source-audited authenticated `/backend-api/models` endpoint and requires a
real models array; it does not invent an account identity. Session-token import
requires `persistSession:true`; omission or false returns
`SESSION_PERSISTENCE_REQUIRED` before any network request. One ordinary
`/api/auth/session` exchange with a valid `accessToken` confirms that session and
can return the actual user id/name. No additional `/models` request delays
saving a rotated credential. A failed candidate preserves the preceding
in-memory session.

Only a successful session-token `auth.import` response includes the private
`credentialUpdate` object, a version-1 `session_token` credential containing the
renewed cookie value or the original value when no replacement was returned.
The C++ parent must save it to its encrypted store and remove it before any
public output or UI signal. It never appears in `auth.status`, progress or
errors. Base cookies and up to 16 contiguous numbered chunks are supported;
ambiguous, incomplete or oversized replacements fail explicitly. The complete
credential envelope is limited to 32 KiB. Session renewal occurs when the parent
opens the connection; the core has no renewal timer.

Ordinary response cookies stay in a private in-memory jar for the validated
session and continue across models, history, upload metadata, preparation and
chat requests. Each candidate import starts with an empty jar and a fresh
connection pool. Failed imports
preserve the active account's jar; account replacement and logout close it,
including against late responses. Cookies are restricted to the ChatGPT origin.
Signed PDF storage PUTs use a separate cookie-free client and no bearer token.

JSON-RPC uses UTF-8 NDJSON, a maximum frame of 1 MiB, string `ui:<integer>` ids,
strict parameter fields, and one final response per accepted request. Busy
operations remain cancellable. `chat.progress` carries cumulative text snapshots
at most about every 300 ms. Unknown or oversized content yields a structured
error instead of a successful truncated answer. Only the active conversation
branch is returned; cursors fail when that branch changes between pages.

History displays user and assistant messages only, with a public recipient and
the final or legacy empty channel. Visually hidden messages, thinking preambles,
tool output, editable context, thoughts and reasoning recaps are excluded before
pagination. Empty non-attachment messages are skipped. The continuation parent
remains the exact `current_node`, including a hidden or empty branch tip.

The RMChat SSE adapter keeps raw message metadata, applies v1 root snapshots and
ordered JSON patches atomically, and supports compressed append events. A new
message resets text, completion and citations. Incomplete message construction
and legacy absent/null channels are withheld until stream completion; explicit
final snapshots can stream immediately. Reclassifying hidden content cannot
publish text or citation metadata inherited from its previous channel, including
temporary hidden states inside a patch batch. Known image/audio parts receive
plain placeholders; transport pointers are never displayed. Assistant citation metadata
is normalized to Markdown source links or readable placeholders while literal
user messages and fenced/inline code examples are preserved.

All included test-server responses and file contents are synthetic fixtures.
Successful local tests do not establish that private ChatGPT Web endpoints will
accept a real session. The ordinary chat attempt stops on Web protection and is
never resubmitted automatically after an uncertain result. See `UPSTREAM.md`
for exact reuse and protocol limitations.

Authentication diagnostics distinguish an explicitly signalled challenge from
an unqualified HTTP 403 or an unexpected HTML response. Error metadata exposes
only the HTTP status, a normalized content-type enum, and whether a challenge
was explicitly reported. A false JSON challenge flag is not a challenge. Raw
headers, bodies, URLs and cookies are never included in this diagnostic.

`chat.send` errors also include a fixed `stage` value: `validation`, `prepare`,
`submit`, or `stream`. Exact upstream machine codes can refine a rejection to
`MODEL_UNAVAILABLE` or `RATE_LIMITED`; a free-form message never supplies the
cause of a 403. Explicit JSON errors and required challenges stop preparation
even if the HTTP status is 200. These diagnostics do not retry a request.

Model identifiers are trimmed and deduplicated by exact slug while preserving
server order. Distinct slugs remain distinct even if their titles match. One
validator accepts the same slug syntax when listing and sending. The normalized
list excludes models with explicit model-level boolean denial, disabled/hidden
or upgrade-required flags; a denial wins if duplicate rows disagree. Missing
flags, category/default-model references, subscription labels and model names
are not interpreted as proof of access. These optional flag rules are tested
with synthetic fixtures and do not establish live entitlement.
The last successfully loaded catalogue belongs to the active session; logout or account
replacement clears it. If `chat.send` has no catalogue, it first loads it with
one authenticated GET. The requested slug must occur in that catalogue before
preparation begins. No alternative model is selected automatically. Catalogue
membership by itself does not establish current entitlement or remaining quota.

For a local diagnostic, the startup option `--enable-model-inspection` explicitly
enables `models.inspect {}`. This method is absent by default. It performs one
ordinary authenticated `/backend-api/models` GET and returns at most 128 KiB of
projected schema keys, model/category identifiers and titles, booleans, and
bounded access counters. Unknown string values, descriptions, raw headers,
bodies and credentials are excluded. It neither changes the active model list
nor writes a file. The calling parent must still save and remove any private
`credentialUpdate` from an earlier import before invoking this diagnostic.
