package rmchat

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func snapshotEvent(id, channel, text string, done bool) string {
	return string(jsonBytes(map[string]any{"message": map[string]any{"id": id, "author": map[string]any{"role": "assistant"}, "content": map[string]any{"content_type": "text", "parts": []any{text}}, "channel": channel, "recipient": "all", "end_turn": done}, "conversation_id": "conv-1"}))
}
func sse(payloads ...string) string { return "data: " + strings.Join(payloads, "\n\ndata: ") + "\n\n" }
func TestStreamSnapshotsAndUnicodePatches(t *testing.T) {
	text := sse(snapshotEvent("answer-1", "final", "Bonjour", false), `{"v":" à"}`, `{"v":[{"p":"/message/content/parts/0","o":"append","v":" tous 🌍"}]}`, `{"p":"/message/end_turn","o":"replace","v":true}`, "[DONE]")
	var updates []Progress
	result, e := readStream(context.Background(), strings.NewReader(text), "user-1", func(p Progress) { updates = append(updates, p) })
	if e != nil {
		t.Fatal(e)
	}
	message := result.(map[string]any)["message"].(Message)
	if message.Text != "Bonjour à tous 🌍" || *message.ParentID != "user-1" || result.(map[string]any)["continuationParentId"] != "answer-1" {
		t.Fatalf("bad final %+v", result)
	}
	if len(updates) != 1 || updates[0].Sequence != 1 {
		t.Fatalf("unbatched updates %d", len(updates))
	}
}
func TestStreamWithholdsAnalysisAndThinkingPreamble(t *testing.T) {
	preamble := map[string]any{"v": map[string]any{"message": map[string]any{"id": "thinking", "author": map[string]any{"role": "assistant"}, "content": map[string]any{"parts": []any{"private preamble"}}, "metadata": map[string]any{"is_thinking_preamble_message": true}, "end_turn": false}, "conversation_id": "conv-1"}}
	text := sse(snapshotEvent("analysis-1", "analysis", "private reasoning", false), string(jsonBytes(preamble)), snapshotEvent("answer-1", "final", "Réponse publique", true), "[DONE]")
	var output strings.Builder
	result, e := readStream(context.Background(), strings.NewReader(text), "user-1", func(p Progress) { output.WriteString(p.Text) })
	if e != nil {
		t.Fatal(e)
	}
	output.Write(jsonBytes(result))
	if strings.Contains(output.String(), "private") {
		t.Fatal("internal channel was published")
	}
}
func TestStreamPrematureEOFAndChangedMessageCompletion(t *testing.T) {
	for _, text := range []string{sse(snapshotEvent("answer-1", "final", "partiel", false)), sse(snapshotEvent("answer-1", "final", "terminé", true), snapshotEvent("answer-2", "final", "nouveau partiel", false))} {
		_, e := readStream(context.Background(), strings.NewReader(text), "user-1", nil)
		requireCode(t, e, "OUTCOME_UNKNOWN")
	}
}
func TestStreamBoundsEmptyDataLinesAndContent(t *testing.T) {
	_, e := readStream(context.Background(), strings.NewReader(strings.Repeat("data:\n", MaxFrame+1)), "user-1", nil)
	requireCode(t, e, "RESPONSE_TOO_LARGE")
	_, e = readStream(context.Background(), strings.NewReader(sse(snapshotEvent("answer-1", "final", strings.Repeat("x", (256<<10)+1), true))), "user-1", nil)
	requireCode(t, e, "RESPONSE_TOO_LARGE")
}
func TestStreamRejectsChallengeHandoffAndMalformedEvent(t *testing.T) {
	for _, tc := range []struct{ payload, code string }{{`{"challenge":{"required":true}}`, "WEB_AUTH_REQUIRED"}, {`{"type":"stream_handoff","options":[]}`, "UNSUPPORTED"}, {`not-json`, "UPSTREAM_ERROR"}} {
		_, e := readStream(context.Background(), strings.NewReader(sse(tc.payload)), "user-1", nil)
		requireCode(t, e, tc.code)
	}
}
func TestStreamMultilineJSONAndFragmentedUTF8(t *testing.T) {
	text := "data: {\"conversation_id\":\"conv-1\",\n" + "data: \"message\":{\"id\":\"answer-1\",\"author\":{\"role\":\"assistant\"},\"content\":{\"parts\":[\"Été 🌞\"]},\"end_turn\":true}}\n\ndata: [DONE]\n\n"
	result, e := readStream(context.Background(), oneByteReader{strings.NewReader(text)}, "user-1", nil)
	if e != nil || result.(map[string]any)["message"].(Message).Text != "Été 🌞" {
		t.Fatalf("fragmented Unicode %v %v", result, e)
	}
}

