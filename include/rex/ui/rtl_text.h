#pragma once

// Right-to-left text for the ImGui settings UI (launcher and F1 overlay).
// ImGui draws glyphs one code point at a time, left to right, without shaping,
// so Arabic text is converted here into what ImGui can draw: letters replaced
// by their contextual presentation forms (Unicode Arabic Presentation Forms-B,
// with the lam-alef ligatures), then put in visual order (right-to-left runs
// reversed, left-to-right runs such as numbers and Latin names kept, mirrored
// brackets). Segoe UI, the UI font, has all these glyphs.
//
// This covers UI strings (Arabic letters, common diacritics, digits, Latin
// words, punctuation); it is not a full Unicode bidi implementation.

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rex::ui::rtl {

inline std::u32string DecodeUtf8(std::string_view text) {
  std::u32string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const uint8_t c = uint8_t(text[i]);
    char32_t cp = 0xFFFD;
    size_t length = 1;
    if (c < 0x80) {
      cp = c;
    } else if ((c >> 5) == 0x6 && i + 1 < text.size()) {
      cp = char32_t(c & 0x1F) << 6 | char32_t(uint8_t(text[i + 1]) & 0x3F);
      length = 2;
    } else if ((c >> 4) == 0xE && i + 2 < text.size()) {
      cp = char32_t(c & 0x0F) << 12 | char32_t(uint8_t(text[i + 1]) & 0x3F) << 6 |
           char32_t(uint8_t(text[i + 2]) & 0x3F);
      length = 3;
    } else if ((c >> 3) == 0x1E && i + 3 < text.size()) {
      cp = char32_t(c & 0x07) << 18 | char32_t(uint8_t(text[i + 1]) & 0x3F) << 12 |
           char32_t(uint8_t(text[i + 2]) & 0x3F) << 6 | char32_t(uint8_t(text[i + 3]) & 0x3F);
      length = 4;
    }
    out.push_back(cp);
    i += length;
  }
  return out;
}

