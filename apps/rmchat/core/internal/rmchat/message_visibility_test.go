package rmchat

import (
	"context"
	"encoding/json"
	"net/http"
	"strings"
	"testing"
)

func visibleFixtureMessage(id, role, channel, contentType, text string) map[string]any {
	return map[string]any{"id": id, "author": map[string]any{"role": role}, "channel": channel, "recipient": "all", "content": map[string]any{"content_type": contentType, "parts": []any{text}}, "metadata": map[string]any{}}
}

func visibilityConversationFixture() map[string]any {
	messages := []map[string]any{
		visibleFixtureMessage("system", "system", "", "text", "PRIVATE SYSTEM CONTEXT"),
		visibleFixtureMessage("user", "user", "", "multimodal_text", "Ma question avec un PDF"),
		visibleFixtureMessage("analysis", "assistant", "analysis", "text", "PRIVATE ANALYSIS"),
		visibleFixtureMessage("tool-call", "assistant", "commentary", "code", "PRIVATE TOOL CALL"),
		visibleFixtureMessage("tool", "tool", "", "text", "PRIVATE TOOL OUTPUT"),
		visibleFixtureMessage("thoughts", "assistant", "", "thoughts", "PRIVATE THOUGHTS"),
		visibleFixtureMessage("recap", "assistant", "final", "reasoning_recap", "PRIVATE RECAP"),
		visibleFixtureMessage("editable", "user", "", "user_editable_context", "PRIVATE EDITABLE CONTEXT"),
		visibleFixtureMessage("model-context", "assistant", "", "model_editable_context", "PRIVATE MODEL CONTEXT"),
		visibleFixtureMessage("hidden-user", "user", "", "text", "PRIVATE HIDDEN USER"),
		visibleFixtureMessage("preamble", "assistant", "final", "text", "PRIVATE PREAMBLE"),
		visibleFixtureMessage("recipient", "assistant", "final", "text", "PRIVATE TOOL RECIPIENT"),
		visibleFixtureMessage("answer", "assistant", "final", "text", "Réponse publique avec **mise en forme**."),
		visibleFixtureMessage("hidden-tip", "assistant", "final", "text", "PRIVATE HIDDEN TIP"),
	}
	messages[1]["metadata"] = map[string]any{"attachments": []any{map[string]any{"id": "file-1", "name": "Document.pdf", "mime_type": "application/pdf"}}}
	messages[9]["metadata"] = map[string]any{"is_visually_hidden_from_conversation": true}
	messages[10]["metadata"] = map[string]any{"is_thinking_preamble_message": true}
	messages[11]["recipient"] = "python"
	messages[13]["metadata"] = map[string]any{"is_visually_hidden_from_conversation": true}
	mapping := map[string]any{"root": map[string]any{"id": "root", "parent": nil, "message": nil}}
	parent := "root"
	for _, message := range messages {
		id := message["id"].(string)
		mapping[id] = map[string]any{"id": id, "parent": parent, "message": message}
		parent = id
	}
	mapping["inactive-answer"] = map[string]any{"id": "inactive-answer", "parent": "user", "message": visibleFixtureMessage("inactive-answer", "assistant", "final", "text", "INACTIVE BRANCH")}
	return map[string]any{"conversation_id": "conv-1", "title": "Réponse avec recherche", "current_node": parent, "mapping": mapping}
}

