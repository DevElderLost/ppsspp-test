// Copyright (c) 2026- PPSSPP Project.
// A minimal std::span for C++17. Only what the mods code uses.

#pragma once

#include <array>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace Mods {

template <class T>
class Span {
public:
	using value_type = std::remove_const_t<T>;

	Span() = default;
	Span(T *data, size_t size) : data_(data), size_(size) {}
	template <size_t N>
	Span(T (&array)[N]) : data_(array), size_(N) {}
	Span(std::vector<value_type> &v) : data_(v.data()), size_(v.size()) {}
	template <class V = T, class = std::enable_if_t<std::is_const<V>::value>>
	Span(const std::vector<value_type> &v) : data_(v.data()), size_(v.size()) {}
	template <size_t N>
	Span(std::array<value_type, N> &a) : data_(a.data()), size_(N) {}
	template <class V = T, size_t N, class = std::enable_if_t<std::is_const<V>::value>>
	Span(const std::array<value_type, N> &a) : data_(a.data()), size_(N) {}
	// From a span of non-const to a span of const.
	template <class V = T, class = std::enable_if_t<std::is_const<V>::value>>
	Span(const Span<value_type> &other) : data_(other.data()), size_(other.size()) {}

	T *data() const { return data_; }
	size_t size() const { return size_; }
	bool empty() const { return size_ == 0; }
	T *begin() const { return data_; }
	T *end() const { return data_ + size_; }
	T &operator[](size_t i) const { return data_[i]; }

	Span subspan(size_t offset) const { return Span(data_ + offset, size_ - offset); }
	Span subspan(size_t offset, size_t count) const { return Span(data_ + offset, count); }
	Span first(size_t count) const { return Span(data_, count); }

private:
	T *data_ = nullptr;
	size_t size_ = 0;
};

}  // namespace Mods

namespace Mods {
// Deduction guides, so that Span(vector) and Span(pointer, size) work as std::span's do.
template <class T> Span(std::vector<T> &) -> Span<T>;
template <class T> Span(const std::vector<T> &) -> Span<const T>;
template <class T, size_t N> Span(std::array<T, N> &) -> Span<T>;
template <class T> Span(T *, size_t) -> Span<T>;
}  // namespace Mods