inline std::string EncodeUtf8(std::u32string_view text) {
  std::string out;
  out.reserve(text.size() * 2);
  for (char32_t cp : text) {
    if (cp < 0x80) {
      out.push_back(char(cp));
    } else if (cp < 0x800) {
      out.push_back(char(0xC0 | (cp >> 6)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(char(0xE0 | (cp >> 12)));
      out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(char(0xF0 | (cp >> 18)));
      out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

// Contextual forms of U+0621..U+064A: isolated, final, initial, medial (0 =
// the letter has no such form, it does not join on that side).
struct ArabicForms {
  char32_t isolated, final_form, initial, medial;
};

inline const ArabicForms* FormsOf(char32_t cp) {
  static constexpr ArabicForms kForms[] = {
      {0xFE80, 0, 0, 0},                // 0621 hamza
      {0xFE81, 0xFE82, 0, 0},           // 0622 alef madda
      {0xFE83, 0xFE84, 0, 0},           // 0623 alef hamza above
      {0xFE85, 0xFE86, 0, 0},           // 0624 waw hamza
      {0xFE87, 0xFE88, 0, 0},           // 0625 alef hamza below
      {0xFE89, 0xFE8A, 0xFE8B, 0xFE8C}, // 0626 yeh hamza
      {0xFE8D, 0xFE8E, 0, 0},           // 0627 alef
      {0xFE8F, 0xFE90, 0xFE91, 0xFE92}, // 0628 beh
      {0xFE93, 0xFE94, 0, 0},           // 0629 teh marbuta
      {0xFE95, 0xFE96, 0xFE97, 0xFE98}, // 062A teh
      {0xFE99, 0xFE9A, 0xFE9B, 0xFE9C}, // 062B theh
      {0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0}, // 062C jeem
      {0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4}, // 062D hah
      {0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8}, // 062E khah
      {0xFEA9, 0xFEAA, 0, 0},           // 062F dal
      {0xFEAB, 0xFEAC, 0, 0},           // 0630 thal
      {0xFEAD, 0xFEAE, 0, 0},           // 0631 reh
      {0xFEAF, 0xFEB0, 0, 0},           // 0632 zain
      {0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4}, // 0633 seen
      {0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8}, // 0634 sheen
      {0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC}, // 0635 sad
      {0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0}, // 0636 dad
      {0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4}, // 0637 tah
      {0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8}, // 0638 zah
      {0xFEC9, 0xFECA, 0xFECB, 0xFECC}, // 0639 ain
      {0xFECD, 0xFECE, 0xFECF, 0xFED0}, // 063A ghain
  };
  static constexpr ArabicForms kForms2[] = {
      {0xFED1, 0xFED2, 0xFED3, 0xFED4}, // 0641 feh
      {0xFED5, 0xFED6, 0xFED7, 0xFED8}, // 0642 qaf
      {0xFED9, 0xFEDA, 0xFEDB, 0xFEDC}, // 0643 kaf
      {0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0}, // 0644 lam
      {0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4}, // 0645 meem
      {0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8}, // 0646 noon
      {0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC}, // 0647 heh
      {0xFEED, 0xFEEE, 0, 0},           // 0648 waw
      {0xFEEF, 0xFEF0, 0xFBE8, 0xFBE9}, // 0649 alef maksura
      {0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4}, // 064A yeh
  };
  if (cp >= 0x0621 && cp <= 0x063A) return &kForms[cp - 0x0621];
  if (cp >= 0x0641 && cp <= 0x064A) return &kForms2[cp - 0x0641];
  return nullptr;
}

// Combining marks (harakat, superscript alef): drawn over the previous letter
// and transparent for joining.
inline bool IsArabicMark(char32_t cp) {
  return (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670 || (cp >= 0x06D6 && cp <= 0x06ED);
}

inline bool IsTatweel(char32_t cp) { return cp == 0x0640; }

// Joins towards the following letter (in logical order).
inline bool JoinsNext(char32_t cp) {
  if (IsTatweel(cp)) return true;
  const ArabicForms* forms = FormsOf(cp);
  return forms && forms->initial;
}

// Accepts a join from the preceding letter.
inline bool JoinsPrevious(char32_t cp) {
  if (IsTatweel(cp)) return true;
  const ArabicForms* forms = FormsOf(cp);
  return forms && forms->final_form;
}

inline bool IsRtlCodePoint(char32_t cp) {
  return (cp >= 0x0590 && cp <= 0x08FF) || (cp >= 0xFB1D && cp <= 0xFDFF) ||
         (cp >= 0xFE70 && cp <= 0xFEFF);
}

inline bool IsLtrCodePoint(char32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
         (cp >= 0x00C0 && cp <= 0x024F) || (cp >= 0x0660 && cp <= 0x0669);
}

inline bool ContainsRtl(std::string_view text) {
  for (char32_t cp : DecodeUtf8(text)) {
    if (IsRtlCodePoint(cp)) return true;
  }
  return false;
}

// Letters to contextual presentation forms, in logical order.
inline std::u32string ShapeArabic(std::u32string_view text) {
  std::u32string out;
  out.reserve(text.size());
  auto neighbour = [&](size_t index, int step) -> char32_t {
    for (ptrdiff_t i = ptrdiff_t(index) + step; i >= 0 && size_t(i) < text.size(); i += step) {
      if (!IsArabicMark(text[size_t(i)])) return text[size_t(i)];
    }
    return 0;
  };
  for (size_t i = 0; i < text.size(); ++i) {
    const char32_t cp = text[i];
    const ArabicForms* forms = FormsOf(cp);
    if (!forms) {
      out.push_back(cp);
      continue;
    }
    const char32_t previous = neighbour(i, -1);
    const bool joined_before = previous && JoinsNext(previous) && forms->final_form;
    // Lam + alef: one ligature glyph (isolated or final).
    if (cp == 0x0644) {
      size_t next_index = i + 1;
      while (next_index < text.size() && IsArabicMark(text[next_index])) ++next_index;
      if (next_index < text.size()) {
        char32_t ligature = 0;
        switch (text[next_index]) {
          case 0x0622:
            ligature = 0xFEF5;
            break;
          case 0x0623:
            ligature = 0xFEF7;
            break;
          case 0x0625:
            ligature = 0xFEF9;
            break;
          case 0x0627:
            ligature = 0xFEFB;
            break;
        }
        if (ligature) {
          out.push_back(joined_before ? ligature + 1 : ligature);
          for (size_t k = i + 1; k < next_index; ++k) out.push_back(text[k]);
          i = next_index;
          continue;
        }
      }
    }
    const char32_t next = neighbour(i, 1);
    const bool joins_after = next && forms->initial && JoinsPrevious(next);
    if (joined_before && joins_after) {
      out.push_back(forms->medial);
    } else if (joined_before) {
      out.push_back(forms->final_form);
    } else if (joins_after) {
      out.push_back(forms->initial);
    } else {
      out.push_back(forms->isolated);
    }
  }
  return out;
}

inline char32_t MirrorInRtl(char32_t cp) {
  switch (cp) {
    case '(':
      return ')';
    case ')':
      return '(';
    case '[':
      return ']';
    case ']':
      return '[';
    case '{':
      return '}';
    case '}':
      return '{';
    case '<':
      return '>';
    case '>':
      return '<';
    case 0x00AB:
      return 0x00BB;
    case 0x00BB:
      return 0x00AB;
  }
  return cp;
}

// One line in visual (left-to-right drawing) order, right-to-left base
// direction: left-to-right runs (numbers, Latin words and the spaces or
// punctuation between them, like "FSR 3.1" or "1280 x 720") keep their order,
// everything else is reversed with brackets mirrored.
inline std::u32string VisualOrder(std::u32string_view line) {
  const size_t n = line.size();
  // 1 = strong left-to-right, 2 = right-to-left, 0 = neutral.
  std::vector<uint8_t> kind(n);
  for (size_t i = 0; i < n; ++i) {
    kind[i] = IsLtrCodePoint(line[i]) ? 1 : (IsRtlCodePoint(line[i]) ? 2 : 0);
  }
  // Bracket pairs take one direction (the bidi rule N0): right-to-left when
  // they hold right-to-left text, left-to-right when they hold only
  // left-to-right text after left-to-right text, as in "XeSS (MIT)", so the
  // pair stays with its Latin run.
  {
    std::vector<std::pair<size_t, size_t>> pairs;
    std::vector<size_t> open;
    for (size_t i = 0; i < n; ++i) {
      const char32_t cp = line[i];
      if (cp == '(' || cp == '[' || cp == '{') {
        if (open.size() < 63) open.push_back(i);
      } else if (cp == ')' || cp == ']' || cp == '}') {
        const char32_t opening = cp == ')' ? '(' : (cp == ']' ? '[' : '{');
        for (size_t depth = open.size(); depth > 0; --depth) {
          if (line[open[depth - 1]] == opening) {
            pairs.emplace_back(open[depth - 1], i);
            open.resize(depth - 1);
            break;
          }
        }
      }
    }
    std::sort(pairs.begin(), pairs.end());
    for (const auto& [first, last] : pairs) {
      bool ltr_inside = false, rtl_inside = false;
      for (size_t k = first + 1; k < last; ++k) {
        ltr_inside |= kind[k] == 1;
        rtl_inside |= kind[k] == 2;
      }
      if (!ltr_inside && !rtl_inside) continue;
      uint8_t resolved = 2;
      if (!rtl_inside) {
        uint8_t before = 2;  // start of the line: the right-to-left base
        for (size_t k = first; k > 0; --k) {
          if (kind[k - 1] != 0) {
            before = kind[k - 1];
            break;
          }
        }
        resolved = before;
      }
      kind[first] = kind[last] = resolved;
    }
  }
  // Neutrals between two left-to-right characters join that run.
  for (size_t i = 0; i < n;) {
    if (kind[i] != 0) {
      ++i;
      continue;
    }
    size_t end = i;
    while (end < n && kind[end] == 0) ++end;
    const bool ltr_before = i > 0 && kind[i - 1] == 1;
    const bool ltr_after = end < n && kind[end] == 1;
    for (size_t k = i; k < end; ++k) kind[k] = (ltr_before && ltr_after) ? 1 : 2;
    i = end;
  }
  std::u32string out;
  out.reserve(n);
  for (size_t end = n; end > 0;) {
    size_t start = end - 1;
    if (kind[start] == 1) {
      while (start > 0 && kind[start - 1] == 1) --start;
      out.append(line.substr(start, end - start));
    } else {
      // Marks end up before their letter: the UI font draws a mark from the
      // pen position over the glyph that follows it.
      out.push_back(MirrorInRtl(line[start]));
    }
    end = start;
  }
  return out;
}

// A UTF-8 line ready for ImGui: shaped and in visual order. Text without
// right-to-left letters is returned unchanged.
inline std::string VisualLine(std::string_view logical) {
  if (!ContainsRtl(logical)) return std::string(logical);
  return EncodeUtf8(VisualOrder(ShapeArabic(DecodeUtf8(logical))));
}

// Word-wraps logical text to a width (measured on the visual form of each
// candidate line) and returns the visual lines, top to bottom.
inline std::vector<std::string> WrapVisual(std::string_view logical, float width,
                                           const std::function<float(const std::string&)>& measure) {
  std::vector<std::string> lines;
  size_t paragraph_start = 0;
  while (paragraph_start <= logical.size()) {
    size_t paragraph_end = logical.find('\n', paragraph_start);
    if (paragraph_end == std::string_view::npos) paragraph_end = logical.size();
    const std::string_view paragraph =
        logical.substr(paragraph_start, paragraph_end - paragraph_start);
    std::string current;
    size_t word_start = 0;
    while (word_start <= paragraph.size()) {
      size_t word_end = paragraph.find(' ', word_start);
      if (word_end == std::string_view::npos) word_end = paragraph.size();
      const std::string word(paragraph.substr(word_start, word_end - word_start));
      std::string candidate = current.empty() ? word : current + " " + word;
      if (!current.empty() && measure(VisualLine(candidate)) > width) {
        lines.push_back(VisualLine(current));
        current = word;
      } else {
        current = std::move(candidate);
      }
      word_start = word_end + 1;
    }
    lines.push_back(VisualLine(current));
    if (paragraph_end >= logical.size()) break;
    paragraph_start = paragraph_end + 1;
  }
  return lines;
}

}  // namespace rex::ui::rtl
