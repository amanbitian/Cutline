// Exact time, keyframe curves, and command validation.

#include "core/anim/Keyframe.h"
#include "core/commands/Command.h"
#include "core/time/RationalTime.h"
#include "effects/EffectRegistry.h"
#include "tests/native/TestHarness.h"

#include "core/model/RenderVersion.h"
#include <cmath>
#include <limits>
#include <set>
#include <string>

using cutline::anim::AnimatedValue;
using cutline::anim::Interpolation;
using cutline::anim::Keyframe;
using cutline::anim::Value;
using cutline::time::RationalTime;
namespace commands = cutline::commands;
namespace model = cutline::model;
namespace rates = cutline::time;

namespace {

RationalTime Seconds(std::int64_t count) { return {count, 1}; }

commands::CommandEnvelope Envelope(commands::CommandType type, commands::CommandPayload payload) {
  commands::CommandEnvelope command;
  command.command_id = "cmd-1";
  command.project_id = "project-1";
  command.author_id = "tester";
  command.base_revision = 0;
  command.timestamp_utc = "2026-10-05T00:00:00Z";
  command.type = type;
  command.payload = std::move(payload);
  command.idempotency_key = "key-1";
  return command;
}

}  // namespace

// ----------------------------------------------------------- rational time ----

CUTLINE_TEST(RationalTimeArithmeticStaysExact) {
  const RationalTime third{1, 3};
  const auto sum = third.Add(third).Add(third);
  CHECK_EQ(sum.numerator(), 1);
  CHECK_EQ(sum.denominator(), 1);
}

CUTLINE_TEST(RationalTimeConvertsFramesAtFractionalRates) {
  const auto time = RationalTime::FromFrames(48, rates::kFrameRate23976);
  CHECK_EQ(time.ToFrames(rates::kFrameRate23976, cutline::time::RoundingMode::Exact), 48);
}

CUTLINE_TEST(DropFrameTimecodeRoundTrips) {
  // One minute in: drop-frame skips frames ;00 and ;01 at 29.97.
  const auto parsed = RationalTime::ParseTimecode("00:01:00;02", rates::kFrameRate2997, true);
  CHECK_EQ(parsed.FormatTimecode(rates::kFrameRate2997, true), std::string("00:01:00;02"));
  // The tenth minute is not dropped, which is the rule a naive implementation
  // gets wrong.
  const auto tenth = RationalTime::ParseTimecode("00:10:00:00", rates::kFrameRate2997, false);
  CHECK_EQ(tenth.FormatTimecode(rates::kFrameRate2997, false), std::string("00:10:00:00"));
}

CUTLINE_TEST(DropFrameRejectsSkippedFrameNumbers) {
  CHECK_THROWS(RationalTime::ParseTimecode("00:01:00;00", rates::kFrameRate2997, true));
}

CUTLINE_TEST(TimecodeDisplaySnapsToTheNearestFrame) {
  // A timecode names a frame, so a display must be able to format a time that
  // falls between frames. At 29.97 a whole second is not a frame boundary.
  const auto one_second = Seconds(1);
  CHECK_EQ(one_second.FormatTimecode(rates::kFrameRate2997), std::string("00:00:01:00"));
  // Exact remains available for callers asserting frame alignment.
  CHECK_THROWS(one_second.FormatTimecode(rates::kFrameRate2997, false, cutline::time::RoundingMode::Exact));
  // A frame-aligned time formats identically under either mode.
  const auto aligned = RationalTime::FromFrames(30, rates::kFrameRate2997);
  CHECK_EQ(aligned.FormatTimecode(rates::kFrameRate2997, false, cutline::time::RoundingMode::Exact),
           aligned.FormatTimecode(rates::kFrameRate2997));
}

CUTLINE_TEST(NegativeTimesFormatWithASign) {
  const RationalTime before{-2, 1};
  CHECK_EQ(before.FormatTimecode(rates::kFrameRate25), std::string("-00:00:02:00"));
}

CUTLINE_TEST(TickTimebaseIsLosslessForBroadcastRates) {
  // The tick timebase exists so timeline ranges can be indexed in SQL. It is
  // only safe to index on if the conversion loses nothing, including at the
  // 1001-based rates.
  const cutline::time::FrameRate every[] = {rates::kFrameRate23976, rates::kFrameRate24, rates::kFrameRate25,
                                            rates::kFrameRate2997,  rates::kFrameRate30, rates::kFrameRate50,
                                            rates::kFrameRate5994,  rates::kFrameRate60};
  for (const auto& rate : every) {
    for (std::int64_t frame = 0; frame < 240; ++frame) {
      const auto time = RationalTime::FromFrames(frame, rate);
      const auto restored = RationalTime::FromTicks(time.ToTicks());
      CHECK_EQ(restored.Compare(time), 0);
    }
  }
}

