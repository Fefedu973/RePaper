package rmchat

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"net/http"
	"net/url"
	"strconv"
	"strings"
)

type cursor struct {
	Kind       string `json:"k"`
	ID         string `json:"i,omitempty"`
	Node       string `json:"n,omitempty"`
	Offset     int    `json:"o"`
	Generation uint64 `json:"g"`
}

func encodeCursor(v cursor) *string {
	s := base64.RawURLEncoding.EncodeToString(jsonBytes(v))
	return &s
}
func decodeCursor(value, kind, id string, generation uint64) (cursor, *Error) {
	out := cursor{Kind: kind, ID: id, Generation: generation}
	if value == "" {
		return out, nil
	}
	if len(value) > 1024 {
		return out, failure("INVALID_PARAMS")
	}
	data, err := base64.RawURLEncoding.DecodeString(value)
	if err != nil || json.Unmarshal(data, &out) != nil || out.Kind != kind || out.ID != id || out.Generation != generation || out.Offset < 0 || out.Offset > 1000000 {
		return out, failure("INVALID_PARAMS")
	}
	return out, nil
}
func validID(id string) bool {
	if id == "" || len(id) > 128 {
		return false
	}
	for _, r := range id {
		if !(r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' || r == '-' || r == '_') {
			return false
		}
	}
	return true
}
func validLimit(limit int) bool { return limit >= 1 && limit <= 50 }
func (c *Client) ListConversations(ctx context.Context, value string, limit int) (any, *Error) {
	if !validLimit(limit) {
		return nil, failure("INVALID_PARAMS")
	}
	s, problem := c.authenticated()
	if problem != nil {
		return nil, problem
	}
	page, problem := decodeCursor(value, "list", "", s.generation)
	if problem != nil {
		return nil, problem
	}
	path := "/backend-api/conversations?offset=" + strconv.Itoa(page.Offset) + "&limit=" + strconv.Itoa(limit) + "&order=updated"
	resp, problem := c.request(ctx, http.MethodGet, path, s, nil)
	if problem != nil {
		return nil, problem
	}
	var raw struct {
		Items []struct {
			ID         string          `json:"id"`
			Title      string          `json:"title"`
			UpdateTime json.RawMessage `json:"update_time"`
		} `json:"items"`
		Total *int `json:"total"`
	}
	if problem = decodeResponse(resp, &raw, 8<<20); problem != nil {
		return nil, problem
	}
	if raw.Items == nil || len(raw.Items) > limit {
		return nil, failure("UPSTREAM_ERROR")
	}
	items := []ConversationSummary{}
	for _, item := range raw.Items {
		if !validID(item.ID) {
			return nil, failure("UPSTREAM_ERROR")
		}
		items = append(items, ConversationSummary{ID: item.ID, Title: item.Title, UpdatedAt: optional(safeTime(item.UpdateTime))})
	}
	var next *string
	newOffset := page.Offset + len(items)
	if len(items) == limit && (raw.Total == nil || newOffset < *raw.Total) {
		page.Offset = newOffset
		next = encodeCursor(page)
	}
	return map[string]any{"items": items, "nextCursor": next}, nil
}

type remoteMessage struct {
	ID        string `json:"id"`
	Channel   string `json:"channel"`
	Recipient string `json:"recipient"`
	Author    struct {
		Role string `json:"role"`
	} `json:"author"`
	Content struct {
		ContentType string            `json:"content_type"`
		Parts       []json.RawMessage `json:"parts"`
	} `json:"content"`
	Metadata struct {
		IsThinkingPreambleMessage        bool            `json:"is_thinking_preamble_message"`
		IsVisuallyHiddenFromConversation bool            `json:"is_visually_hidden_from_conversation"`
		ContentReferences                json.RawMessage `json:"content_references"`
		Citations                        json.RawMessage `json:"citations"`
		Attachments                      []struct {
			ID       string `json:"id"`
			Name     string `json:"name"`
			MIMEType string `json:"mime_type"`
		} `json:"attachments"`
	} `json:"metadata"`
}
type remoteNode struct {
	ID      string         `json:"id"`
	Parent  *string        `json:"parent"`
	Message *remoteMessage `json:"message"`
}
type remoteConversation struct {
	ID          string                `json:"conversation_id"`
	Title       string                `json:"title"`
	CurrentNode string                `json:"current_node"`
	Mapping     map[string]remoteNode `json:"mapping"`
}

