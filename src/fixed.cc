// SPDX-License-Identifier: AGPL-3.0-only
// cairn.fixed -- Room said in advance: a vector and a text that hold at most
// N, in themselves, and never ask for memory. Constexpr throughout.
export module cairn.fixed;

import std;

export namespace cairn {

// At most N values of T, held in place. T default-constructible: what a
// value past the size is.
template <class T, std::size_t N> class fixed_vector {
 public:
  using value_type = T;
  constexpr fixed_vector() = default;
  constexpr fixed_vector(std::initializer_list<T> values) {
    for (const T& one : values) push_back(one);
  }
  static constexpr std::size_t capacity() { return N; }
  constexpr std::size_t size() const { return size_; }
  constexpr bool empty() const { return size_ == 0; }
  constexpr bool full() const { return size_ == N; }
  // Whether it took the value: not where it is full.
  constexpr bool push_back(T value) {
    if (size_ == N) return false;
    values_[size_++] = std::move(value);
    return true;
  }
  constexpr void pop_back() { --size_; }
  constexpr void clear() { size_ = 0; }
  constexpr T& operator[](std::size_t i) { return values_[i]; }
  constexpr const T& operator[](std::size_t i) const { return values_[i]; }
  constexpr T& back() { return values_[size_ - 1]; }
  constexpr const T& back() const { return values_[size_ - 1]; }
  constexpr T& front() { return values_[0]; }
  constexpr const T& front() const { return values_[0]; }
  constexpr T* begin() { return values_.data(); }
  constexpr T* end() { return values_.data() + size_; }
  constexpr const T* begin() const { return values_.data(); }
  constexpr const T* end() const { return values_.data() + size_; }
  constexpr T* data() { return values_.data(); }
  constexpr const T* data() const { return values_.data(); }
  constexpr std::span<const T> span() const { return {values_.data(), size_}; }
  friend constexpr bool operator==(const fixed_vector& a, const fixed_vector& b) {
    return std::ranges::equal(a, b);
  }

 private:
  std::array<T, N> values_{};
  std::size_t size_ = 0;
};

// At most N characters, held in place.
template <std::size_t N> class fixed_text {
 public:
  constexpr fixed_text() = default;
  constexpr fixed_text(std::string_view s) { append(s); }
  static constexpr std::size_t capacity() { return N; }
  constexpr std::size_t size() const { return size_; }
  constexpr bool empty() const { return size_ == 0; }
  // Whether all of it fit: what did not is left out.
  constexpr bool append(std::string_view s) {
    const std::size_t room = N - size_;
    const std::size_t take = std::min(room, s.size());
    std::ranges::copy(s.substr(0, take), chars_.begin() + static_cast<std::ptrdiff_t>(size_));
    size_ += take;
    return take == s.size();
  }
  constexpr bool push_back(char c) {
    if (size_ == N) return false;
    chars_[size_++] = c;
    return true;
  }
  // A range of characters appended, as far as it fits.
  template <std::ranges::input_range R> constexpr bool append_range(R&& r) {
    for (char c : r)
      if (!push_back(c)) return false;
    return true;
  }
  constexpr void resize(std::size_t n) { size_ = std::min(n, N); }
  constexpr void clear() { size_ = 0; }
  constexpr std::string_view view() const { return {chars_.data(), size_}; }
  constexpr operator std::string_view() const { return view(); }
  constexpr const char* data() const { return chars_.data(); }
  constexpr const char* begin() const { return chars_.data(); }
  constexpr const char* end() const { return chars_.data() + size_; }
  friend constexpr bool operator==(const fixed_text& a, const fixed_text& b) { return a.view() == b.view(); }
  friend constexpr bool operator==(const fixed_text& a, std::string_view b) { return a.view() == b; }
  friend constexpr auto operator<=>(const fixed_text& a, const fixed_text& b) { return a.view() <=> b.view(); }

 private:
  std::array<char, N> chars_{};
  std::size_t size_ = 0;
};

}  // namespace cairn
