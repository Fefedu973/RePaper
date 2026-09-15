package rmchat

import (
	"encoding/json"
	"net/url"
	"regexp"
	"strings"
	"unicode/utf16"
)

// ChatGPT Web uses private-use delimiters and separate citation metadata.
// Convert those presentation objects to ordinary Markdown without exposing
// tool payloads, inventing source URLs, or loading any resource.
var nativeMarker = regexp.MustCompile(`\x{E200}[^\x{E201}\r\n]*\x{E201}`)
var legacyMarker = regexp.MustCompile(`[〖【][0-9]+(?::[0-9]+)?†[^〗】\r\n]*[〗】]`)
var altLink = regexp.MustCompile(`\[([^\]\r\n]*)\]\(([^)\r\n]*)\)`)

type presentationSource struct {
	Title string `json:"title"`
	URL   string `json:"url"`
}

type presentationReference struct {
	MatchedText string               `json:"matched_text"`
	Type        string               `json:"type"`
	Invalid     bool                 `json:"invalid"`
	Alt         string               `json:"alt"`
	Items       []presentationSource `json:"items"`
	Sources     []presentationSource `json:"sources"`
	Title       string               `json:"title"`
	URL         string               `json:"url"`
}

func sourceMarkdown(source presentationSource) string {
	if len(source.URL) > 4096 || strings.ContainsAny(source.URL, "\r\n\x00") {
		return ""
	}
	u, err := url.Parse(source.URL)
	if err != nil || !strings.EqualFold(u.Scheme, "https") || u.Hostname() == "" || u.User != nil {
		return ""
	}
	label := strings.Join(strings.Fields(source.Title), " ")
	if label == "" {
		label = u.Hostname()
	}
	if len([]rune(label)) > 100 {
		label = string([]rune(label)[:100]) + "…"
	}
	label = strings.NewReplacer("\\", "\\\\", "[", "\\[", "]", "\\]", "*", "\\*", "_", "\\_", "`", "\\`", "<", "&lt;", ">", "&gt;").Replace(label)
	target := strings.NewReplacer("(", "%28", ")", "%29", "<", "%3C", ">", "%3E").Replace(u.String())
	return "[" + label + "](" + target + ")"
}

func referenceMarkdown(ref presentationReference) string {
	if ref.Invalid {
		return ""
	}
	items := append(append([]presentationSource{}, ref.Items...), ref.Sources...)
	items = append(items, presentationSource{Title: ref.Title, URL: ref.URL})
	if len(ref.Alt) <= 8192 {
		for _, match := range altLink.FindAllStringSubmatch(ref.Alt, 8) {
			items = append(items, presentationSource{Title: match[1], URL: match[2]})
		}
	}
	links, seen := []string{}, map[string]bool{}
	for _, source := range items {
		if len(links) >= 8 {
			break
		}
		link := sourceMarkdown(source)
		if link != "" && !seen[source.URL] {
			links = append(links, link)
			seen[source.URL] = true
		}
	}
	return strings.Join(links, " · ")
}

func citationOffsets(runes []rune, units []uint16, start, end int) string {
	if start < 0 || end <= start || end-start > 4096 {
		return ""
	}
	// Both older Unicode-codepoint exports and UTF-16 browser offsets occur in
	// external readers. Accept an offset only when it selects a complete marker.
	if end <= len(runes) {
		candidate := string(runes[start:end])
		if completeCitationMarker(candidate) {
			return candidate
		}
	}
	if end <= len(units) {
		candidate := string(utf16.Decode(units[start:end]))
		if completeCitationMarker(candidate) {
			return candidate
		}
	}
	return ""
}

func completeCitationMarker(text string) bool {
	return text != "" && (nativeMarker.FindString(text) == text || legacyMarker.FindString(text) == text)
}