CUTLINE_TEST(TickTimebaseIsLosslessForAudioSampleRates) {
  for (const std::int64_t sample_rate : {44100, 48000, 88200, 96000, 192000}) {
    for (std::int64_t sample = 0; sample < 100; ++sample) {
      const RationalTime time{sample, sample_rate};
      CHECK_EQ(RationalTime::FromTicks(time.ToTicks()).Compare(time), 0);
    }
  }
}

CUTLINE_TEST(RationalTimeRejectsZeroDenominator) {
  CHECK_THROWS(RationalTime(1, 0));
}

CUTLINE_TEST(RationalTimeDetectsOverflow) {
  const RationalTime huge{std::numeric_limits<std::int64_t>::max() / 2, 1};
  CHECK_THROWS(huge.Add(huge).Add(huge));
}

// ------------------------------------------------- long-timeline arithmetic ----
//
// The tick conversion used to multiply the numerator by 254016000000 before
// dividing, so a time whose fraction did not reduce overflowed 64 bits even
// though the resulting tick count fits easily. The earlier lossless-timebase
// tests only covered the first 240 frames, which never got near it.

CUTLINE_TEST(TickConversionSurvivesLongTimelinesAtEveryBroadcastRate) {
  const cutline::time::FrameRate every[] = {rates::kFrameRate23976, rates::kFrameRate24, rates::kFrameRate25,
                                            rates::kFrameRate2997,  rates::kFrameRate30, rates::kFrameRate50,
                                            rates::kFrameRate5994,  rates::kFrameRate60};
  for (const auto& rate : every) {
    const auto per_hour = (3600 * rate.numerator) / rate.denominator;
    // Adjacent frames either side of one, ten and twenty-four hours: these are
    // the unreduced fractions that overflowed.
    for (const std::int64_t hours : {1, 10, 24}) {
      for (std::int64_t offset = -3; offset <= 3; ++offset) {
        const auto frame = hours * per_hour + offset;
        const auto time = RationalTime::FromFrames(frame, rate);
        const auto ticks = time.ToTicks();
        // Lossless: converting back must give exactly the same instant.
        CHECK_EQ(RationalTime::FromTicks(ticks).Compare(time), 0);
        CHECK_EQ(time.ToFrames(rate, cutline::time::RoundingMode::Exact), frame);
      }
    }
  }
}

CUTLINE_TEST(TickOrderingFollowsTimeOrderingNearAnHour) {
  // Clip ranges are compared by tick value in SQL, so ticks must be monotonic
  // across frames or an overlap check would be wrong exactly where it matters.
  const auto rate = rates::kFrameRate2997;
  std::int64_t previous = -1;
  for (std::int64_t frame = 107990; frame < 108020; ++frame) {
    const auto ticks = RationalTime::FromFrames(frame, rate).ToTicks();
    CHECK(ticks > previous);
    previous = ticks;
  }
}

CUTLINE_TEST(RescaleAndFrameConversionSurviveLargeTimes) {
  // Ten hours as ticks, rescaled to a sample rate and to frames. The
  // intermediate product exceeds 64 bits; the result does not.
  // Ten hours plus one tick. A whole number of seconds would reduce to a tiny
  // fraction and never exercise the wide intermediate; the extra tick leaves
  // a numerator near 9e15 over a denominator of 2.5e11, which does not.
  const auto ten_hours = RationalTime::FromTicks(36000 * cutline::time::kTicksPerSecond + 1);
  const std::int64_t samples = 36000LL * 48000 + 1;  // one tick is far below one sample
  CHECK(ten_hours.Rescale(48000, cutline::time::RoundingMode::Floor) == 36000LL * 48000);
  CHECK(ten_hours.Rescale(48000, cutline::time::RoundingMode::Ceil) == samples);
  CHECK_EQ(ten_hours.ToFrames(rates::kFrameRate5994, cutline::time::RoundingMode::Floor),
           std::int64_t{36000} * 60000 / 1001);

  // A fractional frame time converts without overflow at 100 hours of samples.
  const RationalTime odd{360000LL * 1001 + 1, 30000};
  CHECK(odd.Rescale(96000, cutline::time::RoundingMode::Nearest) > 0);
}

CUTLINE_TEST(GenuineOverflowStillThrowsRatherThanWrapping) {
  // The wider intermediate must not turn a real overflow into a wrong answer.
  const RationalTime huge{std::numeric_limits<std::int64_t>::max() / 2, 1};
  CHECK_THROWS(huge.ToTicks());
  CHECK_THROWS(huge.Rescale(48000));
}

