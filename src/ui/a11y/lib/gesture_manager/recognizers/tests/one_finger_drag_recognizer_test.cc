// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/a11y/lib/gesture_manager/recognizers/one_finger_drag_recognizer.h"

#include <fuchsia/ui/input/accessibility/cpp/fidl.h>

#include <memory>

#include <gtest/gtest.h>

#include "src/lib/testing/loop_fixture/test_loop_fixture.h"
#include "src/ui/a11y/lib/gesture_manager/arena/tests/mocks/mock_contest_member.h"
#include "src/ui/a11y/lib/gesture_manager/gesture_util/util.h"
#include "src/ui/a11y/lib/gesture_manager/recognizers/timing_constants.h"
#include "src/ui/a11y/lib/testing/input.h"

#include <glm/glm.hpp>

namespace accessibility_test {
namespace {

using AccessibilityPointerEvent = fuchsia::ui::input::accessibility::PointerEvent;
using Phase = fuchsia::ui::input::PointerEventPhase;

class OneFingerDragRecognizerTest : public gtest::TestLoopFixture {
 public:
  OneFingerDragRecognizerTest()
      : recognizer_(
            [this](a11y::GestureContext context) { gesture_start_callback_called_ = true; },
            [this](a11y::GestureContext context) { gesture_updates_.push_back(context); },
            [this](a11y::GestureContext context) { gesture_complete_callback_called_ = true; },
            a11y::kMinDragDuration) {}

  void SendPointerEvents(const std::vector<PointerParams>& events) {
    for (const auto& event : events) {
      SendPointerEvent(event);
    }
  }

  void SendPointerEvent(const PointerParams& event) {
    if (member_.is_held()) {
      recognizer_.HandleEvent(ToPointerEvent(event, 0));
    }
  }

