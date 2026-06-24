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

// Per-line carry state between consecutive lines (small, trivially copyable).
struct LexState {
    bool    inBlockComment{};    // C-like and SQL  /* */
    int32_t luaLongLevel{-1};    // Lua long bracket level (-1 = none); [==[ -> 2
    bool    luaLongIsComment{};  // Lua long bracket is a comment (vs a string)
};

// Shared char-class helpers (used by every lexer).
inline bool isLetter(char aChar) { return (aChar >= 'a' && aChar <= 'z') || (aChar >= 'A' && aChar <= 'Z') || aChar == '_'; }
inline bool isDigit(char aChar)  { return aChar >= '0' && aChar <= '9'; }
inline bool isAlnum(char aChar)  { return isLetter(aChar) || isDigit(aChar); }
inline bool isOpChar(char aChar) { return aChar != 0 && std::strchr("+-*/%=<>!&|^~?:.#", aChar) != nullptr; }
inline bool isPunct(char aChar)  { return aChar != 0 && std::strchr("(){}[];,", aChar) != nullptr; }
inline bool inSet(const std::string& aWord, const std::unordered_set<std::string>& aSet) { return aSet.count(aWord) != 0; }

// Abstract lexer interface — one concrete implementation per language.
// File-local for v1; can be promoted to the public header so hosts register
// custom lexers, alongside the data-driven LanguageDef.
class Lexer {
public:
    virtual ~Lexer() = default;
    virtual void lexLine(const std::string& aLine, const LexState& aInState, std::vector<LexToken>& aoTokens, LexState& aoOutState) const = 0;
};

// Shared C-like lexing (comments, strings, numbers, operators). Subclasses
// only provide the keyword / type vocabulary via classifyWord().
class CLikeLexer : public Lexer {
public:
    void lexLine(const std::string& aLine, const LexState& aInState, std::vector<LexToken>& aoTokens, LexState& aoOutState) const override {
        aoTokens.clear();
        const int32_t n = (int32_t)aLine.size();
        int32_t i = 0;
        bool block = aInState.inBlockComment;
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
                aoTokens.push_back({start, i - start, classifyWord(aLine.substr((size_t)start, (size_t)(i - start)))});
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
        aoOutState.inBlockComment = block;
    }

protected:
    virtual int32_t classifyWord(const std::string& aWord) const = 0;
};

// C and C++.
class CppLexer final : public CLikeLexer {
protected:
    int32_t classifyWord(const std::string& aWord) const override {
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
        if (inSet(aWord, keywords)) return Code::Col_Keyword;
        if (inSet(aWord, types)) return Code::Col_Type;
        return Code::Col_Identifier;
    }
};

// GLSL (same C-like syntax; different keyword / type vocabulary).
class GlslLexer final : public CLikeLexer {
protected:
    int32_t classifyWord(const std::string& aWord) const override {
        static const std::unordered_set<std::string> keywords = {
            "attribute", "const", "uniform", "varying", "buffer", "shared", "coherent", "volatile", "restrict", "readonly",
            "writeonly", "layout", "centroid", "flat", "smooth", "noperspective", "patch", "sample", "in", "out", "inout",
            "invariant", "precise", "discard", "return", "if", "else", "switch", "case", "default", "subroutine", "while",
            "do", "for", "break", "continue", "true", "false", "precision", "highp", "mediump", "lowp", "struct"};
        static const std::unordered_set<std::string> types = {
            "void", "bool", "int", "uint", "float", "double", "vec2", "vec3", "vec4", "bvec2", "bvec3", "bvec4", "ivec2",
            "ivec3", "ivec4", "uvec2", "uvec3", "uvec4", "dvec2", "dvec3", "dvec4", "mat2", "mat3", "mat4", "mat2x2", "mat2x3",
            "mat2x4", "mat3x2", "mat3x3", "mat3x4", "mat4x2", "mat4x3", "mat4x4", "dmat2", "dmat3", "dmat4", "sampler1D",
            "sampler2D", "sampler3D", "samplerCube", "sampler2DArray", "samplerCubeArray", "sampler2DShadow", "samplerCubeShadow",
            "sampler2DRect", "isampler2D", "isampler3D", "usampler2D", "usampler3D", "sampler2DMS", "image2D", "iimage2D",
            "uimage2D", "atomic_uint"};
        if (inSet(aWord, keywords)) return Code::Col_Keyword;
        if (inSet(aWord, types)) return Code::Col_Type;
        return Code::Col_Identifier;
    }
};

