package rmchat

import (
	"encoding/json"
	"strconv"
	"strings"
	"time"
)

// Keep the upstream JSON fields until visibility is decided. Aurora's copied
// response type discards hidden-message metadata and supplies public defaults
// for partial patches; those defaults cannot be used as display authorization.
type streamState struct {
	response       map[string]any
	lastPath       string
	lastOperation  string
	snapshot       bool
	hiddenContent  bool
	done           bool
	lastEmit       time.Time
	sequence       int
	text           string
	conversationID string
	messageID      string
}

func streamMessage(state *streamState) (*remoteMessage, bool) {
	value, exists := state.response["message"]
	if !exists || value == nil {
		return nil, false
	}
	var message remoteMessage
	if json.Unmarshal(jsonBytes(value), &message) != nil {
		return nil, false
	}
	return &message, true
}

func applyStreamValue(state *streamState, value any, depth int) *Error {
	if problem := mutateStreamValue(state, value, depth); problem != nil {
		return problem
	}
	if explicitChallenge(state.response["challenge"]) || explicitChallenge(state.response["turnstile"]) || explicitChallenge(state.response["proofofwork"]) {
		return failure("WEB_AUTH_REQUIRED")
	}
	if code := explicitRemoteError(state.response); code != "" {
		return failure(code)
	}
	if len(jsonBytes(state.response)) > MaxFrame/2 {
		return failure("RESPONSE_TOO_LARGE")
	}
	return projectStreamMessage(state, false)
}

func explicitlyHiddenMessage(message *remoteMessage) bool {
	return (message.Author.Role != "" && message.Author.Role != "assistant") ||
		message.Metadata.IsThinkingPreambleMessage || message.Metadata.IsVisuallyHiddenFromConversation ||
		(message.Channel != "" && message.Channel != "final") || (message.Recipient != "" && message.Recipient != "all") ||
		(message.Content.ContentType != "" && message.Content.ContentType != "text" && message.Content.ContentType != "multimodal_text")
}

// Track provenance after every mutation, including those inside one event.
// A temporary analysis classification in a patch batch must not become public
// merely because a later patch changes the channel back to final.
func trackStreamVisibility(state *streamState, resetContent bool) *Error {
	if resetContent {
		state.hiddenContent = false
	}
	message, ok := streamMessage(state)
	if !ok {
		return nil
	}
	if explicitlyHiddenMessage(message) {
		state.hiddenContent = true
	} else if state.hiddenContent && publicMessage(message) {
		object := state.response["message"].(map[string]any)
		parts := make([]any, len(message.Content.Parts))
		for index := range parts {
			parts[index] = ""
		}
		object["content"] = map[string]any{"content_type": message.Content.ContentType, "parts": parts}
		if metadata, ok := object["metadata"].(map[string]any); ok {
			delete(metadata, "content_references")
			delete(metadata, "citations")
		}
		state.hiddenContent = false
	}
	return nil
}

func projectStreamMessage(state *streamState, completed bool) *Error {
	state.text, state.messageID, state.done = "", "", false
	if id, ok := state.response["conversation_id"].(string); ok && id != "" {
		state.conversationID = id
	}
	message, ok := streamMessage(state)
	if !ok || message.Author.Role != "assistant" || !publicMessage(message) {
		return nil
	}
	object, ok := state.response["message"].(map[string]any)
	if !ok {
		return failure("UPSTREAM_ERROR")
	}
	endTurn, _ := object["end_turn"].(bool)
	// Without an explicit final channel, an incremental message may still be
	// waiting for its analysis/tool classification. Legacy output is released
	// only at completion; an explicit final channel can stream immediately.
	if (message.Channel == "" || !state.snapshot) && !completed {
		return nil
	}
	text := publicMessageParts(message)
	if len(text) > 256<<10 {
		return failure("RESPONSE_TOO_LARGE")
	}
	state.text = displayMessageText(text, message.Metadata.ContentReferences, message.Metadata.Citations)
	if len(state.text) > 256<<10 {
		return failure("RESPONSE_TOO_LARGE")
	}
	state.messageID, state.done = message.ID, endTurn
	return nil
}

func mutateStreamValue(state *streamState, value any, depth int) *Error {
	if depth > 20 {
		return failure("UPSTREAM_ERROR")
	}
	if marker, ok := value.(string); ok && marker == "v1" {
		return nil
	}
	object, ok := value.(map[string]any)
	if !ok {
		return failure("UPSTREAM_ERROR")
	}
	if explicitChallenge(object["challenge"]) || explicitChallenge(object["turnstile"]) || explicitChallenge(object["proofofwork"]) {
		return failure("WEB_AUTH_REQUIRED")
	}
	if code := explicitRemoteError(object); code != "" {
		return failure(code)
	}
	if object["choices"] != nil {
		return failure("UNSUPPORTED")
	}
	if kind, _ := object["type"].(string); kind != "" {
		if kind == "stream_handoff" {
			return failure("UNSUPPORTED")
		}
		// Input echoes, resume tokens, titles and status objects are envelopes,
		// never generated output, even if one happens to contain message or v.
		return nil
	}
	operation, hasOperation := object["o"].(string)
	path, hasPath := object["p"].(string)
	if operation == "patch" {
		patches, ok := object["v"].([]any)
		if !ok || len(patches) > 1024 {
			return failure("UPSTREAM_ERROR")
		}
		for _, patch := range patches {
			if problem := mutateStreamValue(state, patch, depth+1); problem != nil {
				return problem
			}
		}
		return nil
	}
	if hasOperation || hasPath {
		if !hasOperation {
			operation = state.lastOperation
		}
		if !hasPath {
			path = state.lastPath
		}
		return applyStreamPatch(state, path, operation, object["v"])
	}
	if _, exists := object["message"]; exists {
		state.response = object
		state.snapshot = completeStreamSnapshot(object["message"])
		state.lastPath, state.lastOperation = "/message/content/parts/0", "append"
		return trackStreamVisibility(state, true)
	}
	if nested, ok := object["v"].(map[string]any); ok {
		return mutateStreamValue(state, nested, depth+1)
	}
	if patches, ok := object["v"].([]any); ok {
		if len(patches) > 1024 {
			return failure("RESPONSE_TOO_LARGE")
		}
		for _, patch := range patches {
			if problem := mutateStreamValue(state, patch, depth+1); problem != nil {
				return problem
			}
		}
		return nil
	}
	if _, exists := object["v"]; exists && state.lastOperation != "" {
		return applyStreamPatch(state, state.lastPath, state.lastOperation, object["v"])
	}
	return nil
}