func displayMessageText(text string, contentReferences, citations json.RawMessage) string {
	if !strings.ContainsAny(text, "\ue200\ue203\ue204〖【") {
		return text
	}
	replacements := map[string]string{}
	if len(contentReferences) <= 256<<10 {
		var refs []presentationReference
		if json.Unmarshal(contentReferences, &refs) == nil && len(refs) <= 512 {
			for _, ref := range refs {
				if completeCitationMarker(ref.MatchedText) {
					if rendered := referenceMarkdown(ref); rendered != "" {
						replacements[ref.MatchedText] = rendered
					}
				}
			}
		}
	}
	if len(citations) <= 256<<10 {
		var refs []struct {
			Start    int                `json:"start_ix"`
			End      int                `json:"end_ix"`
			Metadata presentationSource `json:"metadata"`
		}
		if json.Unmarshal(citations, &refs) == nil && len(refs) <= 512 {
			runes := []rune(text)
			units := utf16.Encode(runes)
			for _, ref := range refs {
				if marker, link := citationOffsets(runes, units, ref.Start, ref.End), sourceMarkdown(ref.Metadata); marker != "" && link != "" {
					replacements[marker] = link
				}
			}
		}
	}
	return transformOutsideCode(text, func(part string) string {
		part = strings.NewReplacer("\ue203", "", "\ue204", "").Replace(part)
		part = nativeMarker.ReplaceAllStringFunc(part, func(marker string) string {
			if replacement := replacements[marker]; replacement != "" {
				return replacement
			}
			body := strings.TrimSuffix(strings.TrimPrefix(marker, "\ue200"), "\ue201")
			kind, payload, _ := strings.Cut(body, "\ue202")
			switch kind {
			case "cite", "filecite", "citation", "navlist", "filenavlist":
				return "[source]"
			case "entity":
				var entity []string
				if len(payload) <= 4096 && json.Unmarshal([]byte(payload), &entity) == nil && len(entity) >= 2 && len(entity[1]) <= 512 {
					return strings.NewReplacer("[", "\\[", "]", "\\]", "<", "&lt;", ">", "&gt;").Replace(entity[1])
				}
			case "image_group", "image", "video":
				return "[Média]"
			}
			return "[Contenu interactif]"
		})
		part = legacyMarker.ReplaceAllStringFunc(part, func(marker string) string {
			if replacement := replacements[marker]; replacement != "" {
				return replacement
			}
			return "[source]"
		})
		// A streaming chunk can stop inside a marker. Wait for the complete
		// object in the next snapshot instead of painting its transport syntax.
		if index := strings.LastIndex(part, "\ue200"); index >= 0 && !strings.ContainsRune(part[index:], '\ue201') {
			part = part[:index]
		}
		return part
	})
}

// Keep code examples byte-for-byte, including literal native citation syntax.
// Fences and inline backtick runs are part of the Markdown grammar, not HTML.
func transformOutsideCode(text string, transform func(string) string) string {
	var out strings.Builder
	fence := byte(0)
	fenceSize, inlineSize := 0, 0
	for _, line := range strings.SplitAfter(text, "\n") {
		trimmed := strings.TrimLeft(line, " ")
		spaces := len(line) - len(trimmed)
		prefix := strings.TrimLeft(trimmed, "> ")
		if strings.HasPrefix(prefix, "- ") || strings.HasPrefix(prefix, "* ") || strings.HasPrefix(prefix, "+ ") {
			prefix = strings.TrimLeft(prefix[2:], " ")
		}
		run := 0
		if len(prefix) > 0 && (prefix[0] == '`' || prefix[0] == '~') {
			for run < len(prefix) && prefix[run] == prefix[0] {
				run++
			}
		}
		if run >= 3 && (fence == 0 || prefix[0] == fence && run >= fenceSize && strings.TrimSpace(prefix[run:]) == "") {
			if fence == 0 {
				fence, fenceSize = prefix[0], run
			} else {
				fence, fenceSize = 0, 0
			}
			inlineSize = 0
			out.WriteString(line)
			continue
		}
		if fence != 0 || spaces >= 4 || strings.HasPrefix(trimmed, "\t") {
			out.WriteString(line)
			continue
		}
		start := 0
		for i := 0; i < len(line); {
			if line[i] != '`' {
				i++
				continue
			}
			escapes := 0
			for j := i - 1; j >= 0 && line[j] == '\\'; j-- {
				escapes++
			}
			if inlineSize == 0 && escapes%2 != 0 {
				i++
				continue
			}
			if inlineSize == 0 {
				out.WriteString(transform(line[start:i]))
			} else {
				out.WriteString(line[start:i])
			}
			end := i
			for end < len(line) && line[end] == '`' {
				end++
			}
			if inlineSize == 0 {
				inlineSize = end - i
			} else if inlineSize == end-i {
				inlineSize = 0
			}
			out.WriteString(line[i:end])
			i, start = end, end
		}
		if inlineSize == 0 {
			out.WriteString(transform(line[start:]))
		} else {
			out.WriteString(line[start:])
		}
	}
	return out.String()
}