func normalizeBranch(raw remoteConversation) ([]Message, string, *Error) {
	if raw.Mapping == nil || !validID(raw.CurrentNode) || len(raw.Mapping) > 10000 {
		return nil, "", failure("UPSTREAM_ERROR")
	}
	branch := []Message{}
	seen := map[string]bool{}
	nodeID := raw.CurrentNode
	// Continue from the selected branch tip, even when that node is hidden or
	// contains no message. Display filtering must never choose a different branch.
	continuation := raw.CurrentNode
	for nodeID != "" {
		if seen[nodeID] {
			return nil, "", failure("UPSTREAM_ERROR")
		}
		seen[nodeID] = true
		node, exists := raw.Mapping[nodeID]
		if !exists {
			return nil, "", failure("UPSTREAM_ERROR")
		}
		if node.Message != nil && node.Message.ID != "" {
			m := node.Message
			if !validID(m.ID) {
				return nil, "", failure("UPSTREAM_ERROR")
			}
			if !publicMessage(m) {
				if node.Parent == nil {
					break
				}
				nodeID = *node.Parent
				continue
			}
			attachments := []MessageAttachment{}
			for _, a := range m.Metadata.Attachments {
				if a.ID != "" {
					attachments = append(attachments, MessageAttachment{ID: a.ID, Filename: optional(a.Name), MIMEType: optional(a.MIMEType)})
				}
			}
			text := publicMessageParts(m)
			if m.Author.Role == "assistant" {
				text = displayMessageText(text, m.Metadata.ContentReferences, m.Metadata.Citations)
			}
			if strings.TrimSpace(text) != "" || len(attachments) != 0 {
				branch = append(branch, Message{ID: m.ID, ParentID: node.Parent, Role: m.Author.Role, Text: text, Attachments: attachments})
			}
		}
		if node.Parent == nil {
			break
		}
		nodeID = *node.Parent
	}
	for i, j := 0, len(branch)-1; i < j; i, j = i+1, j-1 {
		branch[i], branch[j] = branch[j], branch[i]
	}
	return branch, continuation, nil
}
func (c *Client) GetConversation(ctx context.Context, id, value string, limit int) (any, *Error) {
	if !validID(id) || !validLimit(limit) {
		return nil, failure("INVALID_PARAMS")
	}
	s, problem := c.authenticated()
	if problem != nil {
		return nil, problem
	}
	page, problem := decodeCursor(value, "get", id, s.generation)
	if problem != nil {
		return nil, problem
	}
	resp, problem := c.request(ctx, http.MethodGet, "/backend-api/conversation/"+url.PathEscape(id), s, nil)
	if problem != nil {
		return nil, problem
	}
	var raw remoteConversation
	if problem = decodeResponse(resp, &raw, 16<<20); problem != nil {
		return nil, problem
	}
	if raw.ID != "" && raw.ID != id {
		return nil, failure("UPSTREAM_ERROR")
	}
	branch, continuation, problem := normalizeBranch(raw)
	if problem != nil {
		return nil, problem
	}
	if page.Node != "" && page.Node != raw.CurrentNode {
		return nil, failure("UPSTREAM_ERROR")
	}
	if page.Offset > len(branch) {
		return nil, failure("INVALID_PARAMS")
	}
	end := min(page.Offset+limit, len(branch))
	out := Conversation{ID: id, Title: raw.Title, Messages: branch[page.Offset:end], ContinuationParentID: optional(continuation)}
	if end < len(branch) {
		page.Offset = end
		page.Node = raw.CurrentNode
		out.NextCursor = encodeCursor(page)
	}
	return out, nil
}
