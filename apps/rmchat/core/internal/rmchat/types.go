// Package rmchat provides an isolated, explicitly authenticated ChatGPT web client.
// Its transport never solves or fabricates browser protection challenges.
package rmchat

import (
	"context"
	"encoding/json"
	"net/http"
	"sync"
	"time"
)

const MaxFrame = 1 << 20
const MaxPDF = 64 << 20

type Credential struct {
	Version  int    `json:"version"`
	Provider string `json:"provider"`
	Kind     string `json:"kind"`
	Value    string `json:"value"`
}
type Account struct {
	ID          string  `json:"id"`
	DisplayName *string `json:"displayName"`
}
type Capabilities struct {
	ProtocolVersion int      `json:"protocolVersion"`
	Provider        string   `json:"provider"`
	CredentialKinds []string `json:"credentialKinds"`
	Methods         []string `json:"methods"`
}
type AuthStatus struct {
	Authenticated bool         `json:"authenticated"`
	Account       *Account     `json:"account"`
	Capabilities  Capabilities `json:"capabilities"`
}

// AuthImportResult is private IPC to the parent, which must consume and remove
// CredentialUpdate before producing any user-visible output. AuthStatus stays
// a separate type so credentials cannot appear in auth.status or progress.
type AuthImportResult struct {
	AuthStatus
	CredentialUpdate *Credential `json:"credentialUpdate,omitempty"`
}
type Model struct {
	ID   string `json:"id"`
	Name string `json:"name"`
}
type ConversationSummary struct {
	ID        string  `json:"id"`
	Title     string  `json:"title"`
	UpdatedAt *string `json:"updatedAt"`
}
type MessageAttachment struct {
	ID       string  `json:"id"`
	Filename *string `json:"filename"`
	MIMEType *string `json:"mimeType"`
}
type Message struct {
	ID          string              `json:"id"`
	ParentID    *string             `json:"parentId"`
	Role        string              `json:"role"`
	Text        string              `json:"text"`
	Attachments []MessageAttachment `json:"attachments"`
}
type Conversation struct {
	ID                   string    `json:"id"`
	Title                string    `json:"title"`
	Messages             []Message `json:"messages"`
	ContinuationParentID *string   `json:"continuationParentId"`
	NextCursor           *string   `json:"nextCursor"`
}
type Attachment struct {
	ID        string `json:"id"`
	Name      string `json:"name"`
	MIME      string `json:"mime"`
	Size      int64  `json:"size"`
	SHA256    string `json:"sha256"`
	LibraryID string `json:"-"`
}
type SendParams struct {
	MessageID       string   `json:"clientMessageId"`
	ConversationID  string   `json:"conversationId,omitempty"`
	ParentMessageID string   `json:"parentMessageId"`
	Model           string   `json:"modelId"`
	Text            string   `json:"text"`
	Attachments     []string `json:"attachments,omitempty"`
}
type Progress struct {
	RequestID      string  `json:"requestId"`
	Sequence       int     `json:"sequence"`
	ConversationID *string `json:"conversationId"`
	MessageID      *string `json:"messageId"`
	Text           string  `json:"text"`
}
type Error struct {
	Code      string           `json:"code"`
	Message   string           `json:"message"`
	Retryable bool             `json:"retryable"`
	Uncertain bool             `json:"uncertain,omitempty"`
	Stage     string           `json:"stage,omitempty"`
	HTTP      *HTTPDiagnostics `json:"http,omitempty"`
}

// HTTPDiagnostics is an explicit allowlist, never a copy of response headers.
// ContentType contains only a small normalized enum, without MIME parameters.
type HTTPDiagnostics struct {
	Status      int    `json:"httpStatus"`
	ContentType string `json:"contentType"`
	Challenge   bool   `json:"challenge"`
}

