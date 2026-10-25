#pragma once
#include <cstddef>
#include <new>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace boost {

namespace container {
template<typename T, std::size_t N> using small_vector = std::vector<T>;

template<typename T, std::size_t N> class static_vector
{
    alignas(T) unsigned char m_storage[sizeof(T) * N];
    std::size_t m_size = 0;
    T* ptr() { return reinterpret_cast<T*>(m_storage); }
    const T* ptr() const { return reinterpret_cast<const T*>(m_storage); }
public:
    using value_type = T;
    using iterator = T*;
    using const_iterator = const T*;
    static_vector() = default;
    static_vector(const static_vector& o) { for (auto& v : o) push_back(v); }
    static_vector& operator=(const static_vector& o) { if (this != &o) { clear(); for (auto& v : o) push_back(v); } return *this; }
    ~static_vector() { clear(); }
    template<typename... A> T& emplace_back(A&&... a) { T* p = new (ptr() + m_size) T(std::forward<A>(a)...); ++m_size; return *p; }
    void push_back(const T& v) { emplace_back(v); }
    void push_back(T&& v) { emplace_back(std::move(v)); }
    void pop_back() { ptr()[--m_size].~T(); }
    void clear() { while (m_size) pop_back(); }
    void resize(std::size_t n) { while (m_size > n) pop_back(); while (m_size < n) emplace_back(); }
    std::size_t size() const { return m_size; }
    static constexpr std::size_t capacity() { return N; }
    bool empty() const { return m_size == 0; }
    bool full() const { return m_size == N; }
    T* data() { return ptr(); }
    const T* data() const { return ptr(); }
    T& operator[](std::size_t i) { return ptr()[i]; }
    const T& operator[](std::size_t i) const { return ptr()[i]; }
    T& back() { return ptr()[m_size - 1]; }
    const T& back() const { return ptr()[m_size - 1]; }
    T& front() { return ptr()[0]; }
    T* begin() { return ptr(); }
    T* end() { return ptr() + m_size; }
    const T* begin() const { return ptr(); }
    const T* end() const { return ptr() + m_size; }
    T* erase(T* it)
    {
        for (T* p = it; p + 1 < end(); ++p) *p = std::move(p[1]);
        pop_back();
        return it;
    }
};
}

namespace nowide {
inline std::wstring widen(const char* s, std::size_t n)
{
    int len = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, (int)n, w.data(), len);
    return w;
}
inline std::wstring widen(std::string_view s) { return widen(s.data(), s.size()); }
inline std::wstring widen(const char* s) { return widen(s, strlen(s)); }
inline wchar_t* widen(wchar_t* out, std::size_t outSize, const char* s)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, (int)outSize) ? out : nullptr;
}
inline std::string narrow(const wchar_t* s, std::size_t n)
{
    int len = WideCharToMultiByte(CP_UTF8, 0, s, (int)n, nullptr, 0, nullptr, nullptr);
    std::string r(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, (int)n, r.data(), len, nullptr, nullptr);
    return r;
}
inline std::string narrow(std::wstring_view s) { return narrow(s.data(), s.size()); }
inline std::string narrow(const wchar_t* s) { return narrow(s, wcslen(s)); }
}

inline bool iequals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    return true;
}

namespace random { using std::uniform_int_distribution; }

}
