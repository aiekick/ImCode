/*
MIT License

Copyright (c) 2025-2026 Stephane Cuillerdier (aka Aiekick)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "ImCode.h"

#include <cfloat>         // FLT_MAX
#include <cstdio>         // snprintf
#include <cstring>        // strchr
#include <unordered_set>  // keyword / type lookup
#include <utility>        // std::move

namespace im {

// ---------------------------------------------------------------------------
// File-local C++ lexer (v1). Per-line, stateful across lines for block comments.
// The data-driven LanguageDef + threaded incremental lexing replace this later.
// ---------------------------------------------------------------------------
namespace {

struct LexToken {
    int32_t start{};
    int32_t length{};
    int32_t color{};
};

inline bool isLetter(char aChar) { return (aChar >= 'a' && aChar <= 'z') || (aChar >= 'A' && aChar <= 'Z') || aChar == '_'; }
inline bool isDigit(char aChar)  { return aChar >= '0' && aChar <= '9'; }
inline bool isAlnum(char aChar)  { return isLetter(aChar) || isDigit(aChar); }
inline bool isOpChar(char aChar) { return aChar != 0 && std::strchr("+-*/%=<>!&|^~?:.", aChar) != nullptr; }
inline bool isPunct(char aChar)  { return aChar != 0 && std::strchr("(){}[];,", aChar) != nullptr; }

int32_t classifyCppWord(const std::string& aWord) {
    static const std::unordered_set<std::string> keywords = {
        "alignas", "alignof", "asm", "break", "case", "catch", "class", "concept", "const", "consteval", "constexpr",
        "constinit", "const_cast", "continue", "decltype", "default", "delete", "do", "dynamic_cast", "else", "enum",
        "explicit", "export", "extern", "false", "for", "friend", "goto", "if", "inline", "mutable", "namespace", "new",
        "noexcept", "nullptr", "operator", "private", "protected", "public", "register", "reinterpret_cast", "requires",
        "return", "sizeof", "static", "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local",
        "throw", "true", "try", "typedef", "typeid", "typename", "union", "using", "virtual", "volatile", "while"};
    static const std::unordered_set<std::string> types = {
        "auto", "bool", "char", "char8_t", "char16_t", "char32_t", "double", "float", "int", "long", "short", "signed",
        "unsigned", "void", "wchar_t", "size_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t",
        "uint32_t", "uint64_t"};
    if (keywords.count(aWord) != 0) return Code::Col_Keyword;
    if (types.count(aWord) != 0) return Code::Col_Type;
    return Code::Col_Identifier;
}

void lexCppLine(const std::string& aLine, bool aInBlock, std::vector<LexToken>& aoTokens, bool& aoInBlock) {
    aoTokens.clear();
    const int32_t n = (int32_t)aLine.size();
    int32_t i = 0;
    bool block = aInBlock;
    while (i < n) {
        if (block) {
            const int32_t start = i;
            while (i < n) {
                if (aLine[(size_t)i] == '*' && i + 1 < n && aLine[(size_t)(i + 1)] == '/') { i += 2; block = false; break; }
                ++i;
            }
            aoTokens.push_back({start, i - start, Code::Col_Comment});
            continue;
        }
        const char c = aLine[(size_t)i];
        if (c == ' ' || c == '\t') { ++i; continue; }
        if (c == '/' && i + 1 < n && aLine[(size_t)(i + 1)] == '/') {
            aoTokens.push_back({i, n - i, Code::Col_Comment});
            i = n;
            continue;
        }
        if (c == '/' && i + 1 < n && aLine[(size_t)(i + 1)] == '*') {
            const int32_t start = i;
            i += 2;
            block = true;
            while (i < n) {
                if (aLine[(size_t)i] == '*' && i + 1 < n && aLine[(size_t)(i + 1)] == '/') { i += 2; block = false; break; }
                ++i;
            }
            aoTokens.push_back({start, i - start, Code::Col_Comment});
            continue;
        }
        if (c == '#') {
            aoTokens.push_back({i, n - i, Code::Col_Preproc});
            i = n;
            continue;
        }
        if (c == '"') {
            const int32_t start = i;
            ++i;
            while (i < n) {
                if (aLine[(size_t)i] == '\\' && i + 1 < n) { i += 2; continue; }
                if (aLine[(size_t)i] == '"') { ++i; break; }
                ++i;
            }
            aoTokens.push_back({start, i - start, Code::Col_String});
            continue;
        }
        if (c == '\'') {
            const int32_t start = i;
            ++i;
            while (i < n) {
                if (aLine[(size_t)i] == '\\' && i + 1 < n) { i += 2; continue; }
                if (aLine[(size_t)i] == '\'') { ++i; break; }
                ++i;
            }
            aoTokens.push_back({start, i - start, Code::Col_Char});
            continue;
        }
        if (isDigit(c) || (c == '.' && i + 1 < n && isDigit(aLine[(size_t)(i + 1)]))) {
            const int32_t start = i;
            while (i < n) {
                const char d = aLine[(size_t)i];
                if (isAlnum(d) || d == '.' || d == '\'') {
                    ++i;
                } else if ((d == '+' || d == '-') && i > start && (aLine[(size_t)(i - 1)] == 'e' || aLine[(size_t)(i - 1)] == 'E')) {
                    ++i;
                } else {
                    break;
                }
            }
            aoTokens.push_back({start, i - start, Code::Col_Number});
            continue;
        }
        if (isLetter(c)) {
            const int32_t start = i;
            while (i < n && isAlnum(aLine[(size_t)i])) ++i;
            aoTokens.push_back({start, i - start, classifyCppWord(aLine.substr((size_t)start, (size_t)(i - start)))});
            continue;
        }
        if (isOpChar(c)) {
            const int32_t start = i;
            while (i < n && isOpChar(aLine[(size_t)i])) ++i;
            aoTokens.push_back({start, i - start, Code::Col_Operator});
            continue;
        }
        if (isPunct(c)) {
            aoTokens.push_back({i, 1, Code::Col_Punctuation});
            ++i;
            continue;
        }
        ++i;  // skip unknown byte
    }
    aoInBlock = block;
}

}  // namespace