// Lua: -- line comments, --[[ ]] / [[ ]] long brackets (multi-line, level-aware),
// "..." / '...' short strings. '#' is the length operator (handled via isOpChar).
class LuaLexer final : public Lexer {
public:
    void lexLine(const std::string& aLine, const LexState& aInState, std::vector<LexToken>& aoTokens, LexState& aoOutState) const override {
        aoTokens.clear();
        const int32_t n = (int32_t)aLine.size();
        int32_t i = 0;
        int32_t longLevel = aInState.luaLongLevel;
        bool longIsComment = aInState.luaLongIsComment;
        while (i < n) {
            if (longLevel >= 0) {  // inside a long bracket carried from a previous line
                const int32_t start = i;
                const bool closed = scanLongClose(aLine, i, longLevel);
                const int32_t color = longIsComment ? Code::Col_Comment : Code::Col_String;
                aoTokens.push_back({start, i - start, color});
                if (closed) longLevel = -1;
                continue;
            }
            const char c = aLine[(size_t)i];
            if (c == ' ' || c == '\t') { ++i; continue; }
            if (c == '-' && i + 1 < n && aLine[(size_t)(i + 1)] == '-') {
                const int32_t lvl = longOpenLevel(aLine, i + 2);
                if (lvl >= 0) {  // long comment --[=*[ ... ]=*]
                    const int32_t start = i;
                    i = i + 4 + lvl;  // skip "--" + "[" + lvl '=' + "["
                    longLevel = lvl;
                    longIsComment = true;
                    if (scanLongClose(aLine, i, longLevel)) longLevel = -1;
                    aoTokens.push_back({start, i - start, Code::Col_Comment});
                    continue;
                }
                aoTokens.push_back({i, n - i, Code::Col_Comment});  // plain line comment
                i = n;
                continue;
            }
            if (c == '[') {
                const int32_t lvl = longOpenLevel(aLine, i);
                if (lvl >= 0) {  // long string [=*[ ... ]=*]
                    const int32_t start = i;
                    i = i + 2 + lvl;  // skip "[" + lvl '=' + "["
                    longLevel = lvl;
                    longIsComment = false;
                    if (scanLongClose(aLine, i, longLevel)) longLevel = -1;
                    aoTokens.push_back({start, i - start, Code::Col_String});
                    continue;
                }
                aoTokens.push_back({i, 1, Code::Col_Punctuation});
                ++i;
                continue;
            }
            if (c == '"' || c == '\'') {
                const char quote = c;
                const int32_t start = i;
                ++i;
                while (i < n) {
                    if (aLine[(size_t)i] == '\\' && i + 1 < n) { i += 2; continue; }
                    if (aLine[(size_t)i] == quote) { ++i; break; }
                    ++i;
                }
                aoTokens.push_back({start, i - start, Code::Col_String});
                continue;
            }
            if (isDigit(c) || (c == '.' && i + 1 < n && isDigit(aLine[(size_t)(i + 1)]))) {
                const int32_t start = i;
                while (i < n) {
                    const char d = aLine[(size_t)i];
                    if (isAlnum(d) || d == '.') ++i;
                    else if ((d == '+' || d == '-') && i > start && (aLine[(size_t)(i - 1)] == 'e' || aLine[(size_t)(i - 1)] == 'E')) ++i;
                    else break;
                }
                aoTokens.push_back({start, i - start, Code::Col_Number});
                continue;
            }
            if (isLetter(c)) {
                const int32_t start = i;
                while (i < n && isAlnum(aLine[(size_t)i])) ++i;
                aoTokens.push_back({start, i - start, classifyWord(aLine.substr((size_t)start, (size_t)(i - start)))});
                continue;
            }
            if (isOpChar(c)) {
                const int32_t start = i;
                while (i < n && isOpChar(aLine[(size_t)i])) ++i;
                aoTokens.push_back({start, i - start, Code::Col_Operator});
                continue;
            }
            if (isPunct(c)) { aoTokens.push_back({i, 1, Code::Col_Punctuation}); ++i; continue; }
            ++i;
        }
        aoOutState.luaLongLevel = longLevel;
        aoOutState.luaLongIsComment = longIsComment;
    }

private:
    // at aPos, an opener [=*[ ? returns the '=' count (level) or -1.
    static int32_t longOpenLevel(const std::string& aLine, int32_t aPos) {
        const int32_t n = (int32_t)aLine.size();
        if (aPos >= n || aLine[(size_t)aPos] != '[') return -1;
        int32_t eq = 0;
        int32_t j = aPos + 1;
        while (j < n && aLine[(size_t)j] == '=') { ++eq; ++j; }
        if (j < n && aLine[(size_t)j] == '[') return eq;
        return -1;
    }
    // scan from aPos to a matching ]=*] of the given level; advance aPos; return true if closed on this line.
    static bool scanLongClose(const std::string& aLine, int32_t& aPos, int32_t aLevel) {
        const int32_t n = (int32_t)aLine.size();
        while (aPos < n) {
            if (aLine[(size_t)aPos] == ']') {
                int32_t eq = 0;
                int32_t j = aPos + 1;
                while (j < n && aLine[(size_t)j] == '=') { ++eq; ++j; }
                if (eq == aLevel && j < n && aLine[(size_t)j] == ']') { aPos = j + 1; return true; }
            }
            ++aPos;
        }
        return false;
    }
    static int32_t classifyWord(const std::string& aWord) {
        static const std::unordered_set<std::string> keywords = {
            "and", "break", "do", "else", "elseif", "end", "false", "for", "function", "goto", "if", "in",
            "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
        if (inSet(aWord, keywords)) return Code::Col_Keyword;
        return Code::Col_Identifier;
    }
};

// SQL: -- line comments, /* */ block comments, '...' strings ('' escaped),
// "..." quoted identifiers, case-INSENSITIVE keywords/types.
class SqlLexer final : public Lexer {
public:
    void lexLine(const std::string& aLine, const LexState& aInState, std::vector<LexToken>& aoTokens, LexState& aoOutState) const override {
        aoTokens.clear();
        const int32_t n = (int32_t)aLine.size();
        int32_t i = 0;
        bool block = aInState.inBlockComment;
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
            if (c == '-' && i + 1 < n && aLine[(size_t)(i + 1)] == '-') {
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
            if (c == '\'') {
                const int32_t start = i;
                ++i;
                while (i < n) {
                    if (aLine[(size_t)i] == '\'') {
                        if (i + 1 < n && aLine[(size_t)(i + 1)] == '\'') { i += 2; continue; }  // '' escape
                        ++i;
                        break;
                    }
                    ++i;
                }
                aoTokens.push_back({start, i - start, Code::Col_String});
                continue;
            }
            if (c == '"') {
                const int32_t start = i;
                ++i;
                while (i < n) {
                    if (aLine[(size_t)i] == '"') { ++i; break; }
                    ++i;
                }
                aoTokens.push_back({start, i - start, Code::Col_String});
                continue;
            }
            if (isDigit(c) || (c == '.' && i + 1 < n && isDigit(aLine[(size_t)(i + 1)]))) {
                const int32_t start = i;
                while (i < n) {
                    const char d = aLine[(size_t)i];
                    if (isAlnum(d) || d == '.') ++i;
                    else if ((d == '+' || d == '-') && i > start && (aLine[(size_t)(i - 1)] == 'e' || aLine[(size_t)(i - 1)] == 'E')) ++i;
                    else break;
                }
                aoTokens.push_back({start, i - start, Code::Col_Number});
                continue;
            }
            if (isLetter(c)) {
                const int32_t start = i;
                while (i < n && isAlnum(aLine[(size_t)i])) ++i;
                aoTokens.push_back({start, i - start, classifyWord(aLine.substr((size_t)start, (size_t)(i - start)))});
                continue;
            }
            if (isOpChar(c)) {
                const int32_t start = i;
                while (i < n && isOpChar(aLine[(size_t)i])) ++i;
                aoTokens.push_back({start, i - start, Code::Col_Operator});
                continue;
            }
            if (isPunct(c)) { aoTokens.push_back({i, 1, Code::Col_Punctuation}); ++i; continue; }
            ++i;
        }
        aoOutState.inBlockComment = block;
    }

private:
    static int32_t classifyWord(const std::string& aWord) {
        std::string upper = aWord;
        for (size_t k = 0; k < upper.size(); ++k) {
            if (upper[k] >= 'a' && upper[k] <= 'z') upper[k] = (char)(upper[k] - 'a' + 'A');
        }
        static const std::unordered_set<std::string> keywords = {
            "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "UPDATE", "SET", "DELETE", "CREATE", "TABLE", "DROP",
            "ALTER", "ADD", "COLUMN", "INDEX", "VIEW", "JOIN", "INNER", "LEFT", "RIGHT", "FULL", "OUTER", "ON", "AS",
            "AND", "OR", "NOT", "NULL", "IS", "IN", "LIKE", "BETWEEN", "GROUP", "BY", "ORDER", "HAVING", "DISTINCT",
            "LIMIT", "OFFSET", "UNION", "ALL", "EXISTS", "CASE", "WHEN", "THEN", "ELSE", "END", "PRIMARY", "KEY",
            "FOREIGN", "REFERENCES", "DEFAULT", "UNIQUE", "CONSTRAINT", "ASC", "DESC", "COUNT", "SUM", "AVG", "MIN", "MAX"};
        static const std::unordered_set<std::string> types = {
            "INT", "INTEGER", "SMALLINT", "BIGINT", "DECIMAL", "NUMERIC", "FLOAT", "REAL", "DOUBLE", "CHAR", "VARCHAR",
            "TEXT", "DATE", "TIME", "TIMESTAMP", "DATETIME", "BOOLEAN", "BOOL", "BLOB", "BINARY"};
        if (inSet(upper, keywords)) return Code::Col_Keyword;
        if (inSet(upper, types)) return Code::Col_Type;
        return Code::Col_Identifier;
    }
};

// Returns the lexer for a language name, or nullptr for plain (uncolored) text.
const Lexer* lexerForLanguage(const std::string& aName) {
    static const CppLexer cppLexer;
    static const GlslLexer glslLexer;
    static const LuaLexer luaLexer;
    static const SqlLexer sqlLexer;
    if (aName == "cpp" || aName == "c") return &cppLexer;
    if (aName == "glsl") return &glslLexer;
    if (aName == "lua") return &luaLexer;
    if (aName == "sql") return &sqlLexer;
    return nullptr;
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
    size_t                             m_maxDisplayColumns{};    // widest line in tab-expanded display columns
    std::vector<std::vector<LexToken>> m_lineTokens;            // cached per-line coloring (parallel to m_lines)

    // caret / selection (single caret + range, no multi-cursor)
    Pos     m_cursor{};
    Pos     m_anchor{};          // selection anchor; selection is ordered(anchor, cursor)
    int32_t m_desiredColumn{};   // sticky DISPLAY column (tab-expanded) for vertical moves
    bool    m_ensureCursorVisible{};
    float   m_zoom{1.0f};        // local editor zoom (ctrl + mouse wheel), decoupled from global font

    // undo / redo
    std::vector<UndoEntry> m_undoStack;
    std::vector<UndoEntry> m_redoStack;
    EditKind               m_lastEditKind{EditKind::Other};

    // find / replace (literal v1)
    std::string        m_searchPattern;
    FindFlags          m_searchFlags{};
    std::vector<Range> m_matches;
    int32_t            m_currentMatch{-1};
    bool               m_findBarOpen{};
    bool               m_findBarReplaceMode{};
    bool               m_findBarFocus{};
    char               m_findInput[256]{};
    char               m_replaceInput[256]{};

    // host-pluggable providers (null -> builtin default; wired in later steps)
    DataSource*         m_dataSource{};
    Executor*           m_executor{};
    Matcher*            m_matcher{};
    CompletionProvider* m_completion{};
    std::function<void(int32_t)> m_gutterClick;
    std::vector<Marker>     m_markers;
    std::vector<Decoration> m_decorations;
    std::vector<Diagnostic> m_diagnostics;

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
        m_style.colors[Col_SearchMatch]   = IM_COL32(130, 100, 40, 140);
        m_style.colors[Col_MarkerError]   = IM_COL32(220, 80, 80, 255);
        m_style.colors[Col_MarkerWarning] = IM_COL32(220, 180, 60, 255);
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

    // Map a byte column to its display column, expanding tabs to the next tab
    // stop (tabWidth). Display columns drive every on-screen X (monospace).
    int32_t displayColumn(int32_t aLine, int32_t aByteColumn) const {
        if (aLine < 0 || aLine > lastLineIndex()) return aByteColumn;
        const std::string& line = m_lines[(size_t)aLine];
        const int32_t tabW = (m_style.tabWidth > 0) ? m_style.tabWidth : 4;
        const int32_t end = (aByteColumn < (int32_t)line.size()) ? aByteColumn : (int32_t)line.size();
        int32_t disp = 0;
        for (int32_t k = 0; k < end; ++k) {
            if (line[(size_t)k] == '\t') disp += tabW - (disp % tabW);
            else ++disp;
        }
        if (aByteColumn > end) disp += aByteColumn - end;  // bytes past EOL count as 1 each
        return disp;
    }
    // Inverse of displayColumn: nearest byte column for a display-column target
    // (mouse hit-testing; a click past a glyph midpoint snaps to the next one).
    int32_t byteColumnFromDisplay(int32_t aLine, float aDisplayCols) const {
        if (aLine < 0 || aLine > lastLineIndex()) return 0;
        const std::string& line = m_lines[(size_t)aLine];
        const int32_t tabW = (m_style.tabWidth > 0) ? m_style.tabWidth : 4;
        int32_t disp = 0;
        for (int32_t k = 0; k < (int32_t)line.size(); ++k) {
            const int32_t adv = (line[(size_t)k] == '\t') ? (tabW - (disp % tabW)) : 1;
            if (aDisplayCols < (float)disp + (float)adv * 0.5f) return k;
            disp += adv;
        }
        return (int32_t)line.size();
    }
    void recomputeMaxDisplayColumns() {
        size_t maxc = 0;
        for (int32_t i = 0; i <= lastLineIndex(); ++i) {
            const size_t disp = (size_t)displayColumn(i, lineLen(i));
            if (disp > maxc) maxc = disp;
        }
        m_maxDisplayColumns = maxc;
    }
    void relex() {
        m_lineTokens.assign(m_lines.size(), std::vector<LexToken>());
        const Lexer* lexer = lexerForLanguage(m_languageName);
        if (lexer == nullptr) return;
        LexState state;
        for (size_t i = 0; i < m_lines.size(); ++i) {
            LexState outState;
            lexer->lexLine(m_lines[i], state, m_lineTokens[i], outState);
            state = outState;
        }
    }
    void afterContentChanged() {
        m_cursor = clampPos(m_cursor);
        m_anchor = clampPos(m_anchor);
        setDesiredColumnFromCursor();
        recomputeMaxDisplayColumns();
        relex();
        if (!m_searchPattern.empty()) computeMatches();
        m_ensureCursorVisible = true;
    }

    // --- caret moves -----------------------------------------------------
    void place(Pos aPos, bool aKeepSelection) {
        m_cursor = clampPos(aPos);
        if (!aKeepSelection) m_anchor = m_cursor;
        m_ensureCursorVisible = true;
        m_lastEditKind = EditKind::Other;  // a caret move breaks the typing/deleting coalesce group
    }
    // Desired column is a DISPLAY column (tab-expanded), so vertical moves keep
    // the caret visually aligned across lines with different tab/space layouts.
    void setDesiredColumnFromCursor() { m_desiredColumn = displayColumn(m_cursor.line, m_cursor.column); }
    void moveLeft(bool aShift) {
        Pos cur = m_cursor;
        if (cur.column > 0) cur.column--;
        else if (cur.line > 0) { cur.line--; cur.column = lineLen(cur.line); }
        place(cur, aShift);
        setDesiredColumnFromCursor();
    }
    void moveRight(bool aShift) {
        Pos cur = m_cursor;
        if (cur.column < lineLen(cur.line)) cur.column++;
        else if (cur.line < lastLineIndex()) { cur.line++; cur.column = 0; }
        place(cur, aShift);
        setDesiredColumnFromCursor();
    }
    void moveVertical(int32_t aDelta, bool aShift) {
        Pos cur = m_cursor;
        cur.line += aDelta;
        if (cur.line < 0) cur.line = 0;
        if (cur.line > lastLineIndex()) cur.line = lastLineIndex();
        cur.column = byteColumnFromDisplay(cur.line, (float)m_desiredColumn);  // land on the same visual column
        place(cur, aShift);
    }
    void moveHome(bool aShift) {
        const std::string& line = m_lines[(size_t)m_cursor.line];
        int32_t firstNonWs = 0;
        while (firstNonWs < (int32_t)line.size() && (line[(size_t)firstNonWs] == ' ' || line[(size_t)firstNonWs] == '\t')) ++firstNonWs;
        Pos cur = m_cursor;
        cur.column = (cur.column == firstNonWs) ? 0 : firstNonWs;
        place(cur, aShift);
        setDesiredColumnFromCursor();
    }
    void moveEnd(bool aShift) {
        Pos cur = m_cursor;
        cur.column = lineLen(cur.line);
        place(cur, aShift);
        setDesiredColumnFromCursor();
    }
    void moveDocStart(bool aShift) { place(Pos{0, 0}, aShift); setDesiredColumnFromCursor(); }
    void moveDocEnd(bool aShift) { place(Pos{lastLineIndex(), lineLen(lastLineIndex())}, aShift); setDesiredColumnFromCursor(); }
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

    // --- find / replace (literal, single-line matches) ---
    static void toLowerInPlace(std::string& aStr) {
        for (size_t k = 0; k < aStr.size(); ++k) {
            if (aStr[k] >= 'A' && aStr[k] <= 'Z') aStr[k] = (char)(aStr[k] - 'A' + 'a');
        }
    }
    static bool isWordChar(char aChar) {
        return (aChar >= 'a' && aChar <= 'z') || (aChar >= 'A' && aChar <= 'Z') || (aChar >= '0' && aChar <= '9') || aChar == '_';
    }
    void computeMatches() {
        m_matches.clear();
        m_currentMatch = -1;
        if (m_searchPattern.empty()) return;
        const bool ci = (m_searchFlags & FindFlags_CaseInsensitive) != 0;
        const bool ww = (m_searchFlags & FindFlags_WholeWord) != 0;
        std::string pat = m_searchPattern;
        if (ci) toLowerInPlace(pat);
        if (pat.empty()) return;
        for (int32_t ln = 0; ln <= lastLineIndex(); ++ln) {
            const std::string& original = m_lines[(size_t)ln];
            std::string hay = original;
            if (ci) toLowerInPlace(hay);
            size_t pos = 0;
            while (true) {
                const size_t found = hay.find(pat, pos);
                if (found == std::string::npos) break;
                const int32_t start = (int32_t)found;
                const int32_t end = start + (int32_t)pat.size();
                bool ok = true;
                if (ww) {
                    const bool left = (start == 0) || !isWordChar(original[(size_t)(start - 1)]);
                    const bool right = (end >= (int32_t)original.size()) || !isWordChar(original[(size_t)end]);
                    ok = left && right;
                }
                if (ok) m_matches.push_back(Range{Pos{ln, start}, Pos{ln, end}});
                pos = found + pat.size();
            }
        }
    }
    bool selectMatch(int32_t aIndex) {
        if (aIndex < 0 || aIndex >= (int32_t)m_matches.size()) return false;
        m_currentMatch = aIndex;
        const Range r = m_matches[(size_t)aIndex];
        m_anchor = clampPos(r.start);
        m_cursor = clampPos(r.end);
        setDesiredColumnFromCursor();
        m_ensureCursorVisible = true;
        m_lastEditKind = EditKind::Other;
        return true;
    }
    bool findRelative(bool aForward) {
        if (m_matches.empty()) return false;
        if (m_currentMatch >= 0) {
            int32_t next = aForward ? (m_currentMatch + 1) : (m_currentMatch - 1);
            if (next < 0) next = (int32_t)m_matches.size() - 1;
            if (next >= (int32_t)m_matches.size()) next = 0;
            return selectMatch(next);
        }
        const Pos ref = m_cursor;
        if (aForward) {
            for (size_t k = 0; k < m_matches.size(); ++k) {
                if (!posLess(m_matches[k].start, ref)) return selectMatch((int32_t)k);
            }
            return selectMatch(0);
        }
        for (int32_t k = (int32_t)m_matches.size() - 1; k >= 0; --k) {
            if (posLess(m_matches[(size_t)k].start, ref)) return selectMatch(k);
        }
        return selectMatch((int32_t)m_matches.size() - 1);
    }
    bool replaceCurrentMatch(const std::string& aReplacement) {
        if (m_currentMatch < 0 || m_currentMatch >= (int32_t)m_matches.size()) return false;
        const Range r = m_matches[(size_t)m_currentMatch];
        snapshotForUndo(EditKind::Other);
        deleteRange(r.start, r.end);
        m_cursor = r.start;
        m_anchor = r.start;
        const Pos end = insertTextAt(m_cursor, aReplacement);
        m_cursor = end;
        m_anchor = end;
        afterContentChanged();
        return true;
    }
    int32_t replaceAllMatches(const std::string& aReplacement) {
        if (m_matches.empty()) return 0;
        snapshotForUndo(EditKind::Other);
        const std::vector<Range> matches = m_matches;  // replace last-to-first to keep earlier positions valid
        for (int32_t k = (int32_t)matches.size() - 1; k >= 0; --k) {
            deleteRange(matches[(size_t)k].start, matches[(size_t)k].end);
            insertTextAt(matches[(size_t)k].start, aReplacement);
        }
        m_cursor = clampPos(m_cursor);
        m_anchor = m_cursor;
        afterContentChanged();
        return (int32_t)matches.size();
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
    mp_impl->m_maxDisplayColumns = 0;
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
    mp_impl->recomputeMaxDisplayColumns();
    mp_impl->relex();
}

void Code::insertText(const char* aText) {
    if (aText == nullptr) return;
    if ((mp_impl->m_config.flags & Flags_ReadOnly) != 0) return;
    mp_impl->typeText(aText, Impl::EditKind::Other);
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

void Code::getLineTokens(int32_t aLine, std::vector<Token>& aoTokens) const {
    aoTokens.clear();
    if (aLine < 0 || aLine >= (int32_t)mp_impl->m_lineTokens.size()) {
        return;
    }
    const std::vector<LexToken>& source = mp_impl->m_lineTokens[(size_t)aLine];
    aoTokens.reserve(source.size());
    for (const LexToken& token : source) {
        Token out;
        out.startColumn = token.start;
        out.length      = token.length;
        out.color       = token.color;
        aoTokens.push_back(out);
    }
}

// ---------------------------------------------------------------------------
// Frame entry — virtualized render + caret/selection/edit + coloring + zoom
// ---------------------------------------------------------------------------
bool Code::Render(const char* aId, const ImVec2& aSize) {
    Impl&        impl  = *mp_impl;
    const Style& style = impl.m_style;

    ImVec2 size = aSize;
    if (size.x <= 0.0f) size.x = ImGui::GetContentRegionAvail().x;
    if (size.y <= 0.0f) size.y = ImGui::GetContentRegionAvail().y;

    // find / replace bar (UI font, docked at the top of the editor area)
    float barHeight = 0.0f;
    if (impl.m_findBarOpen) {
        const float yBefore = ImGui::GetCursorPosY();
        ImGui::PushID("##imcode_find");
        if (impl.m_findBarFocus) { ImGui::SetKeyboardFocusHere(); impl.m_findBarFocus = false; }
        ImGui::SetNextItemWidth(220.0f);
        const bool enterFind  = ImGui::InputTextWithHint("##find", "Find", impl.m_findInput, (int)sizeof(impl.m_findInput), ImGuiInputTextFlags_EnterReturnsTrue);
        const bool editedFind = ImGui::IsItemEdited();
        ImGui::SameLine();
        if (ImGui::ArrowButton("##prev", ImGuiDir_Up)) findPrev();
        ImGui::SameLine();
        if (ImGui::ArrowButton("##next", ImGuiDir_Down)) findNext();
        ImGui::SameLine();
        ImGui::Text("%d/%d", (impl.m_currentMatch >= 0 ? impl.m_currentMatch + 1 : 0), (int)impl.m_matches.size());
        ImGui::SameLine();
        bool caseInsensitive = (impl.m_searchFlags & FindFlags_CaseInsensitive) != 0;
        if (ImGui::Checkbox("Aa", &caseInsensitive)) {
            impl.m_searchFlags = caseInsensitive ? (impl.m_searchFlags | FindFlags_CaseInsensitive) : (impl.m_searchFlags & ~FindFlags_CaseInsensitive);
            setSearch(impl.m_findInput, impl.m_searchFlags);
        }
        ImGui::SameLine();
        bool wholeWord = (impl.m_searchFlags & FindFlags_WholeWord) != 0;
        if (ImGui::Checkbox("W", &wholeWord)) {
            impl.m_searchFlags = wholeWord ? (impl.m_searchFlags | FindFlags_WholeWord) : (impl.m_searchFlags & ~FindFlags_WholeWord);
            setSearch(impl.m_findInput, impl.m_searchFlags);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) { clearSearch(); impl.m_findBarOpen = false; }
        if (editedFind) setSearch(impl.m_findInput, impl.m_searchFlags);
        if (enterFind) findNext();
        if (impl.m_findBarReplaceMode) {
            ImGui::SetNextItemWidth(220.0f);
            const bool enterReplace = ImGui::InputTextWithHint("##replace", "Replace", impl.m_replaceInput, (int)sizeof(impl.m_replaceInput), ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::SmallButton("Replace")) replaceCurrent(impl.m_replaceInput);
            ImGui::SameLine();
            if (ImGui::SmallButton("All")) replaceAll(impl.m_replaceInput);
            if (enterReplace) replaceCurrent(impl.m_replaceInput);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) { impl.m_findBarOpen = false; clearSearch(); }
        ImGui::PopID();
        barHeight = ImGui::GetCursorPosY() - yBefore;
    }

    float textHeight = size.y - barHeight;
    if (textHeight < 1.0f) textHeight = 1.0f;

    if (style.font != nullptr) ImGui::PushFont(style.font);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, style.colors[Col_Background]);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild(aId, ImVec2(size.x, textHeight), 0, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoScrollWithMouse);
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
    const float  contentW = gutterW + (float)(impl.m_maxDisplayColumns + 1) * charWidth;
    const float  contentH = (float)lineCount * lineH;
    ImGui::Dummy(ImVec2(contentW, contentH));

    const ImVec2 winPos  = ImGui::GetWindowPos();
    const ImVec2 winSize = ImGui::GetWindowSize();
    const float  scrollY = ImGui::GetScrollY();

    int32_t firstLine = (int32_t)(scrollY / lineH);
    if (firstLine < 0) firstLine = 0;
    const int32_t pageLines = (int32_t)(textHeight / lineH);
    int32_t lastLine = firstLine + pageLines + 2;
    if (lastLine > lineCount) lastLine = lineCount;

    // --- input -------------------------------------------------------------
    const float textOriginX = contentOrigin.x + gutterW;
    auto posFromMouse = [&](ImVec2 aMouse) -> Pos {
        int32_t line = (int32_t)((aMouse.y - contentOrigin.y) / lineH);
        if (line < 0) line = 0;
        if (line > impl.lastLineIndex()) line = impl.lastLineIndex();
        const int32_t col = impl.byteColumnFromDisplay(line, (aMouse.x - textOriginX) / charWidth);
        return impl.clampPos(Pos{line, col});
    };

    const bool hovered = ImGui::IsWindowHovered();
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImGui::SetWindowFocus();
        const ImVec2 mouse = ImGui::GetMousePos();
        if (mouse.x < winPos.x + gutterW) {  // gutter click -> host callback (e.g. breakpoint toggle)
            if (impl.m_gutterClick) {
                const int32_t ln = (int32_t)((mouse.y - contentOrigin.y) / lineH);
                if (ln >= 0 && ln <= impl.lastLineIndex()) impl.m_gutterClick(ln);
            }
        } else {
            const Pos p = posFromMouse(mouse);
            impl.place(p, ImGui::GetIO().KeyShift);
            impl.setDesiredColumnFromCursor();
        }
    }
    const bool focused = ImGui::IsWindowFocused();
    if (focused && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const Pos p = posFromMouse(ImGui::GetMousePos());
        impl.place(p, true);
        impl.setDesiredColumnFromCursor();
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

        // open the find / replace bar (find works in read-only; replace does not)
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_F)) {
            impl.m_findBarOpen = true;
            impl.m_findBarReplaceMode = false;
            impl.m_findBarFocus = true;
            const std::string sel = getSelectedText();
            if (!sel.empty() && sel.find('\n') == std::string::npos) {
                snprintf(impl.m_findInput, sizeof(impl.m_findInput), "%s", sel.c_str());
                setSearch(impl.m_findInput, impl.m_searchFlags);
            }
        }
        if (ctrl && (impl.m_config.flags & Flags_ReadOnly) == 0 && ImGui::IsKeyPressed(ImGuiKey_H)) {
            impl.m_findBarOpen = true;
            impl.m_findBarReplaceMode = true;
            impl.m_findBarFocus = true;
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
        } else if (cursorY + lineH > scrollY + textHeight) {
            ImGui::SetScrollY(cursorY + lineH - textHeight);
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
            const float x0 = textOriginX + (float)impl.displayColumn(i, c0) * charWidth;
            const float x1 = textOriginX + (float)impl.displayColumn(i, c1) * charWidth;
            drawList->AddRectFilled(ImVec2(x0, y), ImVec2(x1 + 1.0f, y + lineH), colSel);
        }
    }

    // search matches
    if (!impl.m_matches.empty()) {
        const ImU32 colMatch = style.colors[Col_SearchMatch];
        for (size_t m = 0; m < impl.m_matches.size(); ++m) {
            const Range& r = impl.m_matches[m];
            if (r.start.line < firstLine || r.start.line >= lastLine) continue;
            const float y  = contentOrigin.y + (float)r.start.line * lineH;
            const float x0 = textOriginX + (float)impl.displayColumn(r.start.line, r.start.column) * charWidth;
            const float x1 = textOriginX + (float)impl.displayColumn(r.end.line, r.end.column) * charWidth;
            drawList->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + lineH), colMatch);
        }
    }

    // text — paint colored token runs, filling any gap the lexer left between
    // them (e.g. unclassified bytes like '@', which produce no token) with the
    // default color. Guarantees every visible byte is drawn, with zero overdraw.
    for (int32_t i = firstLine; i < lastLine; ++i) {
        const std::string& line = impl.m_lines[(size_t)i];
        if (line.empty()) continue;
        const float y = contentOrigin.y + (float)i * lineH + glyphY;
        const int32_t lineSize = (int32_t)line.size();
        const int32_t tabW = (impl.m_style.tabWidth > 0) ? impl.m_style.tabWidth : 4;
        const std::vector<LexToken>* toks = (i < (int32_t)impl.m_lineTokens.size()) ? &impl.m_lineTokens[(size_t)i] : nullptr;
        int32_t disp = 0;  // running display column, advanced left to right across the whole line
        auto drawRun = [&](int32_t aStart, int32_t aEnd, ImU32 aColor) {
            int32_t k = aStart;
            while (k < aEnd) {
                if (line[(size_t)k] == '\t') {  // tab: invisible, jump to the next tab stop
                    disp += tabW - (disp % tabW);
                    ++k;
                } else {  // batch a maximal non-tab span into a single AddText
                    const int32_t spanStart = k;
                    const float tx = textOriginX + (float)disp * charWidth;
                    while (k < aEnd && line[(size_t)k] != '\t') { ++disp; ++k; }
                    drawList->AddText(font, fontSize, ImVec2(tx, y), aColor, line.c_str() + spanStart, line.c_str() + k);
                }
            }
        };
        int32_t col = 0;  // first byte not yet drawn (runs stay contiguous so disp flows correctly)
        if (toks != nullptr) {
            for (const LexToken& tok : *toks) {
                if (tok.start > col) drawRun(col, tok.start, colText);  // gap -> default color
                drawRun(tok.start, tok.start + tok.length, style.colors[tok.color]);
                col = tok.start + tok.length;
            }
        }
        if (col < lineSize) drawRun(col, lineSize, colText);  // trailing gap, or whole line if no tokens
    }

    // decorations + diagnostics (single-line in v1) — underline / squiggle / box + hover message
    const float decoThickness = (impl.m_zoom > 1.0f) ? impl.m_zoom : 1.0f;
    auto drawSquiggle = [&](float aX0, float aX1, float aY, ImU32 aColor) {
        const float amp    = 2.0f * impl.m_zoom;  // amplitude + period grow with the zoom
        const float period = 4.0f * impl.m_zoom;
        float x = aX0;
        bool up = true;
        while (x < aX1) {
            const float nx = (x + period < aX1) ? (x + period) : aX1;
            drawList->AddLine(ImVec2(x, aY + (up ? 0.0f : amp)), ImVec2(nx, aY + (up ? amp : 0.0f)), aColor, decoThickness);
            x = nx;
            up = !up;
        }
    };
    for (size_t d = 0; d < impl.m_decorations.size(); ++d) {
        const Decoration& dec = impl.m_decorations[d];
        const int32_t ln = dec.range.start.line;
        if (ln != dec.range.end.line || ln < firstLine || ln >= lastLine) continue;
        const float y    = contentOrigin.y + (float)ln * lineH;
        const float x0   = textOriginX + (float)impl.displayColumn(ln, dec.range.start.column) * charWidth;
        const float x1   = textOriginX + (float)impl.displayColumn(ln, dec.range.end.column) * charWidth;
        const float base = y + glyphY + fontHeight;
        if (dec.kind == DecoKind::Box) {
            drawList->AddRect(ImVec2(x0, y + glyphY), ImVec2(x1, y + glyphY + fontHeight), dec.color, 0.0f, 0, decoThickness);
        } else if (dec.kind == DecoKind::Squiggle) {
            drawSquiggle(x0, x1, base - 2.0f, dec.color);
        } else {  // Underline (Background renders as underline in v1)
            drawList->AddLine(ImVec2(x0, base), ImVec2(x1, base), dec.color, decoThickness);
        }

        if (!dec.message.empty() && ImGui::IsMouseHoveringRect(ImVec2(x0, y), ImVec2(x1, y + lineH))) {
            ImGui::SetTooltip("%s", dec.message.c_str());
        }
    }
    for (size_t g = 0; g < impl.m_diagnostics.size(); ++g) {
        const Diagnostic& diag = impl.m_diagnostics[g];
        const int32_t ln = diag.range.start.line;
        if (ln != diag.range.end.line || ln < firstLine || ln >= lastLine) continue;
        const ImU32 col  = (diag.severity == Severity::Error) ? style.colors[Col_MarkerError] : style.colors[Col_MarkerWarning];
        const float y    = contentOrigin.y + (float)ln * lineH;
        const float x0   = textOriginX + (float)impl.displayColumn(ln, diag.range.start.column) * charWidth;
        const float x1   = textOriginX + (float)impl.displayColumn(ln, diag.range.end.column) * charWidth;
        drawSquiggle(x0, x1, y + glyphY + fontHeight - 2.0f, col);
        if (!diag.message.empty() && ImGui::IsMouseHoveringRect(ImVec2(x0, y), ImVec2(x1, y + lineH))) {
            ImGui::SetTooltip("%s", diag.message.c_str());
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

    // gutter markers (host-registered; icon + hover message)
    for (size_t mk = 0; mk < impl.m_markers.size(); ++mk) {
        const Marker& mark = impl.m_markers[mk];
        if (mark.line < firstLine || mark.line >= lastLine) continue;
        const float y      = contentOrigin.y + (float)mark.line * lineH;
        const float cx     = winPos.x + charWidth * 0.6f;
        const float cy     = y + lineH * 0.5f;
        const float radius = charWidth * 0.35f;
        const ImU32 col    = (mark.iconColor != 0) ? mark.iconColor : style.colors[Col_MarkerError];
        drawList->AddCircleFilled(ImVec2(cx, cy), radius, col);
        if (!mark.message.empty() && ImGui::IsMouseHoveringRect(ImVec2(winPos.x, y), ImVec2(winPos.x + gutterW, y + lineH))) {
            ImGui::SetTooltip("%s", mark.message.c_str());
        }
    }

    // caret (blinking, only when focused)
    if (focused) {
        const double timeNow = ImGui::GetTime();
        const bool blinkOn = (timeNow - (double)(int64_t)timeNow) < 0.5;
        if (blinkOn) {
            const float cx = textOriginX + (float)impl.displayColumn(impl.m_cursor.line, impl.m_cursor.column) * charWidth;
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
    mp_impl->setDesiredColumnFromCursor();
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
// Find / replace (literal v1; the Matcher provider handles regex later)
// ---------------------------------------------------------------------------
void Code::setSearch(const char* aPattern, FindFlags aFlags) {
    mp_impl->m_searchPattern = (aPattern != nullptr) ? aPattern : "";
    mp_impl->m_searchFlags = aFlags;
    mp_impl->computeMatches();
}
void Code::clearSearch() {
    mp_impl->m_searchPattern.clear();
    mp_impl->m_matches.clear();
    mp_impl->m_currentMatch = -1;
}
int32_t Code::searchMatchCount() const {
    return (int32_t)mp_impl->m_matches.size();
}
bool Code::findNext() { return mp_impl->findRelative(true); }
bool Code::findPrev() { return mp_impl->findRelative(false); }
bool Code::replaceCurrent(const char* aReplacement) {
    if ((mp_impl->m_config.flags & Flags_ReadOnly) != 0) return false;
    return mp_impl->replaceCurrentMatch(aReplacement != nullptr ? aReplacement : "");
}
int32_t Code::replaceAll(const char* aReplacement) {
    if ((mp_impl->m_config.flags & Flags_ReadOnly) != 0) return 0;
    return mp_impl->replaceAllMatches(aReplacement != nullptr ? aReplacement : "");
}

// ---------------------------------------------------------------------------
// Markers / decorations / diagnostics — stored, rendered in later steps
// ---------------------------------------------------------------------------
void Code::setMarkers(const std::vector<Marker>& aMarkers) { mp_impl->m_markers = aMarkers; }
void Code::setDecorations(const std::vector<Decoration>& aDecorations) { mp_impl->m_decorations = aDecorations; }
void Code::setDiagnostics(const std::vector<Diagnostic>& aDiagnostics) { mp_impl->m_diagnostics = aDiagnostics; }
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
