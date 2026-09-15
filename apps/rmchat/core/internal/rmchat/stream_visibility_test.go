package rmchat

import (
	"context"
	"encoding/json"
	"strings"
	"testing"
)

func rootStreamEvent(message map[string]any) string {
	return string(jsonBytes(map[string]any{"p": "", "o": "add", "v": map[string]any{"message": message, "conversation_id": "conv-1"}, "c": 3}))
}

func applyFixtureStreamValue(t *testing.T, state *streamState, event string) {
	t.Helper()
	var value any
	if err := json.Unmarshal([]byte(event), &value); err != nil {
		t.Fatal(err)
	}
	if problem := applyStreamValue(state, value, 0); problem != nil {
		t.Fatal(problem)
	}
}

func TestStreamV1RootAnalysisToolFinalAndCompressedPatches(t *testing.T) {
	analysis := visibleFixtureMessage("analysis", "assistant", "analysis", "text", "PRIVATE ANALYSIS")
	tool := visibleFixtureMessage("tool", "tool", "", "text", "PRIVATE TOOL OUTPUT")
	answer := visibleFixtureMessage("answer", "assistant", "final", "text", "Bonjour")
	events := []string{`"v1"`, `{"type":"resume_conversation_token","v":"PRIVATE TOKEN"}`, rootStreamEvent(analysis), rootStreamEvent(tool), rootStreamEvent(answer),
		`{"p":"/message/content/parts/0","o":"append","v":" à"}`, `{"v":" tous 🌍"}`,
		`{"p":"","o":"patch","v":[{"p":"/message/status","o":"replace","v":"finished_successfully"},{"p":"/message/end_turn","o":"replace","v":true}]}`, "[DONE]"}
	var output strings.Builder
	result, problem := readStream(context.Background(), strings.NewReader(sse(events...)), "user", func(progress Progress) { output.WriteString(progress.Text) })
	if problem != nil {
		t.Fatal(problem)
	}
	message := result.(map[string]any)["message"].(Message)
	output.Write(jsonBytes(result))
	if message.ID != "answer" || message.Text != "Bonjour à tous 🌍" || strings.Contains(output.String(), "PRIVATE") {
		t.Fatalf("wrong public output: %+v", result)
	}
}

func TestStreamVisibilityPatchesAreAppliedBeforeAnyProjection(t *testing.T) {
	for _, flag := range []string{"channel", "recipient", "hidden", "preamble", "content-type", "role"} {
		t.Run(flag, func(t *testing.T) {
			state := streamState{}
			patch := map[string]any{"p": "/message/channel", "o": "replace", "v": "analysis"}
			switch flag {
			case "recipient":
				patch["p"], patch["v"] = "/message/recipient", "python"
			case "hidden":
				patch["p"], patch["v"] = "/message/metadata/is_visually_hidden_from_conversation", true
			case "preamble":
				patch["p"], patch["v"] = "/message/metadata/is_thinking_preamble_message", true
			case "content-type":
				patch["p"], patch["v"] = "/message/content/content_type", "reasoning_recap"
			case "role":
				patch["p"], patch["v"] = "/message/author/role", "tool"
			}
			batch := map[string]any{"o": "patch", "v": []any{
				map[string]any{"p": "/message", "o": "add", "v": visibleFixtureMessage("hidden", "assistant", "final", "text", "PRIVATE")}, patch}}
			applyFixtureStreamValue(t, &state, string(jsonBytes(batch)))
			if state.text != "" || state.messageID != "" || state.done {
				t.Fatalf("hidden message published between ordered patches: %+v", state)
			}
		})
	}
}

func TestStreamTextBeforeChannelAndMetadataIsWithheld(t *testing.T) {
	state := streamState{}
	for _, event := range []string{
		`{"p":"/conversation_id","o":"add","v":"conv-1"}`,
		`{"p":"/message/id","o":"add","v":"analysis"}`,
		`{"p":"/message/author/role","o":"add","v":"assistant"}`,
		`{"p":"/message/content/parts/0","o":"add","v":"PRIVATE TEXT ARRIVES FIRST"}`,
		`{"p":"/message/channel","o":"add","v":"final"}`,
		`{"p":"/message/metadata/is_visually_hidden_from_conversation","o":"add","v":true}`,
		`{"p":"/message/end_turn","o":"add","v":true}`,
	} {
		applyFixtureStreamValue(t, &state, event)
		if state.text != "" {
			t.Fatal("incomplete classification was treated as a public message")
		}
	}
	applyFixtureStreamValue(t, &state, rootStreamEvent(visibleFixtureMessage("answer", "assistant", "final", "text", "Public")))
	if state.text != "Public" {
		t.Fatal("final root snapshot did not replace hidden partial state")
	}
}