// NOTE v1: caret columns are byte offsets into the line (ASCII assumption).
// UTF-8 codepoint stepping is a near-term refinement (design invariant).

// ---------------------------------------------------------------------------
// Internal state (Pimpl). v1 backing store is a simple vector<string> of lines.
// ---------------------------------------------------------------------------
struct Code::Impl {
    enum class EditKind { Typing, Deleting, Other };
    struct UndoEntry {
        std::vector<std::string> lines;
        Pos cursor{};
        Pos anchor{};
    };

    Config m_config{};
    Style  m_style{};

    std::vector<std::string>           m_lines{std::string()};  // always at least one (empty) line
    std::string                        m_languageName;
    size_t                             m_maxLineChars{};
    std::vector<std::vector<LexToken>> m_lineTokens;            // cached per-line coloring (parallel to m_lines)

    // caret / selection (single caret + range, no multi-cursor)
    Pos     m_cursor{};
    Pos     m_anchor{};          // selection anchor; selection is ordered(anchor, cursor)
    int32_t m_desiredColumn{};   // sticky column for vertical moves
    bool    m_ensureCursorVisible{};
    float   m_zoom{1.0f};        // local editor zoom (ctrl + mouse wheel), decoupled from global font

    // undo / redo
    std::vector<UndoEntry> m_undoStack;
    std::vector<UndoEntry> m_redoStack;
    EditKind               m_lastEditKind{EditKind::Other};

    // host-pluggable providers (null -> builtin default; wired in later steps)
    DataSource*         m_dataSource{};
    Executor*           m_executor{};
    Matcher*            m_matcher{};
    CompletionProvider* m_completion{};
    std::function<void(int32_t)> m_gutterClick;

    Impl() {
        m_style.colors[Col_Background]  = IM_COL32(30, 30, 30, 255);
        m_style.colors[Col_Default]     = IM_COL32(220, 220, 220, 255);
        m_style.colors[Col_LineNumber]  = IM_COL32(120, 120, 120, 255);
        m_style.colors[Col_Gutter]      = IM_COL32(24, 24, 24, 255);
        m_style.colors[Col_CurrentLine] = IM_COL32(42, 42, 42, 255);
        m_style.colors[Col_Selection]   = IM_COL32(60, 90, 140, 120);
        m_style.colors[Col_Caret]       = IM_COL32(230, 230, 230, 255);
        m_style.colors[Col_Keyword]     = IM_COL32(86, 156, 214, 255);
        m_style.colors[Col_Type]        = IM_COL32(78, 201, 176, 255);
        m_style.colors[Col_String]      = IM_COL32(214, 157, 133, 255);
        m_style.colors[Col_Char]        = IM_COL32(214, 157, 133, 255);
        m_style.colors[Col_Number]      = IM_COL32(181, 206, 168, 255);
        m_style.colors[Col_Comment]     = IM_COL32(106, 153, 85, 255);
        m_style.colors[Col_Preproc]     = IM_COL32(155, 155, 155, 255);
        m_style.colors[Col_Operator]    = IM_COL32(200, 200, 200, 255);
        m_style.colors[Col_Punctuation] = IM_COL32(200, 200, 200, 255);
        for (int32_t idx = 0; idx < Col_COUNT; ++idx) {
            if (m_style.colors[idx] == 0) {
                m_style.colors[idx] = m_style.colors[Col_Default];
            }
        }
    }

