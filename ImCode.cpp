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

#include <cfloat>   // FLT_MAX
#include <cstdio>   // snprintf
#include <utility>  // std::move

namespace im {

// NOTE v1: caret columns are byte offsets into the line (ASCII assumption).
// UTF-8 codepoint stepping is a near-term refinement (design invariant).

// ---------------------------------------------------------------------------
// Internal state (Pimpl). v1 backing store is a simple vector<string> of lines.
// ---------------------------------------------------------------------------
struct Code::Impl {
    Config m_config{};
    Style  m_style{};

    std::vector<std::string> m_lines{std::string()};  // always at least one (empty) line
    std::string              m_languageName;
    size_t                   m_maxLineChars{};

    // caret / selection (single caret + range, no multi-cursor)
    Pos     m_cursor{};
    Pos     m_anchor{};          // selection anchor; selection is ordered(anchor, cursor)
    int32_t m_desiredColumn{};   // sticky column for vertical moves
    bool    m_ensureCursorVisible{};
    float   m_zoom{1.0f};        // local editor zoom (ctrl + mouse wheel), decoupled from global font

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
        m_style.colors[Col_Number]      = IM_COL32(181, 206, 168, 255);
        m_style.colors[Col_Comment]     = IM_COL32(106, 153, 85, 255);
        m_style.colors[Col_Preproc]     = IM_COL32(155, 155, 155, 255);
        m_style.colors[Col_Operator]    = IM_COL32(200, 200, 200, 255);
        for (int32_t idx = 0; idx < Col_COUNT; ++idx) {
            if (m_style.colors[idx] == 0) {
                m_style.colors[idx] = m_style.colors[Col_Default];
            }
        }
    }

    void recomputeMaxLineChars() {
        size_t maxc = 0;
        for (const auto& line : m_lines) {
            if (line.size() > maxc) {
                maxc = line.size();
            }
        }
        m_maxLineChars = maxc;
    }

    int32_t lastLineIndex() const { return (int32_t)m_lines.size() - 1; }
    int32_t lineLen(int32_t line) const {
        if (line < 0 || line > lastLineIndex()) {
            return 0;
        }
        return (int32_t)m_lines[(size_t)line].size();
    }

    Pos clampPos(Pos aPos) const {
        if (aPos.line < 0) aPos.line = 0;
        if (aPos.line > lastLineIndex()) aPos.line = lastLineIndex();
        const int32_t len = lineLen(aPos.line);
        if (aPos.column < 0) aPos.column = 0;
        if (aPos.column > len) aPos.column = len;
        return aPos;
    }
    static bool posLess(const Pos& a, const Pos& b) {
        return a.line < b.line || (a.line == b.line && a.column < b.column);
    }
    static bool posEqual(const Pos& a, const Pos& b) {
        return a.line == b.line && a.column == b.column;
    }
    bool hasSelection() const { return !posEqual(m_cursor, m_anchor); }
    void orderedSelection(Pos& aoStart, Pos& aoEnd) const {
        if (posLess(m_cursor, m_anchor)) { aoStart = m_cursor; aoEnd = m_anchor; }
        else { aoStart = m_anchor; aoEnd = m_cursor; }
    }

    void place(Pos aPos, bool aKeepSelection) {
        m_cursor = clampPos(aPos);
        if (!aKeepSelection) {
            m_anchor = m_cursor;
        }
        m_ensureCursorVisible = true;
    }