func TestStreamNewMessageClearsPreviousTextCompletionAndCitations(t *testing.T) {
	state := streamState{}
	applyFixtureStreamValue(t, &state, snapshotEvent("old-answer", "final", "Old completed answer", true))
	applyFixtureStreamValue(t, &state, `{"p":"/message/id","o":"replace","v":"next-analysis"}`)
	if state.text != "" || state.messageID != "" || state.done {
		t.Fatal("new message retained the previous public answer")
	}
	applyFixtureStreamValue(t, &state, `{"p":"/message/content/parts/0","o":"add","v":"PRIVATE"}`)
	applyFixtureStreamValue(t, &state, `{"p":"/message/channel","o":"add","v":"analysis"}`)
	if state.text != "" {
		t.Fatal("new analysis leaked old or new content")
	}
	_, problem := readStream(context.Background(), strings.NewReader(sse(snapshotEvent("old", "final", "Old", true), rootStreamEvent(visibleFixtureMessage("tool", "tool", "", "text", "PRIVATE")), "[DONE]")), "user", nil)
	requireCode(t, problem, "OUTCOME_UNKNOWN")
}

func TestStreamReclassifyingHiddenMessageDoesNotPublishItsOldText(t *testing.T) {
	state := streamState{}
	applyFixtureStreamValue(t, &state, rootStreamEvent(visibleFixtureMessage("answer", "assistant", "analysis", "text", "PRIVATE")))
	applyFixtureStreamValue(t, &state, `{"p":"/message/channel","o":"replace","v":"final"}`)
	if state.text != "" {
		t.Fatal("old analysis became public after channel-only patch")
	}
	applyFixtureStreamValue(t, &state, `{"p":"/message/content/parts/0","o":"replace","v":"Public"}`)
	if state.text != "Public" {
		t.Fatal("replacement final content was lost")
	}
}

func TestStreamReplacingOnePartCannotExposeOtherHiddenParts(t *testing.T) {
	for _, operation := range []string{"replace", "remove"} {
		t.Run(operation, func(t *testing.T) {
			state := streamState{}
			message := visibleFixtureMessage("answer", "assistant", "analysis", "text", "PRIVATE FIRST PART")
			message["content"].(map[string]any)["parts"] = []any{"PRIVATE FIRST PART", "PRIVATE SECOND PART"}
			applyFixtureStreamValue(t, &state, rootStreamEvent(message))
			batch := map[string]any{"o": "patch", "v": []any{
				map[string]any{"p": "/message/channel", "o": "replace", "v": "final"},
				map[string]any{"p": "/message/content/parts/1", "o": operation, "v": "Public replacement"},
			}}
			applyFixtureStreamValue(t, &state, string(jsonBytes(batch)))
			if strings.Contains(state.text, "PRIVATE") {
				t.Fatal("partial replacement exposed content collected on a hidden channel")
			}
		})
	}
}

func TestStreamIntermediateHiddenClassificationCannotLeakWithinBatch(t *testing.T) {
	for _, initial := range []bool{true, false} {
		state := streamState{}
		patches := []any{}
		if initial {
			applyFixtureStreamValue(t, &state, rootStreamEvent(visibleFixtureMessage("answer", "assistant", "final", "text", "Public")))
			patches = append(patches, map[string]any{"p": "/message/channel", "o": "replace", "v": "analysis"})
		} else {
			patches = append(patches, map[string]any{"p": "/message", "o": "add", "v": visibleFixtureMessage("answer", "assistant", "analysis", "text", "PRIVATE ROOT")})
		}
		patches = append(patches, map[string]any{"p": "/message/content/parts/0", "o": "append", "v": " PRIVATE APPEND"}, map[string]any{"p": "/message/channel", "o": "replace", "v": "final"})
		applyFixtureStreamValue(t, &state, string(jsonBytes(map[string]any{"o": "patch", "v": patches})))
		if strings.Contains(state.text, "PRIVATE") {
			t.Fatal("intermediate hidden state became public within one patch batch")
		}
	}
}