func TestConversationFiltersInternalMessagesBeforePaginationAndKeepsHiddenTip(t *testing.T) {
	c := testClient(t, func(w http.ResponseWriter, r *http.Request) { jsonResponse(w, visibilityConversationFixture()) })
	setSession(c, "secret")
	value, problem := c.GetConversation(context.Background(), "conv-1", "", 1)
	if problem != nil {
		t.Fatal(problem)
	}
	first := value.(Conversation)
	if len(first.Messages) != 1 || first.Messages[0].ID != "user" || len(first.Messages[0].Attachments) != 1 || first.NextCursor == nil || *first.ContinuationParentID != "hidden-tip" {
		t.Fatalf("wrong first public page: %+v", first)
	}
	value, problem = c.GetConversation(context.Background(), "conv-1", *first.NextCursor, 1)
	if problem != nil {
		t.Fatal(problem)
	}
	last := value.(Conversation)
	if len(last.Messages) != 1 || last.Messages[0].ID != "answer" || last.NextCursor != nil || *last.ContinuationParentID != "hidden-tip" {
		t.Fatalf("wrong last public page: %+v", last)
	}
	if public := string(jsonBytes([]Conversation{first, last})); strings.Contains(public, "PRIVATE") || strings.Contains(public, "INACTIVE") {
		t.Fatal("internal or inactive content escaped the selected branch")
	}
}

func TestConversationLegacyNullFieldsAndEmptyBranchTip(t *testing.T) {
	fixture := visibilityConversationFixture()
	mapping := fixture["mapping"].(map[string]any)
	answer := mapping["answer"].(map[string]any)["message"].(map[string]any)
	answer["channel"], answer["recipient"] = nil, nil
	delete(answer["content"].(map[string]any), "content_type")
	mapping["empty-tip"] = map[string]any{"id": "empty-tip", "parent": "hidden-tip", "message": nil}
	fixture["current_node"] = "empty-tip"
	var raw remoteConversation
	if err := json.Unmarshal(jsonBytes(fixture), &raw); err != nil {
		t.Fatal(err)
	}
	messages, continuation, problem := normalizeBranch(raw)
	if problem != nil || len(messages) != 2 || messages[1].ID != "answer" || continuation != "empty-tip" {
		t.Fatalf("legacy compatibility or continuation lost: %v %s %v", messages, continuation, problem)
	}
}

func TestConversationCitationMetadataSurvivesPublicProjection(t *testing.T) {
	fixture := conversationFixture()
	message := fixture["mapping"].(map[string]any)["answer-2"].(map[string]any)["message"].(map[string]any)
	marker := "\ue200cite\ue202turn0search0\ue201"
	message["content"] = map[string]any{"content_type": "text", "parts": []any{"Une réponse sourcée. " + marker}}
	message["metadata"] = map[string]any{"content_references": []any{map[string]any{"matched_text": marker, "type": "webpage", "items": []any{map[string]any{"title": "Documentation", "url": "https://example.org/source"}}}}}
	var raw remoteConversation
	json.Unmarshal(jsonBytes(fixture), &raw)
	messages, _, problem := normalizeBranch(raw)
	if problem != nil || len(messages) != 2 || !strings.Contains(messages[1].Text, "[Documentation](https://example.org/source)") || strings.ContainsRune(messages[1].Text, '\ue200') {
		t.Fatalf("citation lost in conversation projection: %+v %v", messages, problem)
	}
}

func TestConversationKnownMediaGetsPlaceholderWithoutExposingPointers(t *testing.T) {
	fixture := conversationFixture()
	mapping := fixture["mapping"].(map[string]any)
	user := mapping["user-1"].(map[string]any)["message"].(map[string]any)
	user["content"] = map[string]any{"content_type": "multimodal_text", "parts": []any{
		map[string]any{"content_type": "image_asset_pointer", "asset_pointer": "PRIVATE FILE POINTER"},
		map[string]any{"content_type": "input_audio", "asset_pointer": "PRIVATE AUDIO POINTER"},
	}}
	answer := mapping["answer-2"].(map[string]any)["message"].(map[string]any)
	answer["content"] = map[string]any{"content_type": "text", "parts": []any{""}}
	var raw remoteConversation
	json.Unmarshal(jsonBytes(fixture), &raw)
	messages, continuation, problem := normalizeBranch(raw)
	if problem != nil || len(messages) != 1 || messages[0].Text != "[Image]\n[Audio]" || continuation != "answer-2" || strings.Contains(string(jsonBytes(messages)), "PRIVATE") {
		t.Fatalf("media projection leaked transport data or emitted empty card: %+v %s %v", messages, continuation, problem)
	}
}