    void moveLeft(bool aShift) {
        Pos cur = m_cursor;
        if (cur.column > 0) {
            cur.column--;
        } else if (cur.line > 0) {
            cur.line--;
            cur.column = lineLen(cur.line);
        }
        place(cur, aShift);
        m_desiredColumn = m_cursor.column;
    }
    void moveRight(bool aShift) {
        Pos cur = m_cursor;
        if (cur.column < lineLen(cur.line)) {
            cur.column++;
        } else if (cur.line < lastLineIndex()) {
            cur.line++;
            cur.column = 0;
        }
        place(cur, aShift);
        m_desiredColumn = m_cursor.column;
    }
    void moveVertical(int32_t aDelta, bool aShift) {
        Pos cur = m_cursor;
        cur.line += aDelta;
        cur.column = m_desiredColumn;
        place(cur, aShift);  // clampPos handles line/column bounds
    }
    void moveHome(bool aShift) {
        const std::string& line = m_lines[(size_t)m_cursor.line];
        int32_t firstNonWs = 0;
        while (firstNonWs < (int32_t)line.size() && (line[(size_t)firstNonWs] == ' ' || line[(size_t)firstNonWs] == '\t')) {
            ++firstNonWs;
        }
        Pos cur = m_cursor;
        cur.column = (cur.column == firstNonWs) ? 0 : firstNonWs;  // smart Home toggle
        place(cur, aShift);
        m_desiredColumn = m_cursor.column;
    }
    void moveEnd(bool aShift) {
        Pos cur = m_cursor;
        cur.column = lineLen(cur.line);
        place(cur, aShift);
        m_desiredColumn = m_cursor.column;
    }
    void moveDocStart(bool aShift) {
        place(Pos{0, 0}, aShift);
        m_desiredColumn = 0;
    }
    void moveDocEnd(bool aShift) {
        place(Pos{lastLineIndex(), lineLen(lastLineIndex())}, aShift);
        m_desiredColumn = m_cursor.column;
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
            if (ch == '\n') {
                segs.emplace_back();
            } else if (ch != '\r') {
                segs.back().push_back(ch);
            }
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
    void typeText(const std::string& aText) {
        if (hasSelection()) {
            deleteSelectionAndCollapse();
        }
        const Pos end = insertTextAt(m_cursor, aText);
        m_cursor = end;
        m_anchor = end;
        m_desiredColumn = m_cursor.column;
        recomputeMaxLineChars();
        m_ensureCursorVisible = true;
    }
    void backspace() {
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
        m_desiredColumn = m_cursor.column;
        recomputeMaxLineChars();
        m_ensureCursorVisible = true;
    }
    void deleteForward() {
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
        m_desiredColumn = m_cursor.column;
        recomputeMaxLineChars();
        m_ensureCursorVisible = true;
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
        if (character == '\n') {
            lines.emplace_back();
        } else if (character != '\r') {
            lines.back().push_back(character);
        }
    }
    mp_impl->recomputeMaxLineChars();
    mp_impl->m_cursor = {};
    mp_impl->m_anchor = {};
    mp_impl->m_desiredColumn = 0;
}

std::string Code::getText() const {
    std::string out;
    const auto& lines = mp_impl->m_lines;
    for (size_t idx = 0; idx < lines.size(); ++idx) {
        out += lines[idx];
        if (idx + 1 < lines.size()) {
            out.push_back('\n');
        }
    }
    return out;
}

void Code::enumerateSpans(const std::function<void(const char* aData, uint64_t aLen)>& aVisitor) const {
    static const char newline = '\n';
    const auto& lines = mp_impl->m_lines;
    for (size_t idx = 0; idx < lines.size(); ++idx) {
        aVisitor(lines[idx].data(), (uint64_t)lines[idx].size());
        if (idx + 1 < lines.size()) {
            aVisitor(&newline, 1);
        }
    }
}

void Code::setSource(DataSource* apSource) {
    mp_impl->m_dataSource = apSource;
}

void Code::setLanguage(const char* aName) {
    mp_impl->m_languageName = (aName != nullptr) ? aName : "";
}

// ---------------------------------------------------------------------------
// Frame entry — step 3: virtualized render + caret/selection + keyboard/mouse
// ---------------------------------------------------------------------------
bool Code::Render(const char* aId, const ImVec2& aSize) {
    Impl&        impl  = *mp_impl;
    const Style& style = impl.m_style;

    if (style.font != nullptr) {
        ImGui::PushFont(style.font);
    }

    ImVec2 size = aSize;
    if (size.x <= 0.0f) size.x = ImGui::GetContentRegionAvail().x;
    if (size.y <= 0.0f) size.y = ImGui::GetContentRegionAvail().y;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, style.colors[Col_Background]);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild(aId, size, 0, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    ImDrawList* drawList    = ImGui::GetWindowDrawList();
    ImFont*     font         = ImGui::GetFont();
    const float baseSize     = ImGui::GetFontSize();
    const float spacingRatio = ImGui::GetTextLineHeightWithSpacing() / baseSize;
    const float fontSize     = baseSize * impl.m_zoom;  // local editor zoom (ctrl + wheel)
    const float charWidth    = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, "X").x;
    const float fontHeight   = fontSize;
    const float lineH        = fontSize * spacingRatio;
    const float glyphY       = (lineH - fontHeight) * 0.5f;  // vertical centering within the row
    const int32_t lineCount = (int32_t)impl.m_lines.size();

