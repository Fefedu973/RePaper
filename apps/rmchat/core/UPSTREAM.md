# Aurora components used by RMChat phase 0

Pinned upstream: [aurora-develop/aurora](https://github.com/aurora-develop/aurora/tree/15983ced4d3a7f9faa3b5871086ebbefa8f8881b), commit `15983ced4d3a7f9faa3b5871086ebbefa8f8881b`.
The original MIT license is preserved verbatim in `LICENSE`.

These files are copied byte-for-byte from that commit:

- `LICENSE`
- `httpclient/Iaurorahttpclient.go`
- `internal/sseparser/parser.go` and `parser_test.go`
- `typings/chatgpt/response.go`
- `typings/official/request.go`, `request_test.go`, `response.go`, `response_test.go`, and `models.go`

The application module deliberately retains the upstream module name `aurora`
so its `internal` imports remain valid. This is a selected slice, not a build of
Aurora's server. Its sole external module is `github.com/google/uuid v1.6.0`,
pinned in `go.mod` and `go.sum`. Go 1.25 is the minimum for `os.OpenRoot`; host
and Linux ARM64 tests use Go 1.25.6 with CGO disabled. Upstream's complete service
requires a newer toolchain and additional components that are not included here.
The Go 1.25.6 runtime and google/uuid 1.6.0 BSD notices are copied unchanged
from those installed dependencies to `licenses/go.txt` and
`licenses/google-uuid.txt` and accompany the binaries.

`cmd/rmchat-core` and `internal/rmchat` are the RMChat integration, with a standard
`net/http` transport, bounded JSON-RPC/SSE parsing, cancellation, and a single
explicit in-memory session. The HTTP session, model, PDF upload, ordinary
prepare/send payloads and PDF metadata are adapted from these upstream files:

- `internal/chatgpt/auth.go`
- `internal/chatgpt/models.go`
- `internal/chatgpt/files.go`
- `internal/chatgpt/images.go` (`getConversation`)
- `internal/chatgpt/conversation.go`
- `internal/chatgpt/handler_response.go`
- `typings/chatgpt/request.go`
- `conversion/requests/chatgpt/convert.go`

Conversation listing/pagination and active-branch normalization are new RMChat
code. Aurora's selected snapshot does not supply a conversation-list method.
Fixtures validate these adaptations, not the current availability of the
private ChatGPT Web endpoints. No live account request is part of the tests.

The copied SSE parser remains byte-for-byte upstream, but RMChat's message
projection uses its own `internal/rmchat/stream_patch.go` adapter. This preserves
metadata omitted by the upstream response type, avoids implicit assistant/final
defaults for partial messages, handles v1 root replacement, and evaluates
visibility only after each complete event's ordered patches. History and stream
share public-message rules; tools, system context, thoughts/reasoning recap,
hidden messages and non-public channels/recipients are never display messages.

The fixture shapes also draw on these primary accounts of observed Web formats,
which are reverse-engineering references, not an official OpenAI protocol:

- [dmarx conversation export schema](https://gist.github.com/dmarx/08afeb669cdc2f974d6aca61dcce360d)
- [export-chatgpt project/file research](https://github.com/brianjlacy/export-chatgpt/blob/main/_docs/PROJECTS_FILES_RESEARCH.md)
- [chatgpt2api upstream SSE notes](https://github.com/lowkruc/chatgpt2api/blob/master/docs/upstream-sse-conversation.md)

Source references from `content_references` and legacy `citations` are normalized
by RMChat's `message_text.go`; Aurora's marker-removal fallback is not used in
public output. Full browser-only widgets/media are represented by readable
placeholders. The parser tests cover synthetic analysis/tool/final sequences,
visibility field ordering, active-branch pagination and reference patches.

The full Aurora send orchestrator requires protection components before its
normal prepare/send primitives. RMChat performs only one ordinary preparation
and one explicit submission with the supplied legitimate credential and any
conduit token returned by that preparation. It does not include Aurora's
Sentinel, proof-token, Turnstile, TLS/browser impersonation, account-pool,
fingerprint, or quota-modification components. A challenge is reported as
`WEB_AUTH_REQUIRED`; no solver, synthesized protection token, alternate identity,
or automatic resend is provided. A missing acknowledgement remains uncertain.

Session rotation follows the persistence purpose of Aurora PR #278, adapted
to the C++ parent's encrypted store. This does not reuse Aurora's helper named
`NewStdClient()`: that helper uses `tls-client` and a Chrome profile, whereas
RMChat uses native Go `net/http` with an isolated cookie jar and connection pool.
An `auth.import` of `session_token` requires `persistSession:true` before any
network request. One `/api/auth/session` response with a valid `accessToken`
confirms authentication without an intervening `/models` request.

The successful import's private `credentialUpdate` carries the renewed or
unchanged session credential to the parent, which must persist and remove it
before public output. `auth.status`, other results, errors and progress never
include it. A base response cookie or a bounded, complete numbered chunk
sequence is accepted; ambiguous or incomplete replacements return
`SESSION_REIMPORT_REQUIRED`. This chunk handling is new RMChat code, not a
feature supplied by PR #278. The core keeps secrets in memory only and has no
automatic renewal timer. Cancellation or a failed response cannot undo a
session rotation already performed by the server.

PDF reservation follows the upstream `store_in_library:true` / opportunistic
library workflow and may create a file in the account's ChatGPT library. The
returned attachment reference is usable only by this core and its current
account. A signed storage URL receives no bearer or cookies. Files are opened
under `os.OpenRoot`, checked as regular PDFs, capped at 64 MiB, SHA-256 verified,
and copied to a private temporary snapshot before transmission. Linux opens
non-blockingly so invalid FIFOs cannot trap cancellation before `Stat`.
