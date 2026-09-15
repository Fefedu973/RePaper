package rmchat

import (
	"bufio"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"time"

	"github.com/google/uuid"
)

func (c *Client) Send(ctx context.Context, p SendParams, emit func(Progress)) (result any, problem *Error) {
	stage := "validation"
	defer func() {
		if problem != nil {
			problem.Stage = stage
		}
	}()
	if _, err := uuid.Parse(p.MessageID); err != nil || len(p.MessageID) != 36 || len(p.Text) > 256<<10 || strings.TrimSpace(p.Text) == "" || !validModelID(p.Model) || len(p.Attachments) > 10 {
		return nil, failure("INVALID_PARAMS")
	}
	if p.ConversationID != "" && (!validID(p.ConversationID) || !validID(p.ParentMessageID)) {
		return nil, failure("INVALID_PARAMS")
	}
	if p.ConversationID == "" && p.ParentMessageID != "" {
		return nil, failure("INVALID_PARAMS")
	}
	s, problem := c.authenticated()
	if problem != nil {
		return nil, problem
	}
	attachments := []map[string]any{}
	c.state.mu.Lock()
	if c.state.session.generation != s.generation {
		c.state.mu.Unlock()
		return nil, failure("CANCELLED")
	}
	if c.state.models != nil && !c.state.models[p.Model] {
		c.state.mu.Unlock()
		return nil, failure("MODEL_UNAVAILABLE")
	}
	modelsLoaded := c.state.models != nil
	if c.state.sent[p.MessageID] {
		c.state.mu.Unlock()
		return nil, failure("DUPLICATE_REQUEST")
	}
	for _, id := range p.Attachments {
		a, ok := c.state.uploads[id]
		if !ok {
			c.state.mu.Unlock()
			return nil, failure("FILE_REJECTED")
		}
		entry := map[string]any{"id": a.ID, "name": a.Name, "size": a.Size, "mime_type": a.MIME, "mimeType": a.MIME, "source": "library", "is_big_paste": false}
		if a.LibraryID != "" {
			entry["library_file_id"] = a.LibraryID
		}
		attachments = append(attachments, entry)
	}
	c.state.mu.Unlock()
	if !modelsLoaded {
		if _, problem = c.Models(ctx); problem != nil {
			return nil, problem
		}
		c.state.mu.Lock()
		generationMatches := c.state.session.generation == s.generation
		modelAllowed := c.state.models[p.Model]
		c.state.mu.Unlock()
		if !generationMatches {
			return nil, failure("CANCELLED")
		}
		if !modelAllowed {
			return nil, failure("MODEL_UNAVAILABLE")
		}
	}
	parent := p.ParentMessageID
	if parent == "" {
		parent = "client-created-root"
	}
	_, offset := time.Now().Zone()
	timezone := time.Now().Location().String()
	prepare := map[string]any{"action": "next", "parent_message_id": parent, "model": p.Model, "client_prepare_state": "none", "client_prepare_dispatch": "debounced", "client_prepare_source": "composer_editor_state", "timezone_offset_min": -offset / 60, "timezone": timezone, "conversation_mode": map[string]string{"kind": "primary_assistant"}, "system_hints": []string{}, "supports_buffering": true, "supported_encodings": []string{"v1"}}
	if p.ConversationID != "" {
		prepare["conversation_id"] = p.ConversationID
	}
	stage = "prepare"
	resp, problem := c.request(ctx, http.MethodPost, "/backend-api/f/conversation/prepare", s, postBody(prepare))
	if problem != nil {
		return nil, problem
	}
	var prepared struct {
		ConduitToken string `json:"conduit_token"`
	}
	if problem = decodeResponse(resp, &prepared, 128<<10); problem != nil {
		return nil, problem
	}
	if len(prepared.ConduitToken) > 32768 || strings.ContainsAny(prepared.ConduitToken, "\r\n\x00") {
		return nil, failure("UPSTREAM_ERROR")
	}
	message := map[string]any{"id": p.MessageID, "author": map[string]string{"role": "user"}, "create_time": float64(time.Now().UnixMilli()) / 1000, "content": map[string]any{"content_type": "text", "parts": []string{p.Text}}, "metadata": map[string]any{"attachments": attachments}}
	payload := map[string]any{"action": "next", "messages": []any{message}, "parent_message_id": parent, "model": p.Model, "timezone_offset_min": -offset / 60, "timezone": timezone, "conversation_mode": map[string]string{"kind": "primary_assistant"}, "system_hints": []string{}, "history_and_training_disabled": false}
	if p.ConversationID != "" {
		payload["conversation_id"] = p.ConversationID
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.base+"/backend-api/f/conversation", postBody(payload))
	if err != nil {
		return nil, failure("INVALID_PARAMS")
	}
	req.Header.Set("Authorization", "Bearer "+s.token)
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "text/event-stream")
	req.Header.Set("User-Agent", "reMoodle-rmchat/0.1")
	if prepared.ConduitToken != "" {
		req.Header.Set("X-Conduit-Token", prepared.ConduitToken)
	}
	c.state.mu.Lock()
	if c.state.session.generation != s.generation {
		c.state.mu.Unlock()
		return nil, failure("CANCELLED")
	}
	if c.state.sent[p.MessageID] {
		c.state.mu.Unlock()
		return nil, failure("DUPLICATE_REQUEST")
	}
	c.state.sent[p.MessageID] = true
	c.state.mu.Unlock()
	stage = "submit"
	resp, err = s.http.Do(req)
	if err != nil {
		if ctx.Err() != nil {
			e := failure("CANCELLED")
			e.Uncertain = true
			return nil, e
		}
		e := failure("OUTCOME_UNKNOWN")
		e.Uncertain = true
		return nil, e
	}
	defer resp.Body.Close()
	if problem = responseError(resp); problem != nil {
		return nil, problem
	}
	if !strings.Contains(strings.ToLower(resp.Header.Get("Content-Type")), "text/event-stream") {
		var ignore map[string]any
		if problem = decodeResponse(resp, &ignore, 128<<10); problem != nil {
			return nil, problem
		}
		return nil, httpFailure(resp, "UPSTREAM_ERROR", false)
	}
	stage = "stream"
	result, problem = readStream(ctx, resp.Body, p.MessageID, emit)
	if problem != nil && problem.HTTP == nil {
		problem.HTTP = &HTTPDiagnostics{Status: resp.StatusCode, ContentType: normalizedContentType(resp.Header.Get("Content-Type")), Challenge: problem.Code == "WEB_AUTH_REQUIRED"}
	}
	if problem != nil && problem.Code != "WEB_AUTH_REQUIRED" && problem.Code != "RATE_LIMITED" && problem.Code != "MODEL_UNAVAILABLE" {
		problem.Uncertain = true
	}
	return result, problem
}