CUTLINE_TEST(RoundingModesAreUnchangedByTheWiderArithmetic) {
  // 7/2 s at 1 fps is 3.5 frames; the modes must agree with the old behaviour.
  const RationalTime time{7, 2};
  const cutline::time::FrameRate one{1, 1};
  CHECK_EQ(time.ToFrames(one, cutline::time::RoundingMode::Floor), std::int64_t{3});
  CHECK_EQ(time.ToFrames(one, cutline::time::RoundingMode::Ceil), std::int64_t{4});
  CHECK_EQ(time.ToFrames(one, cutline::time::RoundingMode::Nearest), std::int64_t{4});  // half away from zero
  CHECK_THROWS(time.ToFrames(one, cutline::time::RoundingMode::Exact));

  const RationalTime negative{-7, 2};
  CHECK_EQ(negative.ToFrames(one, cutline::time::RoundingMode::Floor), std::int64_t{-4});
  CHECK_EQ(negative.ToFrames(one, cutline::time::RoundingMode::Ceil), std::int64_t{-3});
  CHECK_EQ(negative.ToFrames(one, cutline::time::RoundingMode::Nearest), std::int64_t{-4});

  const RationalTime below_half{10, 3};  // 3.333...
  CHECK_EQ(below_half.ToFrames(one, cutline::time::RoundingMode::Nearest), std::int64_t{3});
  const RationalTime exact{9, 3};
  CHECK_EQ(exact.ToFrames(one, cutline::time::RoundingMode::Exact), std::int64_t{3});
}

// ---------------------------------------------------------------- keyframes ----

CUTLINE_TEST(AnimatedValueHoldsConstantWithoutKeyframes) {
  const AnimatedValue parameter(Value::Scalar(0.75));
  CHECK(!parameter.animated());
  CHECK(std::abs(parameter.Sample(Seconds(5)).scalar() - 0.75) < 1e-12);
}

CUTLINE_TEST(AnimatedValueInterpolatesLinearly) {
  AnimatedValue parameter;
  parameter.SetKeyframe({Seconds(0), Value::Scalar(0.0), Interpolation::Linear, {}, {}});
  parameter.SetKeyframe({Seconds(4), Value::Scalar(100.0), Interpolation::Linear, {}, {}});
  CHECK(std::abs(parameter.Sample(Seconds(1)).scalar() - 25.0) < 1e-9);
  CHECK(std::abs(parameter.Sample(Seconds(2)).scalar() - 50.0) < 1e-9);
  CHECK(std::abs(parameter.Sample({5, 2}).scalar() - 62.5) < 1e-9);
}

CUTLINE_TEST(AnimatedValueClampsOutsideTheKeyframeRange) {
  AnimatedValue parameter;
  parameter.SetKeyframe({Seconds(2), Value::Scalar(10.0), Interpolation::Linear, {}, {}});
  parameter.SetKeyframe({Seconds(4), Value::Scalar(20.0), Interpolation::Linear, {}, {}});
  CHECK(std::abs(parameter.Sample(Seconds(0)).scalar() - 10.0) < 1e-12);
  CHECK(std::abs(parameter.Sample(Seconds(9)).scalar() - 20.0) < 1e-12);
}

CUTLINE_TEST(HoldInterpolationDoesNotRamp) {
  AnimatedValue parameter;
  parameter.SetKeyframe({Seconds(0), Value::Scalar(0.0), Interpolation::Hold, {}, {}});
  parameter.SetKeyframe({Seconds(4), Value::Scalar(100.0), Interpolation::Hold, {}, {}});
  CHECK(std::abs(parameter.Sample(Seconds(3)).scalar()) < 1e-12);
  CHECK(std::abs(parameter.Sample(Seconds(4)).scalar() - 100.0) < 1e-12);
}

CUTLINE_TEST(EaseInOutIsMonotonicAndSymmetric) {
  AnimatedValue parameter;
  parameter.SetKeyframe({Seconds(0), Value::Scalar(0.0), Interpolation::EaseInOut, {}, {}});
  parameter.SetKeyframe({Seconds(10), Value::Scalar(1.0), Interpolation::EaseInOut, {}, {}});
  double previous = -1.0;
  for (std::int64_t tenth = 0; tenth <= 100; ++tenth) {
    const auto sampled = parameter.Sample({tenth, 10}).scalar();
    CHECK(sampled >= previous - 1e-9);
    previous = sampled;
  }
  // The midpoint of a symmetric ease sits at half the value delta.
  CHECK(std::abs(parameter.Sample(Seconds(5)).scalar() - 0.5) < 1e-3);
}

CUTLINE_TEST(BezierHandlesShapeTheCurve) {
  AnimatedValue parameter;
  Keyframe start{Seconds(0), Value::Scalar(0.0), Interpolation::Bezier, {0.9, 0.0}, {}};
  Keyframe end{Seconds(10), Value::Scalar(1.0), Interpolation::Bezier, {}, {0.1, 1.0}};
  parameter.SetKeyframe(start);
  parameter.SetKeyframe(end);
  // Handles pulled towards the ends make the middle of the curve steep, so the
  // midpoint still reads near 0.5 but the quarter points are pushed outward.
  CHECK(parameter.Sample({25, 10}).scalar() < 0.25);
  CHECK(parameter.Sample({75, 10}).scalar() > 0.75);
}