    // --- helpers ---------------------------------------------------------
    int32_t lastLineIndex() const { return (int32_t)m_lines.size() - 1; }
    int32_t lineLen(int32_t aLine) const {
        if (aLine < 0 || aLine > lastLineIndex()) return 0;
        return (int32_t)m_lines[(size_t)aLine].size();
    }
    Pos clampPos(Pos aPos) const {
        if (aPos.line < 0) aPos.line = 0;
        if (aPos.line > lastLineIndex()) aPos.line = lastLineIndex();
        const int32_t len = lineLen(aPos.line);
        if (aPos.column < 0) aPos.column = 0;
        if (aPos.column > len) aPos.column = len;
        return aPos;
    }
    static bool posLess(const Pos& a, const Pos& b) { return a.line < b.line || (a.line == b.line && a.column < b.column); }
    static bool posEqual(const Pos& a, const Pos& b) { return a.line == b.line && a.column == b.column; }
    bool hasSelection() const { return !posEqual(m_cursor, m_anchor); }
    bool selectionMultiline() const {
        Pos a, b;
        orderedSelection(a, b);
        return a.line != b.line;
    }
    void orderedSelection(Pos& aoStart, Pos& aoEnd) const {
        if (posLess(m_cursor, m_anchor)) { aoStart = m_cursor; aoEnd = m_anchor; }
        else { aoStart = m_anchor; aoEnd = m_cursor; }
    }

    void recomputeMaxLineChars() {
        size_t maxc = 0;
        for (const auto& line : m_lines) {
            if (line.size() > maxc) maxc = line.size();
        }
        m_maxLineChars = maxc;
    }
    bool isCppLike() const {
        return m_languageName == "cpp" || m_languageName == "c" || m_languageName == "glsl";
    }
    void relex() {
        m_lineTokens.assign(m_lines.size(), std::vector<LexToken>());
        if (!isCppLike()) return;
        bool block = false;
        for (size_t i = 0; i < m_lines.size(); ++i) {
            bool outBlock = false;
            lexCppLine(m_lines[i], block, m_lineTokens[i], outBlock);
            block = outBlock;
        }
    }
    void afterContentChanged() {
        m_cursor = clampPos(m_cursor);
        m_anchor = clampPos(m_anchor);
        m_desiredColumn = m_cursor.column;
        recomputeMaxLineChars();
        relex();
        m_ensureCursorVisible = true;
    }

    // --- caret moves -----------------------------------------------------
    void place(Pos aPos, bool aKeepSelection) {
        m_cursor = clampPos(aPos);
        if (!aKeepSelection) m_anchor = m_cursor;
        m_ensureCursorVisible = true;
        m_lastEditKind = EditKind::Other;  // a caret move breaks the typing/deleting coalesce group
    }
    void moveLeft(bool aShift) {
        Pos cur = m_cursor;
        if (cur.column > 0) cur.column--;
        else if (cur.line > 0) { cur.line--; cur.column = lineLen(cur.line); }
        place(cur, aShift);
        m_desiredColumn = m_cursor.column;
    }
    void moveRight(bool aShift) {
        Pos cur = m_cursor;
        if (cur.column < lineLen(cur.line)) cur.column++;
        else if (cur.line < lastLineIndex()) { cur.line++; cur.column = 0; }
        place(cur, aShift);
        m_desiredColumn = m_cursor.column;
    }
    void moveVertical(int32_t aDelta, bool aShift) {
        Pos cur = m_cursor;
        cur.line += aDelta;
        cur.column = m_desiredColumn;
        place(cur, aShift);
    }
    void moveHome(bool aShift) {
        const std::string& line = m_lines[(size_t)m_cursor.line];
        int32_t firstNonWs = 0;
        while (firstNonWs < (int32_t)line.size() && (line[(size_t)firstNonWs] == ' ' || line[(size_t)firstNonWs] == '\t')) ++firstNonWs;
        Pos cur = m_cursor;
        cur.column = (cur.column == firstNonWs) ? 0 : firstNonWs;
        place(cur, aShift);
        m_desiredColumn = m_cursor.column;
    }
    void moveEnd(bool aShift) {
        Pos cur = m_cursor;
        cur.column = lineLen(cur.line);
        place(cur, aShift);
        m_desiredColumn = m_cursor.column;
    }
    void moveDocStart(bool aShift) { place(Pos{0, 0}, aShift); m_desiredColumn = 0; }
    void moveDocEnd(bool aShift) { place(Pos{lastLineIndex(), lineLen(lastLineIndex())}, aShift); m_desiredColumn = m_cursor.column; }
    void selectAll() {
        m_anchor = Pos{0, 0};
        place(Pos{lastLineIndex(), lineLen(lastLineIndex())}, true);
    }