    int32_t digits = 1;
    for (int32_t n = lineCount; n >= 10; n /= 10) {
        ++digits;
    }
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
        impl.place(p, true);  // extend selection
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
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
            impl.m_anchor = Pos{0, 0};
            impl.place(Pos{impl.lastLineIndex(), impl.lineLen(impl.lastLineIndex())}, true);
        }

        // copy works even in read-only
        if (ctrl && impl.hasSelection() && ImGui::IsKeyPressed(ImGuiKey_C)) {
            ImGui::SetClipboardText(getSelectedText().c_str());
        }

        if ((impl.m_config.flags & Flags_ReadOnly) == 0) {
            if (ctrl && impl.hasSelection() && ImGui::IsKeyPressed(ImGuiKey_X)) {
                ImGui::SetClipboardText(getSelectedText().c_str());
                impl.deleteSelectionAndCollapse();
                impl.recomputeMaxLineChars();
                impl.m_ensureCursorVisible = true;
            }
            if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V)) {
                const char* clip = ImGui::GetClipboardText();
                if (clip != nullptr) {
                    impl.typeText(clip);
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) impl.backspace();
            if (ImGui::IsKeyPressed(ImGuiKey_Delete))    impl.deleteForward();
            if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                impl.typeText("\n");
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
                impl.typeText(std::string((size_t)impl.m_style.tabWidth, ' '));
            }
            if (!ctrl) {
                std::string typed;
                for (int32_t c = 0; c < io.InputQueueCharacters.Size; ++c) {
                    const ImWchar ch = io.InputQueueCharacters[(int)c];
                    if (ch >= 32 && ch != 127 && ch < 128) {  // printable ASCII (UTF-8 input deferred)
                        typed.push_back((char)ch);
                    }
                }
                if (!typed.empty()) {
                    impl.typeText(typed);
                }
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
            if (i < selStart.line || i > selEnd.line) {
                continue;
            }
            const int32_t c0 = (i == selStart.line) ? selStart.column : 0;
            const int32_t c1 = (i == selEnd.line) ? selEnd.column : impl.lineLen(i);
            const float y  = contentOrigin.y + (float)i * lineH;
            const float x0 = textOriginX + (float)c0 * charWidth;
            const float x1 = textOriginX + (float)c1 * charWidth;
            drawList->AddRectFilled(ImVec2(x0, y), ImVec2(x1 + 1.0f, y + lineH), colSel);
        }
    }

    // text
    for (int32_t i = firstLine; i < lastLine; ++i) {
        const std::string& line = impl.m_lines[(size_t)i];
        if (!line.empty()) {
            const float y = contentOrigin.y + (float)i * lineH + glyphY;
            drawList->AddText(font, fontSize, ImVec2(textOriginX, y), colText, line.c_str(), line.c_str() + line.size());
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

    if (style.font != nullptr) {
        ImGui::PopFont();
    }
    return false;
}

// ---------------------------------------------------------------------------
// Caret / selection (public)
// ---------------------------------------------------------------------------
Code::Pos Code::getCursor() const {
    return mp_impl->m_cursor;
}
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
    if (!mp_impl->hasSelection()) {
        return std::string();
    }
    Pos selStart, selEnd;
    mp_impl->orderedSelection(selStart, selEnd);
    std::string out;
    for (int32_t i = selStart.line; i <= selEnd.line; ++i) {
        const std::string& line = mp_impl->m_lines[(size_t)i];
        const int32_t c0 = (i == selStart.line) ? selStart.column : 0;
        const int32_t c1 = (i == selEnd.line) ? selEnd.column : (int32_t)line.size();
        out.append(line, (size_t)c0, (size_t)(c1 - c0));
        if (i != selEnd.line) {
            out.push_back('\n');
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Commands — stub until step 5
// ---------------------------------------------------------------------------
bool Code::execute(Command aCommand) {
    (void)aCommand;
    return false;
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
void Code::setExecutor(Executor* apExecutor) {
    mp_impl->m_executor = apExecutor;
}
void Code::setMatcher(Matcher* apMatcher) {
    mp_impl->m_matcher = apMatcher;
}
void Code::setCompletionProvider(CompletionProvider* apProvider) {
    mp_impl->m_completion = apProvider;
}

// ---------------------------------------------------------------------------
// Style / config
// ---------------------------------------------------------------------------
Code::Style& Code::getStyle() {
    return mp_impl->m_style;
}
Code::Config& Code::getConfig() {
    return mp_impl->m_config;
}

}  // namespace im