CUTLINE_TEST(SettingAKeyframeAtTheSameTimeReplacesIt) {
  AnimatedValue parameter;
  parameter.SetKeyframe({Seconds(1), Value::Scalar(5.0), Interpolation::Linear, {}, {}});
  parameter.SetKeyframe({Seconds(1), Value::Scalar(9.0), Interpolation::Linear, {}, {}});
  CHECK_EQ(parameter.keyframes().size(), std::size_t{1});
  CHECK(std::abs(parameter.Sample(Seconds(1)).scalar() - 9.0) < 1e-12);
}

CUTLINE_TEST(RemovingAKeyframeReportsWhetherItExisted) {
  AnimatedValue parameter;
  parameter.SetKeyframe({Seconds(1), Value::Scalar(5.0), Interpolation::Linear, {}, {}});
  CHECK(parameter.RemoveKeyframe(Seconds(1)));
  CHECK(!parameter.RemoveKeyframe(Seconds(1)));
  CHECK(!parameter.animated());
}

CUTLINE_TEST(KeyframesSortRegardlessOfInsertionOrder) {
  AnimatedValue parameter;
  parameter.SetKeyframe({Seconds(8), Value::Scalar(8.0), Interpolation::Linear, {}, {}});
  parameter.SetKeyframe({Seconds(2), Value::Scalar(2.0), Interpolation::Linear, {}, {}});
  parameter.SetKeyframe({Seconds(5), Value::Scalar(5.0), Interpolation::Linear, {}, {}});
  CHECK_EQ(parameter.keyframes().size(), std::size_t{3});
  CHECK_EQ(parameter.keyframes()[0].time.Compare(Seconds(2)), 0);
  CHECK_EQ(parameter.keyframes()[1].time.Compare(Seconds(5)), 0);
  CHECK_EQ(parameter.keyframes()[2].time.Compare(Seconds(8)), 0);
}

CUTLINE_TEST(MultiComponentValuesInterpolatePerComponent) {
  AnimatedValue position;
  position.SetKeyframe({Seconds(0), Value::Vec2(0.0, 100.0), Interpolation::Linear, {}, {}});
  position.SetKeyframe({Seconds(2), Value::Vec2(50.0, 0.0), Interpolation::Linear, {}, {}});
  const auto middle = position.Sample(Seconds(1));
  CHECK_EQ(middle.dimension, 2);
  CHECK(std::abs(middle.components[0] - 25.0) < 1e-9);
  CHECK(std::abs(middle.components[1] - 50.0) < 1e-9);
}

CUTLINE_TEST(InterpolationNamesRoundTrip) {
  for (const auto mode : {Interpolation::Hold, Interpolation::Linear, Interpolation::Bezier, Interpolation::EaseIn,
                          Interpolation::EaseOut, Interpolation::EaseInOut}) {
    CHECK(cutline::anim::ParseInterpolation(cutline::anim::ToString(mode)) == mode);
  }
}

CUTLINE_TEST(SplitKeyframesReproducesTheOriginalCurveOnBothSides) {
  // A cut must not change the animation: sampled in its own clip-local time, each
  // half equals the original over the part it covers. Checked for every
  // interpolation, a multi-component value, and every kind of cut position --
  // before the first key, on a key, inside each segment, on the last key, after it.
  // The Bezier handles overshoot on purpose, so a wrong split cannot hide.
  const Interpolation modes[] = {Interpolation::Hold,     Interpolation::Linear,    Interpolation::EaseIn,
                                 Interpolation::EaseOut,  Interpolation::EaseInOut, Interpolation::Bezier};
  const std::int64_t cuts_in_tenths[] = {5, 20, 33, 60, 77, 90, 125};

  for (const auto mode : modes) {
    const auto key = [&](std::int64_t tenths, double x, double y) {
      return Keyframe{{tenths, 10}, Value::Vec2(x, y), mode, {0.15, 0.8}, {0.7, 1.3}};
    };
    const std::vector<Keyframe> keys{key(20, 0.0, 10.0), key(60, 1.0, -4.0), key(90, 0.25, 6.0)};
    const AnimatedValue original(keys);

    for (const auto cut_tenths : cuts_in_tenths) {
      const RationalTime at{cut_tenths, 10};
      const auto split = cutline::anim::SplitKeyframes(keys, at);
      const AnimatedValue left(split.left);
      const AnimatedValue right(split.right);

      for (std::int64_t step = 0; step <= 1500; ++step) {
        const RationalTime time{step, 100};
        const auto want = original.Sample(time);
        const auto got = time.Compare(at) <= 0 ? left.Sample(time) : right.Sample(time.Subtract(at));
        for (int component = 0; component < 2; ++component) {
          const auto scale = std::max(1.0, std::abs(want.components[static_cast<std::size_t>(component)]));
          if (std::abs(got.components[static_cast<std::size_t>(component)] -
                       want.components[static_cast<std::size_t>(component)]) > 1e-5 * scale) {
            cutline::testing::Fail("split curve disagrees with the original", __FILE__, __LINE__,
                                   std::string("mode ") + cutline::anim::ToString(mode) + ", cut " +
                                       std::to_string(cut_tenths) + "/10, time " + std::to_string(step) + "/100, " +
                                       "component " + std::to_string(component) + ": wanted " +
                                       std::to_string(want.components[static_cast<std::size_t>(component)]) +
                                       " got " +
                                       std::to_string(got.components[static_cast<std::size_t>(component)]));
          }
        }
      }
    }
  }
}

