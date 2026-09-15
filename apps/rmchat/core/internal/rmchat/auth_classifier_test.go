package rmchat

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"testing"
)

func classifiedResponse(status int, contentType, mitigated, body string) *http.Response {
	response := &http.Response{StatusCode: status, Header: make(http.Header), Body: io.NopCloser(strings.NewReader(body))}
	response.Header.Set("Content-Type", contentType)
	if mitigated != "" {
		response.Header.Set("Cf-Mitigated", mitigated)
	}
	return response
}

func TestHTTPClassifierDistinguishesEvidenceFromStatusAndHTML(t *testing.T) {
	for _, test := range []struct {
		name                                     string
		status                                   int
		contentType, mitigated, code, normalized string
		challenge                                bool
	}{
		{"json403", 403, "application/json", "", "UPSTREAM_FORBIDDEN", "application/json", false},
		{"html403", 403, "text/html; charset=UTF-8", "", "UPSTREAM_FORBIDDEN", "text/html", false},
		{"html200", 200, "text/html", "", "UNEXPECTED_HTML", "text/html", false},
		{"xhtml200", 200, "application/xhtml+xml", "", "UNEXPECTED_HTML", "application/xhtml+xml", false},
		{"html401", 401, "text/html", "", "AUTH_INVALID", "text/html", false},
		{"html429", 429, "text/html", "", "RATE_LIMITED", "text/html", false},
		{"html404", 404, "text/html", "", "NOT_FOUND", "text/html", false},
		{"html302", 302, "text/html", "", "UPSTREAM_ERROR", "text/html", false},
		{"json500", 500, "application/json", "", "UPSTREAM_ERROR", "application/json", false},
		{"explicit403", 403, "text/html", "challenge", "WEB_AUTH_REQUIRED", "text/html", true},
		{"normalizedExplicit", 200, "TEXT/HTML; charset=utf-8", "  ChAlLeNgE  ", "WEB_AUTH_REQUIRED", "text/html", true},
		{"otherMitigation", 403, "application/json", "none", "UPSTREAM_FORBIDDEN", "application/json", false},
		{"unknownType", 403, "", "", "UPSTREAM_FORBIDDEN", "unknown", false},
		{"privateSubtype", 403, "text/private-secret", "", "UPSTREAM_FORBIDDEN", "other", false},
		{"invalidMime", 403, "invalid private-secret", "", "UPSTREAM_FORBIDDEN", "other", false},
		{"mimeParameter", 403, "application/json; diagnostic=private-secret", "", "UPSTREAM_FORBIDDEN", "application/json", false},
	} {
		t.Run(test.name, func(t *testing.T) {
			response := classifiedResponse(test.status, test.contentType, test.mitigated, "private-secret-body")
			defer response.Body.Close()
			problem := responseError(response)
			requireCode(t, problem, test.code)
			if problem.HTTP == nil || problem.HTTP.Status != test.status || problem.HTTP.ContentType != test.normalized || problem.HTTP.Challenge != test.challenge {
				t.Fatalf("incorrect evidence %+v", problem.HTTP)
			}
			if strings.Contains(string(jsonBytes(problem)), "private-secret") {
				t.Fatal("raw header/body leaked")
			}
			if test.status == 401 && strings.Contains(problem.Message, "expir") {
				t.Fatal("401 must not assert expiration")
			}
		})
	}
	for _, contentType := range []string{"application/json", "application/json; charset=utf-8", "text/event-stream"} {
		response := classifiedResponse(200, contentType, "", `{}`)
		if problem := responseError(response); problem != nil {
			t.Fatalf("successful response rejected %v", problem)
		}
		response.Body.Close()
	}
}

