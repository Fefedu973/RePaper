package rmchat

import (
	"encoding/json"
	"strings"
)

// Conversation mappings also contain system context, tools and reasoning.
// Empty channel/content_type retain compatibility with complete older messages.
func publicMessage(m *remoteMessage) bool {
	if m.Author.Role != "user" && m.Author.Role != "assistant" {
		return false
	}
	if m.Metadata.IsVisuallyHiddenFromConversation || m.Metadata.IsThinkingPreambleMessage ||
		(m.Channel != "" && m.Channel != "final") || (m.Recipient != "" && m.Recipient != "all") {
		return false
	}
	switch m.Content.ContentType {
	case "", "text", "multimodal_text":
		return true
	default:
		return false
	}
}

func publicMessageParts(message *remoteMessage) string {
	parts := []string{}
	for _, part := range message.Content.Parts {
		var text string
		if json.Unmarshal(part, &text) == nil {
			parts = append(parts, text)
			continue
		}
		if message.Content.ContentType != "multimodal_text" {
			continue
		}
		var media struct {
			ContentType string `json:"content_type"`
		}
		if json.Unmarshal(part, &media) != nil {
			continue
		}
		switch media.ContentType {
		case "image_asset_pointer":
			parts = append(parts, "[Image]")
		case "audio", "input_audio":
			parts = append(parts, "[Audio]")
		}
	}
	return strings.Join(parts, "\n")
}