CUTLINE_TEST(SplitKeyframesPlacesBoundaryKeysAndRebasesTheRightHalf) {
  const std::vector<Keyframe> keys{{{2, 1}, Value::Scalar(0.0), Interpolation::Linear, {}, {}},
                                   {{6, 1}, Value::Scalar(8.0), Interpolation::Linear, {}, {}}};

  // Inside the segment: the left gains a key at the cut, the right gains one at zero.
  const auto inside = cutline::anim::SplitKeyframes(keys, {4, 1});
  CHECK_EQ(inside.left.size(), std::size_t{2});
  CHECK_EQ(inside.left.back().time.Compare({4, 1}), 0);
  CHECK(std::abs(inside.left.back().value.scalar() - 4.0) < 1e-12);
  CHECK_EQ(inside.right.size(), std::size_t{2});
  CHECK_EQ(inside.right.front().time.Compare({0, 1}), 0);
  CHECK(std::abs(inside.right.front().value.scalar() - 4.0) < 1e-12);
  CHECK_EQ(inside.right.back().time.Compare({2, 1}), 0);  // 6 rebased by 4

  // Exactly on a key: both halves keep it and nothing is invented.
  const auto on_key = cutline::anim::SplitKeyframes(keys, {6, 1});
  CHECK_EQ(on_key.left.size(), std::size_t{2});
  CHECK_EQ(on_key.right.size(), std::size_t{1});
  CHECK_EQ(on_key.right.front().time.Compare({0, 1}), 0);
  CHECK(std::abs(on_key.right.front().value.scalar() - 8.0) < 1e-12);

  // Before the first key: the left holds the first value, the right is all of it.
  const auto before = cutline::anim::SplitKeyframes(keys, {1, 1});
  CHECK_EQ(before.left.size(), std::size_t{1});
  CHECK_EQ(before.right.size(), std::size_t{2});
  CHECK_EQ(before.right.front().time.Compare({1, 1}), 0);

  // After the last key: the right holds the last value from zero.
  const auto past = cutline::anim::SplitKeyframes(keys, {9, 1});
  CHECK_EQ(past.left.size(), std::size_t{2});
  CHECK_EQ(past.right.size(), std::size_t{1});
  CHECK(std::abs(past.right.front().value.scalar() - 8.0) < 1e-12);
}

CUTLINE_TEST(SplitKeyframesHandlesNothingAndRejectsDisorder) {
  const auto empty = cutline::anim::SplitKeyframes({}, {1, 1});
  CHECK(empty.left.empty() && empty.right.empty());

  const std::vector<Keyframe> disordered{{{5, 1}, Value::Scalar(1.0), Interpolation::Linear, {}, {}},
                                         {{2, 1}, Value::Scalar(0.0), Interpolation::Linear, {}, {}}};
  CHECK_THROWS(cutline::anim::SplitKeyframes(disordered, {3, 1}));
}

CUTLINE_TEST(SplitKeyframesTurnsEasedSegmentsIntoExplicitBezierHandles) {
  const std::vector<Keyframe> keys{{{0, 1}, Value::Scalar(0.0), Interpolation::EaseInOut, {}, {}},
                                   {{10, 1}, Value::Scalar(1.0), Interpolation::EaseInOut, {}, {}}};
  const auto split = cutline::anim::SplitKeyframes(keys, {4, 1});
  // Both halves of the eased segment are now curves with their own handles; a
  // plain EaseInOut over the shorter spans would ease over the wrong distance.
  CHECK(split.left.front().interpolation == Interpolation::Bezier);
  CHECK(split.right.front().interpolation == Interpolation::Bezier);
  // Handles stay inside the unit time range.
  for (const auto* key : {&split.left.front(), &split.right.front()}) {
    CHECK(key->out_handle.x >= 0.0 && key->out_handle.x <= 1.0);
  }
  CHECK(split.left.back().in_handle.x >= 0.0 && split.left.back().in_handle.x <= 1.0);
  CHECK(split.right.back().in_handle.x >= 0.0 && split.right.back().in_handle.x <= 1.0);
}

// --------------------------------------------------------- command envelope ----

