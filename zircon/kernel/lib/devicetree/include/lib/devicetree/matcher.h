// Copyright 2022 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_LIB_DEVICETREE_INCLUDE_LIB_DEVICETREE_MATCHER_H_
#define ZIRCON_KERNEL_LIB_DEVICETREE_INCLUDE_LIB_DEVICETREE_MATCHER_H_

#include <lib/devicetree/devicetree.h>
#include <lib/devicetree/internal/matcher.h>
#include <lib/devicetree/matcher-result.h>
#include <lib/devicetree/path.h>
#include <lib/fit/result.h>

#include <algorithm>
#include <string_view>
#include <type_traits>

namespace devicetree {

// Scans |tree|, visiting each node at most |kMaxRescan| times. |kMaxRescan| is calculated
// from the MatcherResult<size_t N> and whether the matcher needs to resolve alias or not.
//
// A |matcher| is a callable with the following arguments (const NodePath& path, Properties
// properties):
//
// |path| represent the path to the current node of the in progress walk of the tree.
// |props| are the list of properties contained in the current node.
//
// The return type is used to communicate the |matcher|'s interest on a given node |m| or its
// subtree. See |MatchResult| for possible return values.
//
// When a |matcher| needs multiple(> 1) rescans it may choose to use |MatcherScanResult<N>| as its
// return type. This allows statically determining the maximum number of scan a given matcher may
// need until completion.
//
// It is equivalent returning |MatcherResult| and |MatcherResult<1>| when a matcher requires at
// most a single scan for completion.
//
// The maximum number of scans is determined from the the return type of the provided matchers. It
// is considered an error when not all matchers reach completion after all scans are performed. In
// this case |-1| is returned.
//
// Matcher examples:
//
// MatcherResult MatchFoo(const NodePath& path, Properties props) {
//   return {MatcherResult::kDone};
// }
//
// struct Matcher {
//   MatcherScanResult<45> operator()(const NodePath& path, Properties props) {
//     return {MatcherResult::kDone};
//   }
// };
//
// auto matcher = [&...] (const NodePath& path, Properties) -> MatcherScanResult<2>{
//   return {MatcherResult::kDone};
// };
//
// A matcher may provide an additional overload of the form:
//  * MatcherResult matcher()
//
// That is callable without arguments. This is used to notify a matcher when a scan finishes, this
// is useful for matchers that cannot determine whether they have finished or not until at least
// they are notified that all nodes have been visited (scan finished).
//
// A matcher is notified of every scan finish, as long as it has not reached completion.
//
// Example:
//
// struct Matcher() {
//    // Counts all the childs of cpus whose name is 'cpu'.
//    MatcherScanResult<2> operator()(const NodePath&, Properties, Resolver) {}
//
//    // The matcher can do any calculations now that the scan has finished, which means
//    // all cpu nodes have been visited. Or all interestings nodes in the devicetree have been
//    // visited at least once.
//    MatcherResult operator()() {}
// }
//
template <typename... Matchers>
fit::result<size_t, size_t> Match(Devicetree& tree, Matchers&&... matchers) {
  static_assert(sizeof...(Matchers) > 0);
  static_assert((internal::kIsValidMatcher<Matchers> && ...),
                "Matchers must implement the Matcher API.");
  // If no matchers need aliases, dont bother.
  if constexpr ((... && std::is_invocable_v<Matchers, const NodePath&, Properties>)) {
    return internal::Match(tree, matchers...);
  } else {
    // When providing aliases, the alias matcher MUST be last.
    internal::AliasMatcher alias_matcher;
    return internal::Match(tree, matchers..., alias_matcher);
  }
}

// Type |T| meets matcher API requirements.
template <typename T>
static constexpr bool kIsValidMatcher = internal::kIsValidMatcher<T>;

}  // namespace devicetree

#endif  // ZIRCON_KERNEL_LIB_DEVICETREE_INCLUDE_LIB_DEVICETREE_MATCHER_H_
