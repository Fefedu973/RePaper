package rmchat

import (
	"encoding/json"
	"strings"
	"testing"
	"unicode/utf16"
)

func TestDisplayMessageTextResolvesGroupedCitations(t *testing.T) {
	marker := "\ue200cite\ue202turn0search1\ue202turn0search2\ue201"
	refs := jsonBytes([]any{map[string]any{"matched_text": marker, "type": "grouped_webpages", "items": []any{
		map[string]any{"title": "Source A", "url": "https://example.org/a"},
		map[string]any{"title": "Source A répétée", "url": "https://example.org/a"},
		map[string]any{"title": "Source B", "url": "https://example.org/b"},
	}}})
	text := "\ue203## Résultat\ue204\n\nUne **réponse** avec $x^2$ " + marker + "."
	want := "## Résultat\n\nUne **réponse** avec $x^2$ [Source A](https://example.org/a) · [Source B](https://example.org/b)."
	if got := displayMessageText(text, refs, nil); got != want {
		t.Fatalf("got %q, want %q", got, want)
	}
}

func TestDisplayMessageTextSupportsSingleSourceAndAlt(t *testing.T) {
	marker := "\ue200cite\ue202turn2search1\ue201"
	for name, ref := range map[string]any{
		"webpage": map[string]any{"matched_text": marker, "title": "Une source", "url": "https://example.org/page"},
		"sources": map[string]any{"matched_text": marker, "sources": []any{map[string]any{"title": "Une source", "url": "https://example.org/page"}}},
		"alt":     map[string]any{"matched_text": marker, "alt": "([Une source](https://example.org/page))"},
	} {
		t.Run(name, func(t *testing.T) {
			if got := displayMessageText("Voir "+marker, jsonBytes([]any{ref}), nil); got != "Voir [Une source](https://example.org/page)" {
				t.Fatal(got)
			}
		})
	}
}

func TestDisplayMessageTextRejectsUnsafeSourcesAndWhitespaceReferences(t *testing.T) {
	marker := "\ue200cite\ue202turn0search0\ue201"
	for _, target := range []string{"javascript:alert(1)", "file:///etc/passwd", "http://example.org", "https://name:secret@example.org", "https://example.org/\nsecret"} {
		refs := jsonBytes([]any{map[string]any{"matched_text": marker, "title": "unsafe-title", "url": target},
			map[string]any{"matched_text": " ", "type": "attribution", "url": "https://example.org"}})
		if got := displayMessageText("Deux mots "+marker, refs, nil); got != "Deux mots [source]" {
			t.Fatalf("unsafe citation: %q", got)
		}
	}
	refs := jsonBytes([]any{map[string]any{"matched_text": marker, "invalid": true, "url": "https://example.org"}})
	if got := displayMessageText(marker, refs, nil); got != "[source]" {
		t.Fatal(got)
	}
}

func TestDisplayMessageTextPreservesCodeAndEscapedBackticks(t *testing.T) {
	marker := "\ue200cite\ue202turn0search0\ue201"
	for _, code := range []string{
		"`" + marker + "`", "``" + marker + "``", "```text\n" + marker + "\n```\n",
		"~~~\n" + marker + "\n~~~\n", "    " + marker + "\n", "> ```text\n> " + marker + "\n> ```\n",
		"- ```text\n  " + marker + "\n  ```\n", "```text\n```still code\n" + marker + "\n```\n",
	} {
		if got := displayMessageText(code, nil, nil); got != code {
			t.Fatalf("code changed: %q => %q", code, got)
		}
	}
	if got := displayMessageText("Un accent \\` puis "+marker, nil, nil); got != "Un accent \\` puis [source]" {
		t.Fatal(got)
	}
}

func TestDisplayMessageTextHoldsIncompleteStreamingMarkers(t *testing.T) {
	for _, suffix := range []string{"\ue200", "\ue200cite", "\ue200cite\ue202turn0search0"} {
		if got := displayMessageText("Réponse "+suffix, nil, nil); got != "Réponse " {
			t.Fatalf("transport fragment shown: %q", got)
		}
	}
	if got := displayMessageText("Réponse \ue200cite\ue202turn0search0\ue201.", nil, nil); got != "Réponse [source]." {
		t.Fatal(got)
	}
}

func TestDisplayMessageTextKeepsEntityLabelWithoutInteractivePayload(t *testing.T) {
	text := "Visitez \ue200entity\ue202[\"place\",\"Paris\",\"private transport description\"]\ue201.\n\ue200image_group\ue202{\"query\":\"internal-query\"}\ue201"
	if got := displayMessageText(text, nil, nil); got != "Visitez Paris.\n[Média]" {
		t.Fatal(got)
	}
	if got := displayMessageText("\ue200image_group\ue202"+strings.Repeat("internal payload", 1000)+"\ue201", nil, nil); got != "[Média]" {
		t.Fatal("large interactive payload was exposed")
	}
}

func TestDisplayMessageTextSupportsLegacyCitationOffsetsWithUnicode(t *testing.T) {
	for _, marker := range []string{"\ue200cite\ue202turn1search0\ue201", "〖2†source〗", "【2:1†source】"} {
		prefix := "🙂 Une réponse "
		text := prefix + marker + "."
		for _, start := range []int{len([]rune(prefix)), len(utf16.Encode([]rune(prefix)))} {
			citations := jsonBytes([]any{map[string]any{"start_ix": start, "end_ix": start + len([]rune(marker)),
				"metadata": map[string]any{"title": "Document", "url": "https://example.org/document"}}})
			if got := displayMessageText(text, nil, citations); got != prefix+"[Document](https://example.org/document)." {
				t.Fatalf("offset citation: %q", got)
			}
		}
	}
}

func TestDisplayMessageTextDoesNotExecuteOrInjectMetadataMarkdown(t *testing.T) {
	marker := "\ue200cite\ue202turn0search0\ue201"
	refs := jsonBytes([]any{map[string]any{"matched_text": marker, "title": "[x](javascript:bad) <img>", "url": "https://example.org/(path)"}})
	got := displayMessageText(marker, refs, nil)
	if !strings.Contains(got, "\\[x\\]") || strings.Contains(got, "<img>") || !strings.Contains(got, "https://example.org/%28path%29") {
		t.Fatal(got)
	}
	if got := displayMessageText("Texte intact", json.RawMessage("broken"), nil); got != "Texte intact" {
		t.Fatal(got)
	}
}