CUTLINE_TEST(CommandTypeNamesRoundTrip) {
  // Guards the descriptor table: a command added without a name, or with a name
  // that collides, fails here rather than at runtime.
  const commands::CommandType every[] = {
      commands::CommandType::CreateProject,   commands::CommandType::RenameProject,
      commands::CommandType::CreateBin,       commands::CommandType::RenameBin,
      commands::CommandType::DeleteBin,       commands::CommandType::MoveBin,
      commands::CommandType::ImportMedia,     commands::CommandType::RemoveMedia,
      commands::CommandType::RelinkMedia,     commands::CommandType::SetMediaStreams,
      commands::CommandType::CreateSequence,  commands::CommandType::UpdateSequenceSettings,
      commands::CommandType::DeleteSequence,  commands::CommandType::AddVideoTrack,
      commands::CommandType::AddAudioTrack,   commands::CommandType::RemoveTrack,
      commands::CommandType::SetTrackState,   commands::CommandType::SetTrackRouting, commands::CommandType::SaveTrackingData, commands::CommandType::DeleteTrackingData, commands::CommandType::InsertClip,
      commands::CommandType::DeleteClip,      commands::CommandType::RippleDeleteClip,
      commands::CommandType::MoveClip,        commands::CommandType::SplitClip,
      commands::CommandType::TrimClip,        commands::CommandType::SetClipEnabled,
      commands::CommandType::SetClipSpeed,    commands::CommandType::AddTransition,
      commands::CommandType::RemoveTransition, commands::CommandType::SetTransitionTiming,
      commands::CommandType::AddEffect,       commands::CommandType::RemoveEffect,
      commands::CommandType::SetEffectEnabled, commands::CommandType::ReorderEffect,
      commands::CommandType::SetParameterConstant, commands::CommandType::SetKeyframe,
      commands::CommandType::RemoveKeyframe,  commands::CommandType::AddMarker,
      commands::CommandType::RemoveMarker,    commands::CommandType::UpdateMarker,
  };
  for (const auto type : every) {
    CHECK(commands::ParseCommandType(commands::ToString(type)) == type);
    CHECK(!commands::Label(Envelope(type, commands::CreateProjectPayload{"x"})).empty());
  }
}

CUTLINE_TEST(ValidateRejectsAMismatchedPayload) {
  // AddVideoTrack carrying an InsertClip payload must not be accepted.
  auto command = Envelope(commands::CommandType::AddVideoTrack, commands::InsertClipPayload{});
  CHECK(!commands::TypeMatchesPayload(command));
  CHECK_THROWS(commands::Validate(command));
}

CUTLINE_TEST(ValidateRequiresEnvelopeFields) {
  auto command = Envelope(commands::CommandType::RenameProject, commands::RenameProjectPayload{"Name"});
  CHECK_NO_THROW(commands::Validate(command));

  auto missing_author = command;
  missing_author.author_id.clear();
  CHECK_THROWS(commands::Validate(missing_author));

  auto missing_key = command;
  missing_key.idempotency_key.clear();
  CHECK_THROWS(commands::Validate(missing_key));

  auto negative_revision = command;
  negative_revision.base_revision = -1;
  CHECK_THROWS(commands::Validate(negative_revision));
}

CUTLINE_TEST(CreateProjectIsTheOnlyCommandAllowedWithoutAProjectId) {
  auto create = Envelope(commands::CommandType::CreateProject, commands::CreateProjectPayload{"Film"});
  create.project_id.clear();
  CHECK_NO_THROW(commands::Validate(create));

  auto rename = Envelope(commands::CommandType::RenameProject, commands::RenameProjectPayload{"Film"});
  rename.project_id.clear();
  CHECK_THROWS(commands::Validate(rename));
}

CUTLINE_TEST(ClipSourceReferenceMustMatchItsSourceKind) {
  commands::InsertClipPayload media;
  media.id = "clip-1";
  media.track_id = "v1";
  media.source_kind = model::SourceKind::Media;
  media.source_out = Seconds(2);
  // No media id supplied for a media clip.
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::InsertClip, media)));

  media.media_id = "media-1";
  CHECK_NO_THROW(commands::Validate(Envelope(commands::CommandType::InsertClip, media)));

  // A media clip must not also name a nested sequence.
  media.nested_sequence_id = "seq-2";
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::InsertClip, media)));

  commands::InsertClipPayload adjustment;
  adjustment.id = "clip-2";
  adjustment.track_id = "v1";
  adjustment.source_kind = model::SourceKind::Adjustment;
  adjustment.source_out = Seconds(2);
  CHECK_NO_THROW(commands::Validate(Envelope(commands::CommandType::InsertClip, adjustment)));
  adjustment.media_id = "media-1";
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::InsertClip, adjustment)));
}

