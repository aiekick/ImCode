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

#pragma once

// ImCode, v0.1.0  -  requires C++14 minimum
//   (uses std::make_unique + aggregate init of NSDMI structs; no C++17 feature).
// Standalone Dear ImGui code editor. ImGui-only, no third-party deps.
// v1 backing store is a simple line vector; the piece-tree / threaded engine
// replaces it later behind this same public API.

#define IMCODE_VERSION     "0.1.0"
#define IMCODE_VERSION_NUM 00100

#ifndef IMCODE_API
#define IMCODE_API
#endif

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <memory>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#ifdef IMGUI_INCLUDE
#include IMGUI_INCLUDE
#else
#include <imgui.h>  // ImNodal defaults to imgui_internal.h; Code only needs the public API
#endif

namespace im {

class IMCODE_API Code {
public:
    // ---- palette: token classes + UI chrome (ImGui-style enum, nested) ----
    enum Col_ {
        Col_Default = 0, Col_Keyword, Col_Type, Col_Identifier, Col_Number,
        Col_String, Col_Char, Col_Comment, Col_Preproc, Col_Operator, Col_Punctuation,
        Col_Background, Col_CurrentLine, Col_Selection, Col_Caret,
        Col_LineNumber, Col_Gutter, Col_FoldMarker,
        Col_Whitespace, Col_IndentGuide, Col_Ruler, Col_Minimap,
        Col_MarkerError, Col_MarkerWarning, Col_BracketMatch, Col_SearchMatch,
        Col_COUNT
    };
    typedef int Col;

    // ---- behavior flags (bitwise) ----
    enum Flags_ {
        Flags_None             = 0,
        Flags_ReadOnly         = 1 << 0,
        Flags_ShowLineNumbers  = 1 << 1,
        Flags_ShowGutter       = 1 << 2,
        Flags_ShowFoldColumn   = 1 << 3,
        Flags_ShowMinimap      = 1 << 4,
        Flags_ShowWhitespace   = 1 << 5,
        Flags_ShowIndentGuides = 1 << 6,
        Flags_HighlightCurLine = 1 << 7,
        Flags_ShowRuler        = 1 << 8,
    };
    typedef int Flags;

    // ---- semantic commands (bound by the keymap, never hardcoded keys) ----
    enum class Command {
        // base
        MoveLeft, MoveRight, MoveUp, MoveDown, MoveWordLeft, MoveWordRight,
        MoveHomeSmart, MoveLineEnd, MovePageUp, MovePageDown, MoveDocStart, MoveDocEnd,
        SelectAll, Backspace, DeleteForward, DeleteWordLeft, DeleteWordRight,
        InsertNewline, InsertTab, Cut, Copy, Paste, Undo, Redo,
        // modern block / line ops
        IndentLines, OutdentLines, MoveLinesUp, MoveLinesDown,
        DuplicateUp, DuplicateDown, DeleteLine, JoinLines,
        InsertLineBelow, InsertLineAbove, ToggleLineComment, ToggleBlockComment,
        GotoMatchingBracket, FoldRegion, UnfoldRegion,
    };

    enum class Severity { Hint, Info, Warning, Error };
    enum class DecoKind { Underline, Squiggle, Box, Background };

    // ---- value types ({}-init, camelCase fields) ----
    struct Pos   { int32_t line{}; int32_t column{}; };  // column = display column (tab-expanded)
    struct Range { Pos start{}; Pos end{}; };
    struct Token { int32_t startColumn{}; int32_t length{}; Col color{}; };  // a colored run on one line

    struct Style {
        ImU32   colors[Col_COUNT]{};  // a 0 entry resolves to the builtin dark default
        ImFont* font{};               // host-provided monospace font (null -> current ImGui font)
        float   lineSpacing{1.0f};
        int32_t tabWidth{4};
    };

    struct Config {
        Flags    flags{Flags_ShowLineNumbers | Flags_ShowGutter | Flags_HighlightCurLine};
        int32_t  maxThreads{0};            // 0 = auto (min(4, cores-1)); 1 = inline (no worker)
        uint64_t maxResidentBytes{0};      // 0 = builtin budget
        int32_t  anchorEveryNLines{1024};  // sparse line-index / lexer-state granularity
        int32_t  maxUndoDepth{1000};
    };