type oneByteReader struct{ io.Reader }

func (r oneByteReader) Read(p []byte) (int, error) { return r.Reader.Read(p[:min(1, len(p))]) }
func validSend() SendParams {
	return SendParams{MessageID: "c18c4a46-05b9-4daa-a597-64613025823b", Model: "remote-model", Text: "Question"}
}
func setSendSession(c *Client, token string) {
	setSession(c, token)
	c.state.models = map[string]bool{"remote-model": true}
}
func TestSendNormalPrepareAndSingleSubmission(t *testing.T) {
	var posts atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer supplied" {
			t.Error("wrong auth")
		}
		for name := range r.Header {
			if strings.Contains(strings.ToLower(name), "sentinel") || strings.Contains(strings.ToLower(name), "proof") {
				t.Error("protection header generated")
			}
		}
		switch r.URL.Path {
		case "/backend-api/f/conversation/prepare":
			jsonResponse(w, map[string]any{"conduit_token": "issued-by-fixture"})
		case "/backend-api/f/conversation":
			posts.Add(1)
			if r.Header.Get("X-Conduit-Token") != "issued-by-fixture" {
				t.Error("server conduit not preserved")
			}
			var payload map[string]any
			json.NewDecoder(r.Body).Decode(&payload)
			if payload["history_and_training_disabled"] != false || payload["parent_message_id"] != "client-created-root" {
				t.Error("wrong conversation lifecycle")
			}
			w.Header().Set("Content-Type", "text/event-stream")
			fmt.Fprint(w, sse(snapshotEvent("answer-1", "final", "Réponse", true), "[DONE]"))
		default:
			t.Error("unexpected endpoint")
		}
	})
	setSendSession(c, "supplied")
	p := validSend()
	result, e := c.Send(context.Background(), p, nil)
	if e != nil || result == nil {
		t.Fatalf("send %v %v", result, e)
	}
	_, e = c.Send(context.Background(), p, nil)
	requireCode(t, e, "DUPLICATE_REQUEST")
	if posts.Load() != 1 {
		t.Fatal("message submitted more than once")
	}
}
func TestSendPreparationChallengeNeverSubmitsMessage(t *testing.T) {
	var posts atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/backend-api/f/conversation" {
			posts.Add(1)
		}
		w.Header().Set("Content-Type", "application/json")
		fmt.Fprint(w, `{"turnstile":{"required":true}}`)
	})
	setSendSession(c, "secret")
	_, e := c.Send(context.Background(), validSend(), nil)
	requireCode(t, e, "WEB_AUTH_REQUIRED")
	if posts.Load() != 0 {
		t.Fatal("chat submitted despite challenge")
	}
}
func TestSendCancellationStopsStreamWithoutRetry(t *testing.T) {
	started := make(chan struct{})
	var posts atomic.Int32
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) {
		if strings.HasSuffix(r.URL.Path, "/prepare") {
			jsonResponse(w, map[string]any{})
			return
		}
		posts.Add(1)
		w.Header().Set("Content-Type", "text/event-stream")
		fmt.Fprint(w, sse(snapshotEvent("answer-1", "final", "Partiel", false)))
		w.(http.Flusher).Flush()
		close(started)
		<-r.Context().Done()
	})
	setSendSession(c, "secret")
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan *Error, 1)
	go func() { _, e := c.Send(ctx, validSend(), nil); done <- e }()
	<-started
	cancel()
	select {
	case e := <-done:
		requireCode(t, e, "CANCELLED")
		if !e.Uncertain {
			t.Error("cancel should disclose uncertain outcome")
		}
	case <-time.After(2 * time.Second):
		t.Fatal("stream not cancelled")
	}
	if posts.Load() != 1 {
		t.Fatal("automatic retry")
	}
}
func TestSendRejectsUnknownAttachmentBeforeNetwork(t *testing.T) {
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) { t.Error("unknown attachment reached network") })
	setSession(c, "secret")
	p := validSend()
	p.Attachments = []string{"foreign-file"}
	_, e := c.Send(context.Background(), p, nil)
	requireCode(t, e, "FILE_REJECTED")
}