CUTLINE_TEST(ClipRangeAndRateMustBePositive) {
  commands::InsertClipPayload clip;
  clip.id = "clip-1";
  clip.track_id = "v1";
  clip.media_id = "media-1";
  clip.source_in = Seconds(5);
  clip.source_out = Seconds(5);  // empty range
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::InsertClip, clip)));

  clip.source_out = Seconds(7);
  CHECK_NO_THROW(commands::Validate(Envelope(commands::CommandType::InsertClip, clip)));

  clip.playback_rate = RationalTime(0, 1);
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::InsertClip, clip)));
}

CUTLINE_TEST(DropFrameSequencesMustUseA1001BasedRate) {
  commands::CreateSequencePayload sequence;
  sequence.id = "seq-1";
  sequence.settings.name = "Main";
  sequence.settings.frame_rate = rates::kFrameRate25;
  sequence.settings.width = 1920;
  sequence.settings.height = 1080;
  sequence.settings.sample_rate = 48000;
  sequence.settings.drop_frame = true;
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::CreateSequence, sequence)));

  sequence.settings.frame_rate = rates::kFrameRate2997;
  CHECK_NO_THROW(commands::Validate(Envelope(commands::CommandType::CreateSequence, sequence)));
}

CUTLINE_TEST(TrackGainAndPanAreBounded) {
  commands::SetTrackStatePayload state;
  state.id = "a1";
  state.pan = 1.5;
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::SetTrackState, state)));
  state.pan = 0.0;
  state.gain_db = 48.0;
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::SetTrackState, state)));
  state.gain_db = -6.0;
  CHECK_NO_THROW(commands::Validate(Envelope(commands::CommandType::SetTrackState, state)));
}

CUTLINE_TEST(TrackRoutingIsValidatedBeforeItReachesTheStore) {
  commands::SetTrackRoutingPayload routing;
  routing.id = "a1";
  routing.output_bus_id = "bus-1";
  routing.sends = {{"bus-2", -6.0, false}};
  CHECK_NO_THROW(commands::Validate(Envelope(commands::CommandType::SetTrackRouting, routing)));

  auto self = routing;
  self.output_bus_id = "a1";
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::SetTrackRouting, self)));
  auto self_send = routing;
  self_send.sends = {{"a1", 0.0, false}};
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::SetTrackRouting, self_send)));
  auto twice = routing;
  twice.sends = {{"bus-2", 0.0, false}, {"bus-2", -3.0, true}};
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::SetTrackRouting, twice)));
  auto loud = routing;
  loud.sends = {{"bus-2", 40.0, false}};
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::SetTrackRouting, loud)));
  auto nameless = routing;
  nameless.id.clear();
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::SetTrackRouting, nameless)));
}

CUTLINE_TEST(TransitionNeedsAtLeastOneSide) {
  commands::AddTransitionPayload transition;
  transition.id = "t-1";
  transition.track_id = "v1";
  transition.kind = "cross_dissolve";
  transition.duration = Seconds(1);
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::AddTransition, transition)));

  transition.to_clip_id = "clip-1";
  CHECK_NO_THROW(commands::Validate(Envelope(commands::CommandType::AddTransition, transition)));

  transition.from_clip_id = "clip-1";
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::AddTransition, transition)));
}

CUTLINE_TEST(EffectParameterNamesMustBeUnique) {
  commands::AddEffectPayload effect;
  effect.id = "fx-1";
  effect.owner_id = "clip-1";
  effect.effect_type = "lumetri";
  effect.parameters = {{"p-1", "exposure", Value::Scalar(0.0)}, {"p-2", "exposure", Value::Scalar(1.0)}};
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::AddEffect, effect)));

  effect.parameters[1].name = "contrast";
  CHECK_NO_THROW(commands::Validate(Envelope(commands::CommandType::AddEffect, effect)));
}

CUTLINE_TEST(EffectRegistryDescribesUniqueBuiltInsWithValidDefaults) {
  const auto& descriptors = cutline::effects::BuiltInEffects();
  CHECK_EQ(descriptors.size(), std::size_t{51});
  std::set<std::string> effect_ids;
  for (const auto& effect : descriptors) {
    CHECK(!effect.id.empty());
    CHECK(!effect.display_name.empty());
    CHECK(!effect.category.empty());
    CHECK(effect.cpu_available);
    CHECK(effect_ids.insert(effect.id).second);
    std::set<std::string> parameter_ids;
    for (const auto& parameter : effect.parameters) {
      CHECK(parameter_ids.insert(parameter.id).second);
      CHECK_EQ(parameter.dimension, parameter.default_value.dimension);
      CHECK(cutline::effects::ValidateParameter(effect.id, parameter.id, parameter.default_value).empty());
      CHECK(!cutline::effects::ToString(parameter.unit).empty());
    }
  }
  CHECK(cutline::effects::FindEffect("blur")->medium == cutline::effects::Medium::Video);
  CHECK(cutline::effects::FindEffect("pan")->medium == cutline::effects::Medium::Audio);
}