    // --- undo / redo -----------------------------------------------------
    void snapshotForUndo(EditKind aKind) {
        const bool coalesce = (aKind != EditKind::Other) && (aKind == m_lastEditKind) && !m_undoStack.empty();
        if (!coalesce) {
            UndoEntry entry;
            entry.lines = m_lines;
            entry.cursor = m_cursor;
            entry.anchor = m_anchor;
            m_undoStack.push_back(std::move(entry));
            const int32_t maxDepth = (m_config.maxUndoDepth > 0) ? m_config.maxUndoDepth : 1000;
            while ((int32_t)m_undoStack.size() > maxDepth) m_undoStack.erase(m_undoStack.begin());
            m_redoStack.clear();
        }
        m_lastEditKind = aKind;
    }
    void undo() {
        if (m_undoStack.empty()) return;
        UndoEntry current;
        current.lines = m_lines;
        current.cursor = m_cursor;
        current.anchor = m_anchor;
        m_redoStack.push_back(std::move(current));
        UndoEntry entry = std::move(m_undoStack.back());
        m_undoStack.pop_back();
        m_lines = std::move(entry.lines);
        m_cursor = entry.cursor;
        m_anchor = entry.anchor;
        m_lastEditKind = EditKind::Other;
        afterContentChanged();
    }
    void redo() {
        if (m_redoStack.empty()) return;
        UndoEntry current;
        current.lines = m_lines;
        current.cursor = m_cursor;
        current.anchor = m_anchor;
        m_undoStack.push_back(std::move(current));
        UndoEntry entry = std::move(m_redoStack.back());
        m_redoStack.pop_back();
        m_lines = std::move(entry.lines);
        m_cursor = entry.cursor;
        m_anchor = entry.anchor;
        m_lastEditKind = EditKind::Other;
        afterContentChanged();
    }

