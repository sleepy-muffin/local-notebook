#include <utility>

namespace mmc {


template <typename F>
struct iter_args {
  template <class... Args>
  inline F& invoke() {
    return self();
  }

  template <typename T, typename... Args>
  inline F& invoke(T&& arg, Args&&... args) {
    self()(std::forward<T>(arg));
    return invoke(std::forward<Args>(args)...);
  }

  // template <typename... Args>
  // inline F& operator()(Args&&... args) {
  //   return self()(std::forward<Args>(args)...);
  // }
  //
  // template <typename T>
  // void operator()(const std::vector<T>& args) {
  //   for (const auto& arg : args) {
  //     self()(arg);
  //   }
  // }
private:
  inline F& self() {
    return *static_cast<F *>(this);
  }
};


}