func applyStreamPatch(state *streamState, path, operation string, value any) (problem *Error) {
	resetContent := false
	defer func() {
		if problem == nil {
			problem = trackStreamVisibility(state, resetContent)
		}
	}()
	if operation != "append" && operation != "replace" && operation != "add" && operation != "remove" {
		return failure("UNSUPPORTED")
	}
	if len(path) > 1024 || strings.Count(path, "/") > 32 || (path != "" && !strings.HasPrefix(path, "/")) {
		return failure("UPSTREAM_ERROR")
	}
	state.lastPath, state.lastOperation = path, operation
	if path == "" {
		root, ok := value.(map[string]any)
		if !ok || (operation != "add" && operation != "replace") {
			return failure("UPSTREAM_ERROR")
		}
		state.response = root
		state.snapshot = completeStreamSnapshot(root["message"])
		return trackStreamVisibility(state, true)
	}
	if state.response == nil {
		state.response = map[string]any{}
	}
	if path == "/message/id" {
		id, ok := value.(string)
		if !ok || !validID(id) {
			return failure("UPSTREAM_ERROR")
		}
		if message, ok := state.response["message"].(map[string]any); ok && message["id"] != id {
			state.response["message"] = map[string]any{}
			state.snapshot = false
			resetContent = true
		}
	}
	if path == "/message" {
		state.snapshot = completeStreamSnapshot(value)
	}
	if path == "/message/metadata/content_references" && operation == "append" {
		if object, ok := value.(map[string]any); ok {
			value = []any{object}
		}
	}
	if (path == "/message" || path == "/message/content" || path == "/message/content/parts") && (operation == "replace" || operation == "add") {
		resetContent = true
	}
	tokens := strings.Split(path[1:], "/")
	for i, token := range tokens {
		tokens[i] = strings.NewReplacer("~1", "/", "~0", "~").Replace(token)
	}
	updated, ok := mutateJSON(state.response, tokens, operation, value)
	if !ok {
		return failure("UPSTREAM_ERROR")
	}
	state.response = updated.(map[string]any)
	return nil
}

func completeStreamSnapshot(value any) bool {
	message, ok := value.(map[string]any)
	if !ok {
		return false
	}
	author, authorOK := message["author"].(map[string]any)
	_, contentOK := message["content"].(map[string]any)
	role, _ := author["role"].(string)
	return authorOK && contentOK && role != ""
}

// Only a bounded subset of JSON patch is needed by v1. Array append accepts
// both an array of new elements and a single content-reference object.
func mutateJSON(current any, path []string, operation string, value any) (any, bool) {
	if len(path) == 0 {
		if operation != "append" {
			return value, true
		}
		switch typed := current.(type) {
		case nil:
			return value, true
		case string:
			part, ok := value.(string)
			return typed + part, ok
		case []any:
			if extra, ok := value.([]any); ok {
				return append(typed, extra...), len(typed)+len(extra) <= 1024
			}
			return append(typed, value), len(typed) < 1024
		default:
			return nil, false
		}
	}
	if current == nil {
		if index, err := strconv.Atoi(path[0]); err == nil && index >= 0 || path[0] == "-" {
			current = []any{}
		} else {
			current = map[string]any{}
		}
	}
	switch typed := current.(type) {
	case map[string]any:
		if operation == "remove" && len(path) == 1 {
			delete(typed, path[0])
			return typed, true
		}
		updated, ok := mutateJSON(typed[path[0]], path[1:], operation, value)
		if !ok {
			return nil, false
		}
		typed[path[0]] = updated
		return typed, true
	case []any:
		index, err := strconv.Atoi(path[0])
		if path[0] == "-" {
			index, err = len(typed), nil
		}
		if err != nil || index < 0 || index > len(typed) || index >= 1024 {
			return nil, false
		}
		if index == len(typed) {
			typed = append(typed, nil)
		} else if operation == "add" && len(path) == 1 {
			if len(typed) >= 1024 {
				return nil, false
			}
			typed = append(typed, nil)
			copy(typed[index+1:], typed[index:])
		}
		if operation == "remove" && len(path) == 1 {
			return append(typed[:index], typed[index+1:]...), true
		}
		updated, ok := mutateJSON(typed[index], path[1:], operation, value)
		if !ok {
			return nil, false
		}
		typed[index] = updated
		return typed, true
	default:
		return nil, false
	}
}