 protected:
  MockContestMember member_;
  a11y::OneFingerDragRecognizer recognizer_;
  std::vector<a11y::GestureContext> gesture_updates_;
  bool gesture_start_callback_called_ = false;
  bool gesture_complete_callback_called_ = false;
};

// Tests successful drag detection case.
TEST_F(OneFingerDragRecognizerTest, WonAfterGestureDetected) {
  recognizer_.OnContestStarted(member_.TakeInterface());

  glm::vec2 first_update_ndc_position = {0, .7f};
  auto first_update_local_coordinates = ToLocalCoordinates(first_update_ndc_position);

  SendPointerEvents(DownEvents(1, {}) + MoveEvents(1, {}, first_update_ndc_position));

  EXPECT_EQ(member_.status(), a11y::ContestMember::Status::kUndecided);
  EXPECT_TRUE(gesture_updates_.empty());

  // Wait for the drag delay to elapse, at which point the recognizer should claim the win and
  // invoke the update callback.
  RunLoopFor(a11y::kMinDragDuration);

  ASSERT_EQ(member_.status(), a11y::ContestMember::Status::kAccepted);
  recognizer_.OnWin();

  EXPECT_TRUE(gesture_start_callback_called_);
  EXPECT_FALSE(gesture_complete_callback_called_);

  // We should see an update at location of the last event ingested prior to the delay elapsing.
  EXPECT_EQ(gesture_updates_.size(), 1u);
  {
    auto& location = gesture_updates_[0].current_pointer_locations[1].local_point;
    EXPECT_EQ(location.x, first_update_local_coordinates.x);
    EXPECT_EQ(location.y, first_update_local_coordinates.y);
  }

  SendPointerEvents(MoveEvents(1, {0, .7f}, {0, .85f}) + UpEvents(1, {0, .85f}));

  EXPECT_FALSE(member_.is_held());
  EXPECT_TRUE(gesture_complete_callback_called_);

  // Since MoveEvents() generates 10 evenly-spaced pointer events between the starting point (0, .7)
  // and ending point (0, .85), the recognizer will receive a series of MOVE events at (0, .715),
  // (0, .73), ..., (0, .85). The first for which the distance covered since the initial update,
  // which occurred at (0, .7), will be the event at (0, .775). We therefore expect an update to
  // occur at this point. We would expect an additional update when the distance between the pointer
  // and (0, .775) exceeds .0625, which will occur at (0, .85).
  auto second_update_local_coordinates = ToLocalCoordinates({0, .775f});
  auto third_update_local_coordinates = ToLocalCoordinates({0, .85f});

  EXPECT_EQ(gesture_updates_.size(), 3u);
  {
    auto& location = gesture_updates_[1].current_pointer_locations[1].local_point;
    EXPECT_EQ(location.x, second_update_local_coordinates.x);
    EXPECT_EQ(location.y, second_update_local_coordinates.y);
  }

  {
    auto& location = gesture_updates_[2].current_pointer_locations[1].local_point;
    EXPECT_EQ(location.x, third_update_local_coordinates.x);
    EXPECT_EQ(location.y, third_update_local_coordinates.y);
  }
}

// Verifies that recognizer rejects if multiple pointers are onscreen prior to
// accept.
TEST_F(OneFingerDragRecognizerTest, RejectMultifinger) {
  recognizer_.OnContestStarted(member_.TakeInterface());

  SendPointerEvents(DownEvents(1, {}) + MoveEvents(1, {}, {0, .7f}) + DownEvents(2, {}));

  ASSERT_EQ(member_.status(), a11y::ContestMember::Status::kRejected);
}

// Verifies that recognizer ignores additional pointers after accepting.
TEST_F(OneFingerDragRecognizerTest, SuppressMultitouchAfterAccept) {
  recognizer_.OnContestStarted(member_.TakeInterface());

  SendPointerEvents(DownEvents(1, {}) + MoveEvents(1, {}, {0, .7f}));

  // Wait for the drag delay to elapse, at which point the recognizer should claim the win and
  // invoke the update callback.
  RunLoopFor(a11y::kMinDragDuration);

  ASSERT_EQ(member_.status(), a11y::ContestMember::Status::kAccepted);
  recognizer_.OnWin();

  EXPECT_TRUE(gesture_start_callback_called_);

  SendPointerEvents(DownEvents(2, {}) + MoveEvents(1, {0, .7f}, {0, .85f}));

  EXPECT_EQ(gesture_updates_.size(), 1u);
}

// Tests that distance threshold between updates is enforced after first update.
TEST_F(OneFingerDragRecognizerTest, MinimumDistanceRequirementForUpdatesEnforced) {
  recognizer_.OnContestStarted(member_.TakeInterface());

  SendPointerEvents(DownEvents(1, {}) + MoveEvents(1, {}, {0, .7f}));

  // Wait for the drag delay to elapse, at which point the recognizer should claim the win and
  // invoke the update callback.
  RunLoopFor(a11y::kMinDragDuration);

  ASSERT_EQ(member_.status(), a11y::ContestMember::Status::kAccepted);
  recognizer_.OnWin();

  // Move pointer to location that does NOT meet the minimum threshold update.
  SendPointerEvents(MoveEvents(1, {0, .7f}, {0, .75f}) + UpEvents(1, {0, .75f}));

  EXPECT_FALSE(member_.is_held());
  EXPECT_TRUE(gesture_complete_callback_called_);

  // The update callback should only be invoked again if the pointer moves a sufficient distance
  // from the previous update. Since the pointer only moves .05f in this case, and the threshold
  // for an update is 1.f/16, no updates beyond the initial should have occurred.
  EXPECT_EQ(gesture_updates_.size(), 1u);
}

// Verifies that recognizer does not accept gesture before delay period elapses.
TEST_F(OneFingerDragRecognizerTest, DoNotAcceptPriorToDelayElapsing) {
  recognizer_.OnContestStarted(member_.TakeInterface());

  SendPointerEvents(DragEvents(1, {}, {0, .7f}));

  EXPECT_EQ(member_.status(), a11y::ContestMember::Status::kRejected);

  // Wait for the drag delay to elapse to ensure that task scheduled to claim win was cancelled.
  // The task calls Accept(), and then invokes the drag update callback. Therefore, if it was
  // cancelled successfully, we would not expect either method to have been called. The mock member_
  // has an assertion that if it was rejected, it may not have Accept() called on it.
  RunLoopFor(a11y::kMinDragDuration);

  EXPECT_TRUE(gesture_updates_.empty());
  EXPECT_FALSE(gesture_complete_callback_called_);
}

// Tests that recognizer abandons gesture if it is defeated.
TEST_F(OneFingerDragRecognizerTest, Defeat) {
  recognizer_.OnContestStarted(member_.TakeInterface());

  SendPointerEvents(DownEvents(1, {}) + MoveEvents(1, {}, {0, .7f}));

  // Wait for the drag delay to elapse, at which point the recognizer should attempt to claim the
  // win.
  RunLoopFor(a11y::kMinDragDuration);

  ASSERT_EQ(member_.status(), a11y::ContestMember::Status::kAccepted);
  // When it loses, the recognizer should NOT call the update task, and should instead abandon the
  // gesture.
  recognizer_.OnDefeat();

  EXPECT_FALSE(gesture_start_callback_called_);
  EXPECT_TRUE(gesture_updates_.empty());
  EXPECT_FALSE(gesture_complete_callback_called_);
}

}  // namespace
}  // namespace accessibility_test