func (e *Error) Error() string { return e.Message }
func failure(code string) *Error {
	messages := map[string]string{
		"INVALID_PARAMS": "Paramètres invalides.", "AUTH_REQUIRED": "Connectez votre compte ChatGPT.", "AUTH_INVALID": "Le service a refusé ces identifiants. Réimportez votre session ChatGPT.", "UNSUPPORTED_CREDENTIAL_KIND": "Ce type de connexion n’est pas pris en charge.",
		"SESSION_PERSISTENCE_REQUIRED": "Cette connexion renouvelle la session et nécessite sa sauvegarde dans le coffre.",
		"SESSION_REIMPORT_REQUIRED":    "La session renouvelée est incomplète ou invalide. Reconnectez votre compte ChatGPT.",
		"WEB_AUTH_REQUIRED":            "Une vérification dans ChatGPT est nécessaire. Ouvrez ChatGPT dans un navigateur.", "RATE_LIMITED": "ChatGPT limite temporairement les requêtes. Réessayez plus tard.",
		"UPSTREAM_FORBIDDEN": "Le service a refusé cette requête (HTTP 403). La cause n’a pas été précisée.",
		"MODEL_UNAVAILABLE":  "Ce modèle n’est pas disponible pour ce compte. Choisissez un autre modèle proposé.",
		"UNEXPECTED_HTML":    "Le service a renvoyé une page Web au lieu des données attendues.",
		"NETWORK_ERROR":      "ChatGPT est momentanément injoignable.", "UPSTREAM_ERROR": "ChatGPT a renvoyé une réponse inutilisable.", "CANCELLED": "Requête annulée.",
		"BUSY": "Une opération est déjà en cours.", "UNSUPPORTED": "Cette opération nécessite un parcours web indisponible dans cette version.",
		"FILE_REJECTED": "Sélectionnez un PDF autorisé de 64 Mio maximum.", "FILE_CHANGED": "Le fichier a changé depuis sa sélection.",
		"RESPONSE_TOO_LARGE": "Cette conversation est trop volumineuse pour être affichée.", "NOT_FOUND": "La conversation est introuvable.",
		"OUTCOME_UNKNOWN": "La confirmation de l’envoi manque. Vérifiez la conversation avant de renvoyer.", "DUPLICATE_REQUEST": "Cette requête a déjà été reçue.",
	}
	message, ok := messages[code]
	if !ok {
		message = "L’opération n’a pas abouti."
	}
	return &Error{Code: code, Message: message, Retryable: code == "NETWORK_ERROR" || code == "RATE_LIMITED"}
}
func contextError(ctx context.Context) *Error {
	if ctx.Err() != nil {
		return failure("CANCELLED")
	}
	return failure("NETWORK_ERROR")
}

type session struct {
	token      string
	account    Account
	generation uint64
	http       *http.Client
	cookies    *sessionCookies
}
type state struct {
	mu         sync.Mutex
	session    session
	generation uint64
	authBusy   bool
	uploads    map[string]Attachment
	sent       map[string]bool
	models     map[string]bool
}

func (s *state) current() session { s.mu.Lock(); defer s.mu.Unlock(); return s.session }
func (s *state) clear() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.generation++
	s.session.close()
	s.session = session{}
	s.uploads = map[string]Attachment{}
	s.sent = map[string]bool{}
	s.models = nil
}
func jsonBytes(v any) []byte { b, _ := json.Marshal(v); return b }
func optional(s string) *string {
	if s == "" {
		return nil
	}
	return &s
}
func safeTime(v json.RawMessage) string {
	var text string
	if json.Unmarshal(v, &text) == nil {
		if _, err := time.Parse(time.RFC3339Nano, text); err == nil {
			return text
		}
		return ""
	}
	var seconds float64
	if json.Unmarshal(v, &seconds) == nil && seconds > 0 && seconds < 253402300799 {
		return time.UnixMilli(int64(seconds * 1000)).UTC().Format(time.RFC3339)
	}
	return ""
}