    // --- mutations (v1: byte-based; UTF-8 codepoint stepping is a refinement) ---
    void deleteRange(Pos aStart, Pos aEnd) {  // ordered: aStart <= aEnd
        if (aStart.line == aEnd.line) {
            m_lines[(size_t)aStart.line].erase((size_t)aStart.column, (size_t)(aEnd.column - aStart.column));
        } else {
            const std::string tail = m_lines[(size_t)aEnd.line].substr((size_t)aEnd.column);
            m_lines[(size_t)aStart.line].erase((size_t)aStart.column);
            m_lines[(size_t)aStart.line] += tail;
            m_lines.erase(m_lines.begin() + aStart.line + 1, m_lines.begin() + aEnd.line + 1);
        }
    }
    void deleteSelectionAndCollapse() {
        Pos selStart, selEnd;
        orderedSelection(selStart, selEnd);
        deleteRange(selStart, selEnd);
        m_cursor = selStart;
        m_anchor = selStart;
    }
    Pos insertTextAt(Pos aPos, const std::string& aText) {
        std::vector<std::string> segs;
        segs.emplace_back();
        for (size_t i = 0; i < aText.size(); ++i) {
            const char ch = aText[i];
            if (ch == '\n') segs.emplace_back();
            else if (ch != '\r') segs.back().push_back(ch);
        }
        std::string& line = m_lines[(size_t)aPos.line];
        const std::string tail = line.substr((size_t)aPos.column);
        line.erase((size_t)aPos.column);
        if (segs.size() == 1) {
            line += segs[0];
            const Pos end{aPos.line, (int32_t)line.size()};
            line += tail;
            return end;
        }
        line += segs[0];
        int32_t insertAt = aPos.line + 1;
        for (size_t s = 1; s < segs.size(); ++s) {
            m_lines.insert(m_lines.begin() + insertAt, segs[s]);
            ++insertAt;
        }
        const int32_t endLine = aPos.line + (int32_t)segs.size() - 1;
        const int32_t endCol  = (int32_t)segs[segs.size() - 1].size();
        m_lines[(size_t)endLine] += tail;
        return Pos{endLine, endCol};
    }
    void typeText(const std::string& aText, EditKind aKind) {
        if (aText.empty() && !hasSelection()) return;
        snapshotForUndo(aKind);
        if (hasSelection()) deleteSelectionAndCollapse();
        const Pos end = insertTextAt(m_cursor, aText);
        m_cursor = end;
        m_anchor = end;
        afterContentChanged();
    }
    void backspace() {
        if (!hasSelection() && m_cursor.line == 0 && m_cursor.column == 0) return;
        snapshotForUndo(EditKind::Deleting);
        if (hasSelection()) {
            deleteSelectionAndCollapse();
        } else {
            Pos cur = m_cursor;
            if (cur.column > 0) {
                m_lines[(size_t)cur.line].erase((size_t)(cur.column - 1), 1);
                cur.column--;
            } else if (cur.line > 0) {
                const int32_t prevLen = lineLen(cur.line - 1);
                m_lines[(size_t)(cur.line - 1)] += m_lines[(size_t)cur.line];
                m_lines.erase(m_lines.begin() + cur.line);
                cur.line--;
                cur.column = prevLen;
            }
            m_cursor = cur;
            m_anchor = cur;
        }
        afterContentChanged();
    }
    void deleteForward() {
        if (!hasSelection() && m_cursor.line == lastLineIndex() && m_cursor.column == lineLen(m_cursor.line)) return;
        snapshotForUndo(EditKind::Deleting);
        if (hasSelection()) {
            deleteSelectionAndCollapse();
        } else {
            Pos cur = m_cursor;
            if (cur.column < lineLen(cur.line)) {
                m_lines[(size_t)cur.line].erase((size_t)cur.column, 1);
            } else if (cur.line < lastLineIndex()) {
                m_lines[(size_t)cur.line] += m_lines[(size_t)(cur.line + 1)];
                m_lines.erase(m_lines.begin() + cur.line + 1);
            }
        }
        afterContentChanged();
    }
    void cutSelection() {
        if (!hasSelection()) return;
        snapshotForUndo(EditKind::Other);
        deleteSelectionAndCollapse();
        afterContentChanged();
    }
    void indentSelection() {
        snapshotForUndo(EditKind::Other);
        Pos a, b;
        orderedSelection(a, b);
        const std::string indent((size_t)m_style.tabWidth, ' ');
        for (int32_t ln = a.line; ln <= b.line; ++ln) {
            m_lines[(size_t)ln].insert(0, indent);
        }
        m_cursor.column += m_style.tabWidth;
        m_anchor.column += m_style.tabWidth;
        afterContentChanged();
    }
    void dedentSelection() {
        snapshotForUndo(EditKind::Other);
        Pos a, b;
        orderedSelection(a, b);
        for (int32_t ln = a.line; ln <= b.line; ++ln) {
            std::string& line = m_lines[(size_t)ln];
            int32_t removed = 0;
            while (removed < m_style.tabWidth && !line.empty() && (line[0] == ' ' || line[0] == '\t')) {
                line.erase(0, 1);
                ++removed;
            }
        }
        m_cursor.column = (m_cursor.column > m_style.tabWidth) ? m_cursor.column - m_style.tabWidth : 0;
        m_anchor.column = (m_anchor.column > m_style.tabWidth) ? m_anchor.column - m_style.tabWidth : 0;
        afterContentChanged();
    }
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
Code::Code() : mp_impl(std::make_unique<Impl>()) {}
Code::~Code() = default;

bool Code::init(const Config& aConfig) {
    mp_impl->m_config = aConfig;
    return true;
}

void Code::unit() {
    mp_impl->m_lines.assign(1, std::string());
    mp_impl->m_maxLineChars = 0;
    mp_impl->m_cursor = {};
    mp_impl->m_anchor = {};
    mp_impl->m_desiredColumn = 0;
    mp_impl->m_undoStack.clear();
    mp_impl->m_redoStack.clear();
    mp_impl->relex();
}

// ---------------------------------------------------------------------------
// Content (no file IO inside Code)
// ---------------------------------------------------------------------------
void Code::setText(const char* aData, uint64_t aLen) {
    auto& lines = mp_impl->m_lines;
    lines.clear();
    lines.emplace_back();
    for (uint64_t idx = 0; idx < aLen; ++idx) {
        const char character = aData[idx];
        if (character == '\n') lines.emplace_back();
        else if (character != '\r') lines.back().push_back(character);
    }
    mp_impl->m_cursor = {};
    mp_impl->m_anchor = {};
    mp_impl->m_desiredColumn = 0;
    mp_impl->m_undoStack.clear();
    mp_impl->m_redoStack.clear();
    mp_impl->recomputeMaxLineChars();
    mp_impl->relex();
}

std::string Code::getText() const {
    std::string out;
    const auto& lines = mp_impl->m_lines;
    for (size_t idx = 0; idx < lines.size(); ++idx) {
        out += lines[idx];
        if (idx + 1 < lines.size()) out.push_back('\n');
    }
    return out;
}

void Code::enumerateSpans(const std::function<void(const char* aData, uint64_t aLen)>& aVisitor) const {
    static const char newline = '\n';
    const auto& lines = mp_impl->m_lines;
    for (size_t idx = 0; idx < lines.size(); ++idx) {
        aVisitor(lines[idx].data(), (uint64_t)lines[idx].size());
        if (idx + 1 < lines.size()) aVisitor(&newline, 1);
    }
}

void Code::setSource(DataSource* apSource) { mp_impl->m_dataSource = apSource; }

void Code::setLanguage(const char* aName) {
    mp_impl->m_languageName = (aName != nullptr) ? aName : "";
    mp_impl->relex();
}

// ---------------------------------------------------------------------------
// Frame entry — virtualized render + caret/selection/edit + coloring + zoom
// ---------------------------------------------------------------------------
bool Code::Render(const char* aId, const ImVec2& aSize) {
    Impl&        impl  = *mp_impl;
    const Style& style = impl.m_style;

    if (style.font != nullptr) ImGui::PushFont(style.font);

    ImVec2 size = aSize;
    if (size.x <= 0.0f) size.x = ImGui::GetContentRegionAvail().x;
    if (size.y <= 0.0f) size.y = ImGui::GetContentRegionAvail().y;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, style.colors[Col_Background]);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild(aId, size, 0, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    ImDrawList* drawList     = ImGui::GetWindowDrawList();
    ImFont*     font         = ImGui::GetFont();
    const float baseSize     = ImGui::GetFontSize();
    const float spacingRatio = ImGui::GetTextLineHeightWithSpacing() / baseSize;
    const float fontSize     = baseSize * impl.m_zoom;  // local editor zoom (ctrl + wheel)
    const float charWidth    = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, "X").x;
    const float fontHeight   = fontSize;
    const float lineH        = fontSize * spacingRatio;
    const float glyphY       = (lineH - fontHeight) * 0.5f;  // vertical centering within the row
    const int32_t lineCount  = (int32_t)impl.m_lines.size();

