// Copyright (C) 2020 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of the
// License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#ifndef FLARE_BASE_OBJECT_POOL_TYPES_H_
#define FLARE_BASE_OBJECT_POOL_TYPES_H_

#include <memory>
#include <utility>

#include "flare/base/internal/meta.h"
#include "flare/base/logging.h"
#include "flare/base/type_index.h"

namespace flare {

template <class T>
struct PoolTraits;

}

namespace flare::object_pool::detail {

struct TypeDescriptor {
  TypeIndex type;

  void* (*create)();
  void (*destroy)(void*);

  // Get / Put hook are called directly via `detail::OnXxxHook`. This eliminates
  // an indirect function call.
  //
  // So no `on_get` / `on_put` function pointer here.
};

// These hook helpers detect if the corresponding hook was set, and call the
// hook if so. It's a no-op otherwise.
#define FLARE_DEFINE_OBJECT_POOL_HOOK_IMPL(HookName)                       \
  template <class T>                                                       \
  inline void HookName##Hook([[maybe_unused]] void* p) {                   \
    if constexpr (FLARE_INTERNAL_IS_VALID(&x.HookName)(PoolTraits<T>{})) { \
      return PoolTraits<T>::HookName(reinterpret_cast<T*>(p));             \
    }                                                                      \
  }
FLARE_DEFINE_OBJECT_POOL_HOOK_IMPL(OnGet)
FLARE_DEFINE_OBJECT_POOL_HOOK_IMPL(OnPut)
#undef FLARE_DEFINE_OBJECT_POOL_HOOK_IMPL

// Call user defined factory if present, `new T()` otherwise.
template <class T>
void* CreateObject() {
  if constexpr (FLARE_INTERNAL_IS_VALID(&x.Create)(PoolTraits<T>{})) {
    return PoolTraits<T>::Create();
  } else {
    return new T();
  }
}

// Call user defined deleter if present, `delete static_cast<T*>(ptr)`
// otherwise.
template <class T>
void DestroyObject(void* ptr) {
  auto p = static_cast<T*>(ptr);
  if constexpr (FLARE_INTERNAL_IS_VALID(&x.Destroy)(PoolTraits<T>{})) {
    PoolTraits<T>::Destroy(p);
  } else {
    delete p;
  }
}

template <class T>
const TypeDescriptor* GetTypeDesc() {
  // `constexpr` doesn't work here.
  //
  // Possibly related: https://gcc.gnu.org/bugzilla/show_bug.cgi?id=81933
  static const TypeDescriptor desc = {.type = GetTypeIndex<T>(),
                                      .create = CreateObject<T>,
                                      .destroy = DestroyObject<T>};
  return &desc;
}

// Using `std::vector<ErasedPtr>` in thread-local cache hurts optimization. The
// compiler cannot optimize away call to destructor of `ErasedPtr`.
//
// Besides, even if we use `std::vector<NakedPtr>`, the compiler will have a
// hard time in eliminiting calls to `std::vector::realloc`.
//
// Objects here are either moved into internal cache (during which they're
// converted to `ErasedPtr`), or destroyed by this class's dtor.
class FixedVector {
 public:
  using NakedPtr = std::pair<void*, void (*)(void*)>;

  FixedVector() = default;
  explicit FixedVector(std::size_t size)
      : objects_(std::make_unique<NakedPtr[]>(size)),
        current_(objects_.get()),
        end_(objects_.get() + size) {}

  ~FixedVector() {
    while (!empty()) {
      auto&& [p, deleter] = *pop_back();
      deleter(p);
    }

    // We **hope** after destruction, calls to `full()` would return `true`.
    //
    // Frankly it's U.B.. Yet we  need this behavior when thread is leaving, so
    // as to deal with thread-local destruction order issues.
    *const_cast<NakedPtr* volatile*>(&current_) =
        *const_cast<NakedPtr* volatile*>(&end_) = nullptr;
  }

  bool empty() const noexcept { return current_ == objects_.get(); }
  std::size_t size() const noexcept { return current_ - objects_.get(); }
  bool full() const noexcept { return current_ == end_; }
  template <class... Ts>
  void emplace_back(Ts&&... args) noexcept {
    FLARE_DCHECK_LT(current_, end_);
    FLARE_DCHECK_GE(current_, objects_.get());
    *current_++ = {std::forward<Ts>(args)...};
  }
  NakedPtr* pop_back() noexcept {
    FLARE_DCHECK_LE(current_, end_);
    FLARE_DCHECK_GT(current_, objects_.get());
    return --current_;
  }

 private:
  std::unique_ptr<NakedPtr[]> objects_;
  NakedPtr* current_;
  NakedPtr* end_;
};

}  // namespace flare::object_pool::detail

#endif  // FLARE_BASE_OBJECT_POOL_TYPES_H_