    struct Marker {
        int32_t     line{};
        ImU32       iconColor{};
        std::string message;   // shown on gutter hover
        int32_t     userKind{}; // host-defined (breakpoint, bookmark, ...)
    };
    struct Decoration {
        Range       range{};
        DecoKind    kind{DecoKind::Squiggle};
        ImU32       color{};
        std::string message;   // optional hover
    };
    struct Diagnostic     { Range range{}; Severity severity{Severity::Error}; std::string message; };
    struct CompletionItem { std::string label; std::string insertText; int32_t kind{}; };

    // ---- host-pluggable providers (each ships a default implementation) ----
    struct DataSource {  // default: in-memory buffer
        virtual ~DataSource() = default;
        virtual uint64_t size() const = 0;
        virtual uint64_t read(uint64_t aOffset, uint64_t aLen, void* aoDst) const = 0;
        virtual bool     isThreadSafe() const { return false; }  // true -> workers read directly
    };
    struct Executor {  // default: bounded std::thread pool
        virtual ~Executor() = default;
        virtual void enqueue(std::function<void()> aTask) = 0;
    };
    struct SearchSlice { const char* data{}; uint64_t len{}; uint64_t from{}; };
    struct SearchMatch { uint64_t start{}; uint64_t len{}; };  // captures added for replace later
    struct Matcher {  // default: std::regex adapter
        virtual ~Matcher() = default;
        virtual bool compile(const char* aPattern, int32_t aFlags) = 0;
        virtual bool findNext(const SearchSlice& aSlice, SearchMatch& aoMatch) = 0;
    };
    struct CompletionProvider {  // default: buffer words + language keywords
        virtual ~CompletionProvider() = default;
        virtual void complete(const Pos& aAt, std::vector<CompletionItem>& aoItems) = 0;
    };

    // ---- lifecycle (host-owned, heavy class -> init/unit) ----
    Code();
    ~Code();
    Code(const Code&) = delete;
    Code& operator=(const Code&) = delete;
    bool init(const Config& aConfig = {});
    void unit();

    // ---- content (no file IO inside Code) ----
    void        setText(const char* aData, uint64_t aLen);  // buffer mode
    void        insertText(const char* aText);              // insert at caret (replaces selection)
    void        setSource(DataSource* apSource);            // pull mode (host owns the bytes)
    std::string getText() const;                            // small-file convenience
    void        enumerateSpans(const std::function<void(const char* aData, uint64_t aLen)>& aVisitor) const;  // save walk
    void        setLanguage(const char* aName);             // "cpp" / "glsl" / "sql" / "lua"
    void        getLineTokens(int32_t aLine, std::vector<Token>& aoTokens) const;  // colored tokens for one line

    // ---- frame entry ----
    bool Render(const char* aId, const ImVec2& aSize);  // draw + handle input; true if content changed

    // ---- caret / selection ----
    Pos         getCursor() const;
    void        setCursor(const Pos& aPos);
    Range       getSelection() const;
    void        setSelection(const Range& aRange);
    std::string getSelectedText() const;

    // ---- commands (the keymap calls these) ----
    bool execute(Command aCommand);

    // ---- markers / decorations / diagnostics -> render layer ----
    void setMarkers(const std::vector<Marker>& aMarkers);
    void setDecorations(const std::vector<Decoration>& aDecorations);
    void setDiagnostics(const std::vector<Diagnostic>& aDiagnostics);  // from grammar or LSP
    void setGutterClickCallback(std::function<void(int32_t aLine)> aCallback);

    // ---- providers (null -> shipped default) ----
    void setExecutor(Executor* apExecutor);
    void setMatcher(Matcher* apMatcher);
    void setCompletionProvider(CompletionProvider* apProvider);

    // ---- style / config (runtime-mutable) ----
    Style&  getStyle();
    Config& getConfig();

private:
    struct Impl;                  // internal state (Pimpl) — defined in ImCode.cpp
    std::unique_ptr<Impl> mp_impl;
};

}  // namespace im