    int32_t digits = 1;
    for (int32_t n = lineCount; n >= 10; n /= 10) ++digits;
    const float pad     = charWidth * 0.5f;
    const float gutterW = (float)digits * charWidth + pad * 3.0f;

    const ImVec2 contentOrigin = ImGui::GetCursorScreenPos();
    const float  contentW = gutterW + (float)(impl.m_maxLineChars + 1) * charWidth;
    const float  contentH = (float)lineCount * lineH;
    ImGui::Dummy(ImVec2(contentW, contentH));

    const ImVec2 winPos  = ImGui::GetWindowPos();
    const ImVec2 winSize = ImGui::GetWindowSize();
    const float  scrollY = ImGui::GetScrollY();

    int32_t firstLine = (int32_t)(scrollY / lineH);
    if (firstLine < 0) firstLine = 0;
    const int32_t pageLines = (int32_t)(size.y / lineH);
    int32_t lastLine = firstLine + pageLines + 2;
    if (lastLine > lineCount) lastLine = lineCount;

    // --- input -------------------------------------------------------------
    const float textOriginX = contentOrigin.x + gutterW;
    auto posFromMouse = [&](ImVec2 aMouse) -> Pos {
        int32_t line = (int32_t)((aMouse.y - contentOrigin.y) / lineH);
        int32_t col  = (int32_t)((aMouse.x - textOriginX) / charWidth + 0.5f);
        return impl.clampPos(Pos{line, col});
    };