func TestJSONChallengeRequiresExplicitPositiveEvidence(t *testing.T) {
	for _, test := range []struct {
		name, body string
		challenge  bool
	}{
		{"false", `{"challenge":false,"models":[]}`, false},
		{"null", `{"challenge":null,"models":[]}`, false},
		{"requiredFalse", `{"challenge":{"required":false},"models":[]}`, false},
		{"turnstileFalse", `{"turnstile":false,"models":[]}`, false},
		{"turnstileNotRequired", `{"turnstile":{"required":false},"models":[]}`, false},
		{"emptyObject", `{"challenge":{},"models":[]}`, false},
		{"string", `{"challenge":"private-secret","models":[]}`, false},
		{"true", `{"challenge":true}`, true},
		{"requiredTrue", `{"challenge":{"required":true}}`, true},
		{"turnstileRequired", `{"turnstile":{"required":true}}`, true},
	} {
		t.Run(test.name, func(t *testing.T) {
			response := classifiedResponse(200, "application/json; charset=utf-8", "", test.body)
			var result map[string]any
			problem := decodeResponse(response, &result, 4096)
			if test.challenge {
				requireCode(t, problem, "WEB_AUTH_REQUIRED")
				if problem.HTTP == nil || !problem.HTTP.Challenge || problem.HTTP.Status != 200 || problem.HTTP.ContentType != "application/json" {
					t.Fatal("missing explicit challenge evidence")
				}
			} else if problem != nil {
				t.Fatalf("non-challenge rejected: %v", problem)
			}
		})
	}
}

func TestAuthImportAcceptsFalseChallengeMarkers(t *testing.T) {
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		jsonResponse(w, map[string]any{"models": []any{}, "challenge": false, "turnstile": map[string]any{"required": false}})
	})
	status, problem := c.Import(context.Background(), credential("fixture-private-secret"), false)
	if problem != nil || !status.Authenticated {
		t.Fatalf("valid models rejected as challenge: %v", problem)
	}
}

func TestHTTPFailureMetadataPropagatesOnlySafeFieldsOverRPC(t *testing.T) {
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json; diagnostic=fixture-private-secret")
		w.Header().Set("Location", "https://private.example/fixture-private-secret")
		w.Header().Set("Set-Cookie", "private=fixture-private-secret")
		w.Header().Set("Server", "fixture-private-secret")
		w.WriteHeader(403)
		io.WriteString(w, `{"error":"fixture-private-secret"}`)
	})
	setSession(c, "preserved")
	peer := testPeer(t, c)
	peer.send(t, "ui:1", "auth.import", map[string]any{"credential": credential("fixture-private-secret")})
	response := peer.read(t)
	if rpcKind(response) != "UPSTREAM_FORBIDDEN" {
		t.Fatalf("403 misclassified %+v", response)
	}
	data := response["error"].(map[string]any)["data"].(map[string]any)
	if len(data) != 5 || data["httpStatus"] != float64(403) || data["contentType"] != "application/json" || data["challenge"] != false || data["retryable"] != false {
		t.Fatalf("metadata contract %+v", data)
	}
	encoded, _ := json.Marshal(response)
	if strings.Contains(string(encoded), "fixture-private-secret") || strings.Contains(string(encoded), "private.example") {
		t.Fatal("sensitive HTTP details leaked through IPC")
	}
	if c.state.current().token != "preserved" {
		t.Fatal("403 replaced the preceding session")
	}
}

func TestDecodeErrorsRetainOnlyNormalizedHTTPMetadata(t *testing.T) {
	for _, test := range []struct {
		name, body, code string
		limit            int64
	}{{"invalidJSON", "private-secret", "UPSTREAM_ERROR", 4096}, {"oversize", strings.Repeat("x", 1025), "RESPONSE_TOO_LARGE", 1024}} {
		t.Run(test.name, func(t *testing.T) {
			response := classifiedResponse(200, "application/json; source=private-secret", "", test.body)
			var result any
			problem := decodeResponse(response, &result, test.limit)
			requireCode(t, problem, test.code)
			if problem.HTTP == nil || problem.HTTP.Status != 200 || problem.HTTP.ContentType != "application/json" || problem.HTTP.Challenge || strings.Contains(string(jsonBytes(problem)), "private-secret") {
				t.Fatalf("unsafe metadata %+v", problem)
			}
		})
	}
}
