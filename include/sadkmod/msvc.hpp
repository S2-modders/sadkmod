// Mirrors of the MSVC 2005 (VC8) standard library containers the game uses, laid out byte for byte. GCC's own
// std::string / std::vector have different layouts and must never be passed to the game.
//
// Memory rule: a container the game owns is read, never resized or freed by us (its memory belongs to the game's
// heap). Containers we build for the game (string::borrow, string::small) must outlive the call and must only go
// to functions that read or copy them.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace sadk::msvc {

// std::basic_string<char> (28 bytes): +0 allocator word, +4 the text inline while capacity < 16, otherwise a
// pointer to it, +0x14 size, +0x18 capacity. (Ghidra /std/string; conf=known.)
struct string {
    std::uint32_t allocator_word;
    union {
        char inline_text[16];
        char *heap_text;
    };
    std::uint32_t size;
    std::uint32_t capacity;

    bool is_inline() const { return capacity < 16; }
    const char *data() const { return is_inline() ? inline_text : heap_text; }
    std::string_view view() const { return {data(), size}; }
    bool equals_icase(std::string_view s) const;

    // A string of up to 15 characters, stored inline (longer text is cut).
    static string small(std::string_view text);
    // A string that points at caller-owned text (any length, should be NUL-terminated). The game must not free it:
    // only pass it to functions that read or copy their argument.
    static string borrow(const char *text, std::size_t length);
    static string borrow(std::string_view text) { return borrow(text.data(), text.size()); }
};
static_assert(sizeof(string) == 28);
static_assert(offsetof(string, inline_text) == 4 && offsetof(string, size) == 0x14 && offsetof(string, capacity) == 0x18);

// std::vector<T> (16 bytes): +0 allocator word, +4 first, +8 last, +0xc end of storage. (Ghidra /ai/stl/MsvcVector.)
template <class T>
struct vector {
    std::uint32_t allocator_word;
    T *first;
    T *last;
    T *end_of_storage;

    T *begin() const { return first; }
    T *end() const { return last; }
    std::size_t size() const { return first ? static_cast<std::size_t>(last - first) : 0; }
    bool empty() const { return size() == 0; }
    T &operator[](std::size_t i) const { return first[i]; }
};
static_assert(sizeof(vector<int>) == 16);

// std::list<T> (12 bytes): +0 allocator word, +4 sentinel node, +8 size. Node: +0 next, +4 prev, +8 value.
// (Ghidra /ai/std/MsvcList.)
template <class T>
struct list {
    struct node {
        node *next;
        node *prev;
        T value;
    };
    struct iterator {
        node *n;
        T &operator*() const { return n->value; }
        T *operator->() const { return &n->value; }
        iterator &operator++() { n = n->next; return *this; }
        bool operator!=(const iterator &o) const { return n != o.n; }
    };
    std::uint32_t allocator_word;
    node *head;
    std::uint32_t count;

    iterator begin() const { return {head->next}; }
    iterator end() const { return {head}; }
    std::size_t size() const { return count; }
};
static_assert(sizeof(list<int>) == 12);

}  // namespace sadk::msvc