CUTLINE_TEST(RegisteredEffectsRejectInvalidParametersButUnknownPluginEffectsRemainSerializable) {
  commands::AddEffectPayload effect;
  effect.id = "fx-1";
  effect.owner_id = "clip-1";
  effect.effect_type = "blur";
  effect.parameters = {{"p-1", "radius", Value::Scalar(12.0)}};
  CHECK_NO_THROW(commands::Validate(Envelope(commands::CommandType::AddEffect, effect)));

  effect.parameters[0].name = "mystery";
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::AddEffect, effect)));
  effect.parameters[0].name = "radius";
  effect.parameters[0].value = Value::Vec2(1.0, 2.0);
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::AddEffect, effect)));
  effect.parameters[0].value = Value::Scalar(129.0);
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::AddEffect, effect)));

  effect.effect_type = "lut";
  effect.parameters = {{"p-1", "intensity", Value::Scalar(1.0)}};
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::AddEffect, effect)));
  effect.preset_name = "looks/film.cube";
  CHECK_NO_THROW(commands::Validate(Envelope(commands::CommandType::AddEffect, effect)));

  effect.effect_type = "third_party.sparkles";
  effect.preset_name.clear();
  effect.parameters = {{"p-1", "vendor-control", Value::Vec4(1.0, 2.0, 3.0, 4.0)}};
  CHECK_NO_THROW(commands::Validate(Envelope(commands::CommandType::AddEffect, effect)));
}

CUTLINE_TEST(MediaStreamsMustHaveDistinctIndicesAndValidGeometry) {
  commands::SetMediaStreamsPayload payload;
  payload.media_id = "media-1";
  commands::MediaStream video;
  video.stream_index = 0;
  video.kind = model::StreamKind::Video;
  video.width = 1920;
  video.height = 1080;
  video.frame_rate = rates::kFrameRate2997;
  payload.streams = {video, video};  // duplicate index
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::SetMediaStreams, payload)));

  payload.streams[1].stream_index = 1;
  payload.streams[1].kind = model::StreamKind::Audio;
  payload.streams[1].sample_rate = 48000;
  payload.streams[1].channel_count = 2;
  CHECK_NO_THROW(commands::Validate(Envelope(commands::CommandType::SetMediaStreams, payload)));

  // A video stream with no frame size is not usable.
  payload.streams[0].width = 0;
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::SetMediaStreams, payload)));
}

CUTLINE_TEST(ImportMediaRequiresAPositiveDuration) {
  commands::ImportMediaPayload media;
  media.id = "media-1";
  media.display_name = "shot.mov";
  media.original_path = "/footage/shot.mov";
  media.fingerprint = "abc";
  CHECK_THROWS(commands::Validate(Envelope(commands::CommandType::ImportMedia, media)));
  media.duration = Seconds(12);
  CHECK_NO_THROW(commands::Validate(Envelope(commands::CommandType::ImportMedia, media)));
}

CUTLINE_TEST(PayloadJsonEscapesControlCharactersAndQuotes) {
  commands::CreateProjectPayload payload;
  payload.name = "Tab\there \"quoted\" \x01 end";
  const auto json = commands::PayloadJson(payload);
  CHECK(json.find("\\t") != std::string::npos);
  CHECK(json.find("\\\"quoted\\\"") != std::string::npos);
  CHECK(json.find("\\u0001") != std::string::npos);
  // A raw control byte in the output would make the journal unparseable.
  CHECK(json.find('\x01') == std::string::npos);
}

CUTLINE_TEST(PayloadJsonWritesRationalTimesAsPairs) {
  commands::MoveClipPayload payload;
  payload.id = "clip-1";
  payload.track_id = "v1";
  payload.timeline_start = cutline::time::RationalTime::FromFrames(48, rates::kFrameRate23976);
  const auto json = commands::PayloadJson(payload);
  // 48 frames at 24000/1001 is 1001/500 seconds exactly; a float would not
  // survive the round trip.
  CHECK(json.find("\"num\":1001") != std::string::npos);
  CHECK(json.find("\"den\":500") != std::string::npos);
}

CUTLINE_TEST(RenderVersionsSelectNamedDecisionsAndUnknownOnesAreRefused) {
  using cutline::model::RenderSemantics;
  CHECK(!RenderSemantics::For(cutline::model::kLegacyRenderVersion).equal_power_crossfades);
  CHECK(RenderSemantics::For(cutline::model::kCurrentRenderVersion).equal_power_crossfades);
  CHECK(RenderSemantics::Known(1) && RenderSemantics::Known(cutline::model::kCurrentRenderVersion));
  CHECK(!RenderSemantics::Known(0) && !RenderSemantics::Known(cutline::model::kCurrentRenderVersion + 1));
  CHECK_THROWS(RenderSemantics::For(0));
  CHECK_THROWS(RenderSemantics::For(cutline::model::kCurrentRenderVersion + 1));
}

int main() { return cutline::testing::RunAll("core"); }