    const bool hovered = ImGui::IsWindowHovered();
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImGui::SetWindowFocus();
        const Pos p = posFromMouse(ImGui::GetMousePos());
        impl.place(p, ImGui::GetIO().KeyShift);
        impl.m_desiredColumn = p.column;
    }
    const bool focused = ImGui::IsWindowFocused();
    if (focused && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const Pos p = posFromMouse(ImGui::GetMousePos());
        impl.place(p, true);
        impl.m_desiredColumn = p.column;
    }

    // ctrl + wheel = local editor zoom; plain wheel = manual scroll (NoScrollWithMouse owns it)
    if (hovered) {
        const ImGuiIO& wio = ImGui::GetIO();
        if (wio.KeyCtrl && wio.MouseWheel != 0.0f) {
            impl.m_zoom *= (wio.MouseWheel > 0.0f) ? 1.1f : (1.0f / 1.1f);
            if (impl.m_zoom < 0.3f) impl.m_zoom = 0.3f;
            if (impl.m_zoom > 6.0f) impl.m_zoom = 6.0f;
        } else if (wio.MouseWheel != 0.0f || wio.MouseWheelH != 0.0f) {
            ImGui::SetScrollY(ImGui::GetScrollY() - wio.MouseWheel * lineH * 3.0f);
            ImGui::SetScrollX(ImGui::GetScrollX() - wio.MouseWheelH * charWidth * 6.0f);
        }
    }

    if (focused) {
        const ImGuiIO& io = ImGui::GetIO();
        const bool shift = io.KeyShift;
        const bool ctrl  = io.KeyCtrl;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))  impl.moveLeft(shift);
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) impl.moveRight(shift);
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))    impl.moveVertical(-1, shift);
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))  impl.moveVertical(+1, shift);
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp))     impl.moveVertical(-pageLines, shift);
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown))   impl.moveVertical(+pageLines, shift);
        if (ImGui::IsKeyPressed(ImGuiKey_Home))       ctrl ? impl.moveDocStart(shift) : impl.moveHome(shift);
        if (ImGui::IsKeyPressed(ImGuiKey_End))        ctrl ? impl.moveDocEnd(shift) : impl.moveEnd(shift);
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A))  impl.selectAll();

        // copy works even in read-only
        if (ctrl && impl.hasSelection() && ImGui::IsKeyPressed(ImGuiKey_C)) {
            ImGui::SetClipboardText(getSelectedText().c_str());
        }

        if ((impl.m_config.flags & Flags_ReadOnly) == 0) {
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z)) { shift ? impl.redo() : impl.undo(); }
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y)) impl.redo();
            if (ctrl && impl.hasSelection() && ImGui::IsKeyPressed(ImGuiKey_X)) {
                ImGui::SetClipboardText(getSelectedText().c_str());
                impl.cutSelection();
            }
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V)) {
                const char* clip = ImGui::GetClipboardText();
                if (clip != nullptr) impl.typeText(clip, Impl::EditKind::Other);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) impl.backspace();
            if (ImGui::IsKeyPressed(ImGuiKey_Delete))    impl.deleteForward();
            if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                impl.typeText("\n", Impl::EditKind::Other);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
                if (shift) {
                    impl.dedentSelection();
                } else if (impl.hasSelection() && impl.selectionMultiline()) {
                    impl.indentSelection();
                } else {
                    impl.typeText(std::string((size_t)impl.m_style.tabWidth, ' '), Impl::EditKind::Other);
                }
            }
            if (!ctrl) {
                std::string typed;
                for (int32_t c = 0; c < io.InputQueueCharacters.Size; ++c) {
                    const ImWchar ch = io.InputQueueCharacters[(int)c];
                    if (ch >= 32 && ch != 127 && ch < 128) typed.push_back((char)ch);  // printable ASCII (UTF-8 deferred)
                }
                if (!typed.empty()) impl.typeText(typed, Impl::EditKind::Typing);
            }
        }
    }

    // --- ensure cursor visible (vertical only, applies next frame) ----------
    if (impl.m_ensureCursorVisible) {
        const float cursorY = (float)impl.m_cursor.line * lineH;
        if (cursorY < scrollY) {
            ImGui::SetScrollY(cursorY);
        } else if (cursorY + lineH > scrollY + size.y) {
            ImGui::SetScrollY(cursorY + lineH - size.y);
        }
        impl.m_ensureCursorVisible = false;
    }

    // content may have changed via editing this frame -> re-clamp the draw range
    const int32_t drawLineCount = (int32_t)impl.m_lines.size();
    if (lastLine > drawLineCount) lastLine = drawLineCount;
    if (firstLine > drawLineCount - 1) firstLine = (drawLineCount > 0) ? drawLineCount - 1 : 0;

    // --- draw --------------------------------------------------------------
    const ImU32 colText    = style.colors[Col_Default];
    const ImU32 colNumber  = style.colors[Col_LineNumber];
    const ImU32 colGutter  = style.colors[Col_Gutter];
    const ImU32 colCurLine = style.colors[Col_CurrentLine];
    const ImU32 colSel     = style.colors[Col_Selection];
    const ImU32 colCaret   = style.colors[Col_Caret];

    // current line highlight
    if ((impl.m_config.flags & Flags_HighlightCurLine) && impl.m_cursor.line >= firstLine && impl.m_cursor.line < lastLine) {
        const float y = contentOrigin.y + (float)impl.m_cursor.line * lineH;
        drawList->AddRectFilled(ImVec2(winPos.x + gutterW, y), ImVec2(winPos.x + winSize.x, y + lineH), colCurLine);
    }

    // selection
    if (impl.hasSelection()) {
        Pos selStart, selEnd;
        impl.orderedSelection(selStart, selEnd);
        for (int32_t i = firstLine; i < lastLine; ++i) {
            if (i < selStart.line || i > selEnd.line) continue;
            const int32_t c0 = (i == selStart.line) ? selStart.column : 0;
            const int32_t c1 = (i == selEnd.line) ? selEnd.column : impl.lineLen(i);
            const float y  = contentOrigin.y + (float)i * lineH;
            const float x0 = textOriginX + (float)c0 * charWidth;
            const float x1 = textOriginX + (float)c1 * charWidth;
            drawList->AddRectFilled(ImVec2(x0, y), ImVec2(x1 + 1.0f, y + lineH), colSel);
        }
    }

    // text (colored token runs; fallback to plain default color)
    for (int32_t i = firstLine; i < lastLine; ++i) {
        const std::string& line = impl.m_lines[(size_t)i];
        if (line.empty()) continue;
        const float y = contentOrigin.y + (float)i * lineH + glyphY;
        const std::vector<LexToken>* toks = (i < (int32_t)impl.m_lineTokens.size()) ? &impl.m_lineTokens[(size_t)i] : nullptr;
        if (toks == nullptr || toks->empty()) {
            drawList->AddText(font, fontSize, ImVec2(textOriginX, y), colText, line.c_str(), line.c_str() + line.size());
        } else {
            for (const LexToken& tok : *toks) {
                const char* s  = line.c_str() + tok.start;
                const float tx = textOriginX + (float)tok.start * charWidth;
                drawList->AddText(font, fontSize, ImVec2(tx, y), style.colors[tok.color], s, s + tok.length);
            }
        }
    }

    // gutter (fixed horizontally) + line numbers
    drawList->AddRectFilled(ImVec2(winPos.x, winPos.y), ImVec2(winPos.x + gutterW, winPos.y + winSize.y), colGutter);
    char numberText[16];
    for (int32_t i = firstLine; i < lastLine; ++i) {
        const float y = contentOrigin.y + (float)i * lineH + glyphY;
        snprintf(numberText, sizeof(numberText), "%d", i + 1);
        const float textWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, numberText).x;
        drawList->AddText(font, fontSize, ImVec2(winPos.x + gutterW - pad - textWidth, y), colNumber, numberText);
    }

    // caret (blinking, only when focused)
    if (focused) {
        const double timeNow = ImGui::GetTime();
        const bool blinkOn = (timeNow - (double)(int64_t)timeNow) < 0.5;
        if (blinkOn) {
            const float cx = textOriginX + (float)impl.m_cursor.column * charWidth;
            const float cy = contentOrigin.y + (float)impl.m_cursor.line * lineH + glyphY;
            drawList->AddLine(ImVec2(cx, cy), ImVec2(cx, cy + fontHeight), colCaret, 1.0f);
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    if (style.font != nullptr) ImGui::PopFont();
    return false;
}

// ---------------------------------------------------------------------------
// Caret / selection (public)
// ---------------------------------------------------------------------------
Code::Pos Code::getCursor() const { return mp_impl->m_cursor; }
void Code::setCursor(const Pos& aPos) {
    mp_impl->place(aPos, false);
    mp_impl->m_desiredColumn = mp_impl->m_cursor.column;
}
Code::Range Code::getSelection() const {
    Range range;
    mp_impl->orderedSelection(range.start, range.end);
    return range;
}
void Code::setSelection(const Range& aRange) {
    mp_impl->m_anchor = mp_impl->clampPos(aRange.start);
    mp_impl->place(aRange.end, true);
}
std::string Code::getSelectedText() const {
    if (!mp_impl->hasSelection()) return std::string();
    Pos selStart, selEnd;
    mp_impl->orderedSelection(selStart, selEnd);
    std::string out;
    for (int32_t i = selStart.line; i <= selEnd.line; ++i) {
        const std::string& line = mp_impl->m_lines[(size_t)i];
        const int32_t c0 = (i == selStart.line) ? selStart.column : 0;
        const int32_t c1 = (i == selEnd.line) ? selEnd.column : (int32_t)line.size();
        out.append(line, (size_t)c0, (size_t)(c1 - c0));
        if (i != selEnd.line) out.push_back('\n');
    }
    return out;
}

// ---------------------------------------------------------------------------
// Commands — public entry mirroring the keymap (no clipboard for movements)
// ---------------------------------------------------------------------------
bool Code::execute(Command aCommand) {
    Impl& impl = *mp_impl;
    const bool readOnly = (impl.m_config.flags & Flags_ReadOnly) != 0;
    switch (aCommand) {
        case Command::MoveLeft:      impl.moveLeft(false); return true;
        case Command::MoveRight:     impl.moveRight(false); return true;
        case Command::MoveUp:        impl.moveVertical(-1, false); return true;
        case Command::MoveDown:      impl.moveVertical(+1, false); return true;
        case Command::MoveHomeSmart: impl.moveHome(false); return true;
        case Command::MoveLineEnd:   impl.moveEnd(false); return true;
        case Command::MoveDocStart:  impl.moveDocStart(false); return true;
        case Command::MoveDocEnd:    impl.moveDocEnd(false); return true;
        case Command::SelectAll:     impl.selectAll(); return true;
        case Command::Undo:          if (!readOnly) { impl.undo(); return true; } return false;
        case Command::Redo:          if (!readOnly) { impl.redo(); return true; } return false;
        case Command::Backspace:     if (!readOnly) { impl.backspace(); return true; } return false;
        case Command::DeleteForward: if (!readOnly) { impl.deleteForward(); return true; } return false;
        case Command::InsertNewline: if (!readOnly) { impl.typeText("\n", Impl::EditKind::Other); return true; } return false;
        case Command::IndentLines:   if (!readOnly) { impl.indentSelection(); return true; } return false;
        case Command::OutdentLines:  if (!readOnly) { impl.dedentSelection(); return true; } return false;
        case Command::Copy:          if (impl.hasSelection()) { ImGui::SetClipboardText(getSelectedText().c_str()); return true; } return false;
        case Command::Cut:           if (!readOnly && impl.hasSelection()) { ImGui::SetClipboardText(getSelectedText().c_str()); impl.cutSelection(); return true; } return false;
        case Command::Paste: {
            if (readOnly) return false;
            const char* clip = ImGui::GetClipboardText();
            if (clip != nullptr) { impl.typeText(clip, Impl::EditKind::Other); return true; }
            return false;
        }
        default: return false;  // remaining modern commands land in later steps
    }
}

// ---------------------------------------------------------------------------
// Markers / decorations / diagnostics — stored, rendered in later steps
// ---------------------------------------------------------------------------
void Code::setMarkers(const std::vector<Marker>&) {}
void Code::setDecorations(const std::vector<Decoration>&) {}
void Code::setDiagnostics(const std::vector<Diagnostic>&) {}
void Code::setGutterClickCallback(std::function<void(int32_t aLine)> aCallback) {
    mp_impl->m_gutterClick = std::move(aCallback);
}

// ---------------------------------------------------------------------------
// Providers (null -> builtin default behavior)
// ---------------------------------------------------------------------------
void Code::setExecutor(Executor* apExecutor) { mp_impl->m_executor = apExecutor; }
void Code::setMatcher(Matcher* apMatcher) { mp_impl->m_matcher = apMatcher; }
void Code::setCompletionProvider(CompletionProvider* apProvider) { mp_impl->m_completion = apProvider; }

// ---------------------------------------------------------------------------
// Style / config
// ---------------------------------------------------------------------------
Code::Style& Code::getStyle() { return mp_impl->m_style; }
Code::Config& Code::getConfig() { return mp_impl->m_config; }

}  // namespace im
