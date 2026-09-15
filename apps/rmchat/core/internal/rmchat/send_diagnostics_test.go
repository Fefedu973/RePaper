package rmchat

import (
	"context"
	"fmt"
	"net/http"
	"strings"
	"sync/atomic"
	"testing"
)

func TestSendErrorsReportExactSafeStageAndNeverRetry(t *testing.T) {
	for _, test := range []struct {
		name, stage, code   string
		status, submissions int
	}{
		{"prepareForbidden", "prepare", "UPSTREAM_FORBIDDEN", 403, 0},
		{"submissionForbidden", "submit", "UPSTREAM_FORBIDDEN", 403, 1},
		{"streamTruncated", "stream", "OUTCOME_UNKNOWN", 200, 1},
	} {
		t.Run(test.name, func(t *testing.T) {
			var prepares, submissions atomic.Int32
			c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("Content-Type", "application/json; private=private-secret")
				w.Header().Set("Set-Cookie", "private=private-secret")
				if strings.HasSuffix(r.URL.Path, "/prepare") {
					prepares.Add(1)
					if test.stage != "prepare" {
						jsonResponse(w, map[string]any{"conduit_token": "private-secret"})
						return
					}
				} else {
					submissions.Add(1)
				}
				if test.stage == "stream" {
					w.Header().Set("Content-Type", "text/event-stream; private=private-secret")
					fmt.Fprint(w, sse(snapshotEvent("answer-1", "final", "Partiel", false)))
					return
				}
				w.WriteHeader(403)
				fmt.Fprint(w, `{"detail":"private-secret"}`)
			})
			setSendSession(c, "private-secret")
			peer := testPeer(t, c)
			peer.send(t, "ui:1", "chat.send", validSend())
			response := peer.read(t)
			if response["method"] == "chat.progress" {
				response = peer.read(t)
			}
			if rpcKind(response) != test.code {
				t.Fatalf("wrong result: %v", response)
			}
			data := response["error"].(map[string]any)["data"].(map[string]any)
			if data["stage"] != test.stage || data["httpStatus"] != float64(test.status) || data["challenge"] != false {
				t.Fatalf("wrong diagnostic: %v", data)
			}
			if strings.Contains(string(jsonBytes(response)), "private-secret") || prepares.Load() != 1 || submissions.Load() != int32(test.submissions) {
				t.Fatal("sensitive data escaped or request automatically retried")
			}
		})
	}
}

func TestPrepareProofOfWorkChallengeNeverSubmits(t *testing.T) {
	var calls atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		jsonResponse(w, map[string]any{"proofofwork": map[string]any{"required": true, "seed": "private-secret"}})
	})
	setSendSession(c, "fixture")
	_, problem := c.Send(context.Background(), validSend(), nil)
	requireCode(t, problem, "WEB_AUTH_REQUIRED")
	if problem.Stage != "prepare" || calls.Load() != 1 || len(c.state.sent) != 0 || strings.Contains(string(jsonBytes(problem)), "private-secret") {
		t.Fatal("prepare challenge was not stopped before submission")
	}
}

func TestSendClassifiesOnlyExplicitModelRejectionCodes(t *testing.T) {
	for _, test := range []struct {
		body, expected string
	}{
		{`{"detail":{"code":"model_not_found","message":"private-secret"}}`, "MODEL_UNAVAILABLE"},
		{`{"error":{"code":"model_not_available","message":"private-secret"}}`, "MODEL_UNAVAILABLE"},
		{`{"code":"model_not_allowed","message":"private-secret"}`, "MODEL_UNAVAILABLE"},
		{`{"error":{"code":"model_cap_exceeded","message":"private-secret"}}`, "RATE_LIMITED"},
		{`{"detail":"model_not_available private-secret"}`, "UPSTREAM_FORBIDDEN"},
		{`{"error":{"code":"forbidden","message":"model_not_available private-secret"}}`, "UPSTREAM_FORBIDDEN"},
	} {
		t.Run(test.expected+test.body[:10], func(t *testing.T) {
			var calls atomic.Int32
			c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
				calls.Add(1)
				w.Header().Set("Content-Type", "application/json")
				w.WriteHeader(403)
				fmt.Fprint(w, test.body)
			})
			setSendSession(c, "fixture")
			_, problem := c.Send(context.Background(), validSend(), nil)
			requireCode(t, problem, test.expected)
			if problem.Stage != "prepare" || problem.HTTP == nil || problem.HTTP.Status != 403 || calls.Load() != 1 || strings.Contains(string(jsonBytes(problem)), "private-secret") {
				t.Fatal("model rejection diagnostic leaked data or retried")
			}
		})
	}
}

func TestPrepareJSONErrorOnHTTPSuccessDoesNotSubmit(t *testing.T) {
	for _, test := range []struct {
		body, expected string
	}{
		{`{"error":{"code":"model_not_available","message":"private-secret"}}`, "MODEL_UNAVAILABLE"},
		{`{"error":{"code":"unknown","message":"private-secret"}}`, "UPSTREAM_ERROR"},
	} {
		t.Run(test.expected, func(t *testing.T) {
			var calls atomic.Int32
			c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
				calls.Add(1)
				w.Header().Set("Content-Type", "application/json")
				fmt.Fprint(w, test.body)
			})
			setSendSession(c, "fixture")
			_, problem := c.Send(context.Background(), validSend(), nil)
			requireCode(t, problem, test.expected)
			if problem.Stage != "prepare" || calls.Load() != 1 || len(c.state.sent) != 0 {
				t.Fatal("prepare error was mistaken for a successful preparation")
			}
		})
	}
}

func TestStreamModelRejectionIsReportedWithoutPublishingServerText(t *testing.T) {
	_, problem := readStream(context.Background(), strings.NewReader(sse(`{"error":{"code":"model_not_allowed","message":"private-secret"}}`)), "fixture", nil)
	requireCode(t, problem, "MODEL_UNAVAILABLE")
	if strings.Contains(string(jsonBytes(problem)), "private-secret") {
		t.Fatal("raw stream error was published")
	}
}