func readStream(ctx context.Context, reader io.Reader, parentID string, emit func(Progress)) (any, *Error) {
	scanner := bufio.NewScanner(reader)
	scanner.Buffer(make([]byte, 4096), MaxFrame+1)
	state := streamState{}
	data := []string{}
	bytesInEvent := 0
	events := 0
	doneMarker := false
	consume := func() *Error {
		if len(data) == 0 {
			return nil
		}
		payload := strings.Join(data, "\n")
		data = nil
		bytesInEvent = 0
		events++
		if events > 100000 {
			return failure("RESPONSE_TOO_LARGE")
		}
		if payload == "[DONE]" {
			doneMarker = true
			return projectStreamMessage(&state, true)
		}
		var value any
		if json.Unmarshal([]byte(payload), &value) != nil {
			return failure("UPSTREAM_ERROR")
		}
		if problem := applyStreamValue(&state, value, 0); problem != nil {
			return problem
		}
		if state.text != "" && time.Since(state.lastEmit) >= 300*time.Millisecond {
			state.sequence++
			state.lastEmit = time.Now()
			if emit != nil {
				emit(Progress{Sequence: state.sequence, ConversationID: optional(state.conversationID), MessageID: optional(state.messageID), Text: state.text})
			}
		}
		return nil
	}
	for scanner.Scan() {
		if ctx.Err() != nil {
			return nil, failure("CANCELLED")
		}
		line := strings.TrimSuffix(scanner.Text(), "\r")
		if line == "" {
			if problem := consume(); problem != nil {
				return nil, problem
			}
			if doneMarker {
				break
			}
			continue
		}
		if strings.HasPrefix(line, "data:") {
			piece := strings.TrimPrefix(line, "data:")
			piece = strings.TrimPrefix(piece, " ")
			bytesInEvent += len(piece) + 1
			if bytesInEvent > MaxFrame {
				return nil, failure("RESPONSE_TOO_LARGE")
			}
			data = append(data, piece)
		}
	}
	if scanner.Err() != nil {
		if ctx.Err() != nil {
			return nil, failure("CANCELLED")
		}
		return nil, failure("OUTCOME_UNKNOWN")
	}
	if problem := consume(); problem != nil {
		return nil, problem
	}
	if ctx.Err() != nil {
		return nil, failure("CANCELLED")
	}
	// Older snapshots can have no channel. Wait through all classification
	// patches, then release a completed legacy message at EOF or [DONE].
	if object, ok := state.response["message"].(map[string]any); ok {
		endTurn, _ := object["end_turn"].(bool)
		if endTurn || doneMarker {
			if problem := projectStreamMessage(&state, true); problem != nil {
				return nil, problem
			}
		}
	}
	if !(doneMarker || state.done) || state.text == "" || !validID(state.messageID) || !validID(state.conversationID) {
		return nil, failure("OUTCOME_UNKNOWN")
	}
	message := Message{ID: state.messageID, ParentID: optional(parentID), Role: "assistant", Text: state.text, Attachments: []MessageAttachment{}}
	return map[string]any{"conversationId": state.conversationID, "message": message, "continuationParentId": state.messageID}, nil
}