func TestStreamReclassificationClearsHiddenCitationMetadata(t *testing.T) {
	state := streamState{}
	marker := "\ue200cite\ue202turn0search0\ue201"
	message := visibleFixtureMessage("answer", "assistant", "analysis", "text", "PRIVATE TEXT "+marker)
	message["metadata"] = map[string]any{"content_references": []any{map[string]any{"matched_text": marker, "items": []any{map[string]any{"title": "PRIVATE TITLE", "url": "https://example.org/private"}}}}, "citations": []any{map[string]any{"start_ix": 7, "end_ix": 7 + len([]rune(marker)), "metadata": map[string]any{"title": "PRIVATE LEGACY TITLE", "url": "https://example.org/private-legacy"}}}}
	applyFixtureStreamValue(t, &state, rootStreamEvent(message))
	applyFixtureStreamValue(t, &state, `{"p":"/message/channel","o":"replace","v":"final"}`)
	applyFixtureStreamValue(t, &state, string(jsonBytes(map[string]any{"p": "/message/content/parts/0", "o": "append", "v": "Public " + marker})))
	if strings.Contains(state.text, "PRIVATE") || strings.Contains(state.text, "example.org") {
		t.Fatal("hidden citation metadata survived reclassification")
	}
}

func TestStreamLegacyNullChannelIsReleasedOnlyAtCompletion(t *testing.T) {
	state := streamState{}
	message := visibleFixtureMessage("answer", "assistant", "", "text", "Legacy answer")
	message["channel"], message["recipient"] = nil, nil
	message["end_turn"] = true
	applyFixtureStreamValue(t, &state, rootStreamEvent(message))
	if state.text != "" {
		t.Fatal("unclassified legacy content published prematurely")
	}
	if problem := projectStreamMessage(&state, true); problem != nil || state.text != "Legacy answer" {
		t.Fatalf("completed legacy content lost: %q %v", state.text, problem)
	}
}

func TestStreamClassificationAfterEndTurnStillCannotLeakLegacyText(t *testing.T) {
	message := visibleFixtureMessage("hidden", "assistant", "", "text", "PRIVATE LATE CLASSIFICATION")
	message["end_turn"] = true
	events := []string{rootStreamEvent(message), `{"p":"/message/channel","o":"replace","v":"analysis"}`, rootStreamEvent(visibleFixtureMessage("answer", "assistant", "final", "text", "Public")), "[DONE]"}
	var progress strings.Builder
	result, problem := readStream(context.Background(), strings.NewReader(sse(events...)), "user", func(update Progress) { progress.WriteString(update.Text) })
	if problem != nil || strings.Contains(progress.String(), "PRIVATE") || result.(map[string]any)["message"].(Message).Text != "Public" {
		t.Fatalf("late classification escaped: %+v %v", result, problem)
	}
	message["content"].(map[string]any)["parts"] = []any{"Completed legacy answer"}
	result, problem = readStream(context.Background(), strings.NewReader(sse(rootStreamEvent(message))), "user", nil)
	if problem != nil || result.(map[string]any)["message"].(Message).Text != "Completed legacy answer" {
		t.Fatalf("legacy EOF completion lost: %+v %v", result, problem)
	}
}

func TestStreamCitationReferencesCanArriveAsOrderedPatches(t *testing.T) {
	state := streamState{}
	marker := "\ue200cite\ue202turn0search0\ue201"
	applyFixtureStreamValue(t, &state, rootStreamEvent(visibleFixtureMessage("answer", "assistant", "final", "text", "La réponse. "+marker)))
	applyFixtureStreamValue(t, &state, `{"p":"/message/metadata/content_references","o":"append","v":{"type":"webpage","items":[{"title":"Documentation","url":"https://example.org/source"}]}}`)
	applyFixtureStreamValue(t, &state, string(jsonBytes(map[string]any{"p": "/message/metadata/content_references/0/matched_text", "o": "replace", "v": marker})))
	if !strings.Contains(state.text, "[Documentation](https://example.org/source)") || strings.ContainsRune(state.text, '\ue200') {
		t.Fatalf("ordered citation fields lost: %q", state.text)
	}
	applyFixtureStreamValue(t, &state, rootStreamEvent(visibleFixtureMessage("next-answer", "assistant", "final", "text", "Autre réponse. "+marker)))
	if strings.Contains(state.text, "example.org") {
		t.Fatal("citation metadata survived a message reset")
	}
}
