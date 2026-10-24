#ifndef GOTESTUTILS_H
#define GOTESTUTILS_H

#include "GOTestException.h"
#include <cpptrace/cpptrace.hpp>
#include <sstream>
#include <string_view>
#include <utility>

namespace GOTestFormatDetail {

inline void AppendFormat(std::ostringstream &oss, std::string_view fmt) {
  oss << fmt;
}

template<typename T, typename... Rest>
void AppendFormat(
  std::ostringstream &oss,
  std::string_view fmt,
  T &&value,
  Rest &&...rest) {
  const size_t pos = fmt.find("{}");
  if (pos == std::string_view::npos) {
    oss << fmt;
    return;
  }

  oss << fmt.substr(0, pos) << std::forward<T>(value);
  AppendFormat(oss, fmt.substr(pos + 2), std::forward<Rest>(rest)...);
}

} // namespace GOTestFormatDetail

template<typename... Args>
std::string GOFormat(std::string_view fmt, Args &&...args) {
  std::ostringstream oss;
  GOTestFormatDetail::AppendFormat(
    oss, fmt, std::forward<Args>(args)...);
  return oss.str();
}

class GOTestUtils {
  /*
      We define here all methods that should be used in tests to check
      values and raise a GOTestException
  */

public:
  void GOAssert(bool expression, std::string message) {
    /**
     * This util function should be called as soon as we want to test an
     * expression in the test framework and launch the appropriate Exception
     * in GrandOrgue context with a good stack trace that shows us the
     * culprit line in order to find it easily.
     *
     */
    if (expression != true) {
      // The stack trace should be put here as all tests inherits
      // from this util class in order to display the original
      // faulty line in stack.
      std::cout << cpptrace::generate_trace();
      throw GOTestException(message);
    }
  }
};
#endif
