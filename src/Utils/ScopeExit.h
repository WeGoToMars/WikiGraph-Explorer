#pragma once

#include <concepts>
#include <utility>

/// Calls `f` when the scope ends, also when an exception leaves it: `const ScopeExit done{[&] { ... }};`
template <std::invocable F>
class ScopeExit {
   public:
    explicit ScopeExit(F f) : f_(std::move(f)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit() { f_(); }

   private:
    F f_;
};
template <class F>
ScopeExit(F) -> ScopeExit<F>;
