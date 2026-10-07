#include "core/commands/Command.h"

#include "core/model/RenderVersion.h"
#include "core/util/Json.h"
#include "core/util/JsonParse.h"
#include "effects/EffectRegistry.h"
#include "effects/MaskDocument.h"
#include "effects/GraphicsDocument.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <set>
#include <variant>
#include <vector>

namespace cutline::commands {
namespace {

// One row per command type: the wire name, the undo-stack label, and the index
// of the payload alternative it must carry. Keeping all three together is what
// lets TypeMatchesPayload exist at all, and means a new command cannot be added
// with a mismatched payload without this table failing to compile.
struct Descriptor final {
  CommandType type;
  std::string_view name;
  std::string_view label;
  std::size_t payload_index;
};

// Compile-time lookup of a payload type position in CommandPayload. Done by
// type-list search rather than by constructing a variant, so the descriptor
// table below costs nothing at runtime.
template <typename T, std::size_t Index = 0>
constexpr std::size_t IndexOf() {
  static_assert(Index < std::variant_size_v<CommandPayload>, "Payload type is not part of CommandPayload");
  if constexpr (std::is_same_v<std::variant_alternative_t<Index, CommandPayload>, T>) {
    return Index;
  } else {
    return IndexOf<T, Index + 1>();
  }
}

const std::array<Descriptor, 70>& Descriptors() {
  static const std::array<Descriptor, 70> table{{
      {CommandType::CreateProject, "project.create", "Create Project", IndexOf<CreateProjectPayload>()},
      {CommandType::RenameProject, "project.rename", "Rename Project", IndexOf<RenameProjectPayload>()},
      {CommandType::CreateBin, "bin.create", "New Bin", IndexOf<CreateBinPayload>()},
      {CommandType::RenameBin, "bin.rename", "Rename Bin", IndexOf<RenameBinPayload>()},
      {CommandType::DeleteBin, "bin.delete", "Delete Bin", IndexOf<DeleteBinPayload>()},
      {CommandType::MoveBin, "bin.move", "Move Bin", IndexOf<MoveBinPayload>()},
      {CommandType::ImportMedia, "media.import", "Import Media", IndexOf<ImportMediaPayload>()},
      {CommandType::RemoveMedia, "media.remove", "Remove Media", IndexOf<RemoveMediaPayload>()},
      {CommandType::RelinkMedia, "media.relink", "Relink Media", IndexOf<RelinkMediaPayload>()},
      {CommandType::SetMediaStreams, "media.set_streams", "Update Media Metadata",
       IndexOf<SetMediaStreamsPayload>()},
      {CommandType::AttachProxy, "media.proxy.attach", "Attach Proxy", IndexOf<AttachProxyPayload>()},
      {CommandType::DetachProxy, "media.proxy.detach", "Detach Proxy", IndexOf<DetachProxyPayload>()},
      {CommandType::CreateSequence, "sequence.create", "New Sequence", IndexOf<CreateSequencePayload>()},
      {CommandType::UpdateSequenceSettings, "sequence.update_settings", "Sequence Settings",
       IndexOf<UpdateSequenceSettingsPayload>()},
      {CommandType::DeleteSequence, "sequence.delete", "Delete Sequence", IndexOf<DeleteSequencePayload>()},
      {CommandType::AddVideoTrack, "track.add_video", "Add Video Track", IndexOf<AddTrackPayload>()},
      {CommandType::AddAudioTrack, "track.add_audio", "Add Audio Track", IndexOf<AddTrackPayload>()},
      {CommandType::RemoveTrack, "track.remove", "Delete Track", IndexOf<RemoveTrackPayload>()},
      {CommandType::SetTrackState, "track.set_state", "Track Settings", IndexOf<SetTrackStatePayload>()},
      {CommandType::SetTrackRouting, "track.set_routing", "Track Routing", IndexOf<SetTrackRoutingPayload>()},
      {CommandType::InsertClip, "clip.insert", "Insert Clip", IndexOf<InsertClipPayload>()},
      {CommandType::DeleteClip, "clip.delete", "Delete Clip", IndexOf<DeleteClipPayload>()},
      {CommandType::RippleDeleteClip, "clip.ripple_delete", "Ripple Delete", IndexOf<RippleDeleteClipPayload>()},
      {CommandType::MoveClip, "clip.move", "Move Clip", IndexOf<MoveClipPayload>()},
      {CommandType::SplitClip, "clip.split", "Split Clip", IndexOf<SplitClipPayload>()},
      {CommandType::TrimClip, "clip.trim", "Trim Clip", IndexOf<TrimClipPayload>()},
      {CommandType::SetClipEnabled, "clip.set_enabled", "Enable Clip", IndexOf<SetClipEnabledPayload>()},
      {CommandType::SetClipSpeed, "clip.set_speed", "Speed / Duration", IndexOf<SetClipSpeedPayload>()},
      {CommandType::LinkClips, "clip.link", "Link Clips", IndexOf<LinkClipsPayload>()},
      {CommandType::UnlinkClips, "clip.unlink", "Unlink Clips", IndexOf<UnlinkClipsPayload>()},
      {CommandType::SetSpeedRamp, "clip.set_speed_ramp", "Speed Ramp", IndexOf<SetSpeedRampPayload>()},
      {CommandType::ClearSpeedRamp, "clip.clear_speed_ramp", "Remove Speed Ramp", IndexOf<ClearSpeedRampPayload>()},
      {CommandType::CreateMulticamGroup, "multicam.create", "Create Multicam Group", IndexOf<CreateMulticamGroupPayload>()},
      {CommandType::SetMulticamSync, "multicam.set_sync", "Sync Multicam Angles", IndexOf<SetMulticamSyncPayload>()},
      {CommandType::RecordMulticamSwitch, "multicam.switch", "Multicam Cut", IndexOf<RecordMulticamSwitchPayload>()},
      {CommandType::RemoveMulticamSwitch, "multicam.remove_switch", "Remove Multicam Cut", IndexOf<RemoveMulticamSwitchPayload>()},
      {CommandType::RenameMulticamAngle, "multicam.rename_angle", "Rename Multicam Angle", IndexOf<RenameMulticamAnglePayload>()},
      {CommandType::DeleteMulticamGroup, "multicam.delete", "Delete Multicam Group", IndexOf<DeleteMulticamGroupPayload>()},
      {CommandType::FlattenMulticamGroup, "multicam.flatten", "Flatten Multicam", IndexOf<FlattenMulticamGroupPayload>()},
      {CommandType::CreateGraphic, "graphic.create", "Create Graphic", IndexOf<CreateGraphicPayload>()},
      {CommandType::UpdateGraphic, "graphic.update", "Edit Graphic", IndexOf<UpdateGraphicPayload>()},
      {CommandType::DeleteGraphic, "graphic.delete", "Delete Graphic", IndexOf<DeleteGraphicPayload>()},
      {CommandType::InstallGraphicTemplate, "graphic.install_template", "Install Template", IndexOf<InstallGraphicTemplatePayload>()},
      {CommandType::RemoveGraphicTemplate, "graphic.remove_template", "Remove Template", IndexOf<RemoveGraphicTemplatePayload>()},
      {CommandType::AddGraphicClip, "graphic.add_clip", "Add Graphic", IndexOf<AddGraphicClipPayload>()},
      {CommandType::SaveTrackingData, "analysis.save_tracking", "Save Tracking Data", IndexOf<SaveTrackingDataPayload>()},
      {CommandType::DeleteTrackingData, "analysis.delete_tracking", "Delete Tracking Data", IndexOf<DeleteTrackingDataPayload>()},
      {CommandType::AddCaptionTrack, "caption.add_track", "Add Caption Track", IndexOf<AddCaptionTrackPayload>()},
      {CommandType::RemoveCaptionTrack, "caption.remove_track", "Delete Caption Track", IndexOf<RemoveCaptionTrackPayload>()},
      {CommandType::UpdateCaptionTrack, "caption.update_track", "Caption Track Settings", IndexOf<UpdateCaptionTrackPayload>()},
      {CommandType::AddCaptions, "caption.add", "Add Captions", IndexOf<AddCaptionsPayload>()},
      {CommandType::UpdateCaption, "caption.update", "Edit Caption", IndexOf<UpdateCaptionPayload>()},
      {CommandType::RemoveCaptions, "caption.remove", "Delete Captions", IndexOf<RemoveCaptionsPayload>()},
      {CommandType::AddTransition, "transition.add", "Apply Transition", IndexOf<AddTransitionPayload>()},
      {CommandType::RemoveTransition, "transition.remove", "Delete Transition", IndexOf<RemoveTransitionPayload>()},
      {CommandType::SetTransitionTiming, "transition.set_timing", "Transition Duration",
       IndexOf<SetTransitionTimingPayload>()},
      {CommandType::AddEffect, "effect.add", "Add Effect", IndexOf<AddEffectPayload>()},
      {CommandType::RemoveEffect, "effect.remove", "Remove Effect", IndexOf<RemoveEffectPayload>()},
      {CommandType::SetEffectEnabled, "effect.set_enabled", "Toggle Effect", IndexOf<SetEffectEnabledPayload>()},
      {CommandType::ReorderEffect, "effect.reorder", "Reorder Effect", IndexOf<ReorderEffectPayload>()},
      {CommandType::AddMask, "mask.add", "Add Mask", IndexOf<AddMaskPayload>()},
      {CommandType::UpdateMask, "mask.update", "Edit Mask", IndexOf<UpdateMaskPayload>()},
      {CommandType::RemoveMask, "mask.remove", "Delete Mask", IndexOf<RemoveMaskPayload>()},
      {CommandType::SetParameterConstant, "parameter.set_constant", "Change Parameter",
       IndexOf<SetParameterConstantPayload>()},
      {CommandType::SetKeyframe, "parameter.set_keyframe", "Add Keyframe", IndexOf<SetKeyframePayload>()},
      {CommandType::RemoveKeyframe, "parameter.remove_keyframe", "Delete Keyframe",
       IndexOf<RemoveKeyframePayload>()},
      {CommandType::AddMarker, "marker.add", "Add Marker", IndexOf<AddMarkerPayload>()},
      {CommandType::RemoveMarker, "marker.remove", "Delete Marker", IndexOf<RemoveMarkerPayload>()},
      {CommandType::UpdateMarker, "marker.update", "Edit Marker", IndexOf<UpdateMarkerPayload>()},
      {CommandType::SetClipAudioRole, "clip.set_audio_role", "Set Audio Role", IndexOf<SetClipAudioRolePayload>()},
  }};
  return table;
}

const Descriptor& Find(CommandType type) {
  for (const auto& descriptor : Descriptors()) {
    if (descriptor.type == type) return descriptor;
  }
  throw std::invalid_argument("Unhandled command type");
}

void Require(bool condition, const char* message) {
  if (!condition) throw std::invalid_argument(message);
}

void RequireIdentifier(const std::string& value, const char* what) {
  if (value.empty()) throw std::invalid_argument(std::string(what) + " must not be empty");
  if (value.size() > 256) throw std::invalid_argument(std::string(what) + " is longer than 256 characters");
}

void RequirePositiveRate(const time::FrameRate& rate, const char* what) {
  if (rate.numerator <= 0 || rate.denominator <= 0) {
    throw std::invalid_argument(std::string(what) + " must be a positive frame rate");
  }
}

void ValidateSequenceSettings(const SequenceSettings& settings) {
  RequireIdentifier(settings.name, "Sequence name");
  RequirePositiveRate(settings.frame_rate, "Sequence frame rate");
  RequirePositiveRate(settings.pixel_aspect, "Sequence pixel aspect ratio");
  Require(settings.width > 0 && settings.height > 0, "Sequence frame size must be positive");
  Require(settings.sample_rate > 0, "Sequence sample rate must be positive");
  Require(!settings.channel_layout.empty(), "Sequence channel layout must not be empty");
  Require(!settings.working_color_space.empty(), "Sequence working colour space must not be empty");
  Require(!settings.display_color_space.empty(), "Sequence display colour space must not be empty");
  if (settings.render_version.has_value()) {
    Require(model::RenderSemantics::Known(*settings.render_version),
            "Sequence render version is not one this build knows");
  }
  if (settings.drop_frame) {
    // Drop-frame timecode is only defined for the 1001-based 30 and 60 families.
    const bool supported = (settings.frame_rate.numerator == 30000 || settings.frame_rate.numerator == 60000) &&
                           settings.frame_rate.denominator == 1001;
    Require(supported, "Drop-frame timecode requires a 30000/1001 or 60000/1001 sequence rate");
  }
}

// ----------------------------------------------------------- JSON writers ----

std::string ToJson(const CreateProjectPayload& payload) { return json::Object().Add("name", payload.name).Build(); }
std::string ToJson(const RenameProjectPayload& payload) { return json::Object().Add("name", payload.name).Build(); }

std::string ToJson(const CreateBinPayload& payload) {
  return json::Object()
      .Add("id", payload.id)
      .Add("parentId", payload.parent_id)
      .Add("name", payload.name)
      .Add("order", payload.order)
      .Build();
}

std::string ToJson(const RenameBinPayload& payload) {
  return json::Object().Add("id", payload.id).Add("name", payload.name).Build();
}

std::string ToJson(const DeleteBinPayload& payload) { return json::Object().Add("id", payload.id).Build(); }

std::string ToJson(const MoveBinPayload& payload) {
  return json::Object()
      .Add("id", payload.id)
      .Add("parentId", payload.parent_id)
      .Add("order", payload.order)
      .Build();
}

std::string ToJson(const MediaStream& stream) {
  return json::Object()
      .Add("streamIndex", stream.stream_index)
      .Add("kind", model::ToString(stream.kind))
      .Add("codec", stream.codec)
      .Add("width", stream.width)
      .Add("height", stream.height)
      .Add("pixelAspect", stream.pixel_aspect)
      .Add("frameRate", stream.frame_rate)
      .Add("cadence", model::ToString(stream.cadence))
      .Add("bitDepth", stream.bit_depth)
      .Add("chroma", stream.chroma)
      .Add("fieldOrder", model::ToString(stream.field_order))
      .Add("colorPrimaries", stream.color_primaries)
      .Add("colorTransfer", stream.color_transfer)
      .Add("colorMatrix", stream.color_matrix)
      .Add("colorRange", model::ToString(stream.color_range))
      .Add("sampleRate", stream.sample_rate)
      .Add("channelCount", stream.channel_count)
      .Add("channelLayout", stream.channel_layout)
      .Build();
}

std::string StreamsJson(const std::vector<MediaStream>& streams) {
  std::vector<std::string> entries;
  entries.reserve(streams.size());
  for (const auto& stream : streams) entries.push_back(ToJson(stream));
  return json::Array(entries);
}

std::string ToJson(const ImportMediaPayload& payload) {
  return json::Object()
      .Add("id", payload.id)
      .Add("binId", payload.bin_id)
      .Add("displayName", payload.display_name)
      .Add("originalPath", payload.original_path)
      .Add("fingerprint", payload.fingerprint)
      .Add("duration", payload.duration)
      .Add("startTimecode", payload.start_timecode)
      .AddRaw("streams", StreamsJson(payload.streams))
      .Build();
}

std::string ToJson(const RemoveMediaPayload& payload) { return json::Object().Add("id", payload.id).Build(); }

std::string ToJson(const RelinkMediaPayload& payload) {
  return json::Object()
      .Add("id", payload.id)
      .Add("originalPath", payload.original_path)
      .Add("missing", payload.missing)
      .Add("fingerprint", payload.fingerprint)
      .Build();
}

std::string ToJson(const SetMediaStreamsPayload& payload) {
  std::vector<std::string> streams;
  streams.reserve(payload.streams.size());
  for (const auto& stream : payload.streams) streams.push_back(ToJson(stream));
  return json::Object().Add("mediaId", payload.media_id).AddRaw("streams", json::Array(streams)).Build();
}

std::string ToJson(const AttachProxyPayload& payload) {
  return json::Object()
      .Add("mediaId", payload.media_id)
      .Add("path", payload.path)
      .Add("fingerprint", payload.fingerprint)
      .Add("sourceFingerprint", payload.source_fingerprint)
      .Add("codec", payload.codec)
      .Add("width", payload.width)
      .Add("height", payload.height)
      .Build();
}

std::string ToJson(const DetachProxyPayload& payload) {
  return json::Object().Add("mediaId", payload.media_id).Build();
}

std::string ToJson(const SequenceSettings& settings) {
  auto object = json::Object();
  object
      .Add("name", settings.name)
      .Add("frameRate", settings.frame_rate)
      .Add("width", settings.width)
      .Add("height", settings.height)
      .Add("pixelAspect", settings.pixel_aspect)
      .Add("sampleRate", settings.sample_rate)
      .Add("channelLayout", settings.channel_layout)
      .Add("workingColorSpace", settings.working_color_space)
      .Add("displayColorSpace", settings.display_color_space)
      .Add("fieldOrder", model::ToString(settings.field_order))
      .Add("dropFrame", settings.drop_frame);
  if (settings.render_version.has_value()) object.Add("renderVersion", *settings.render_version);
  return object.Build();
}

std::string ToJson(const CreateSequencePayload& payload) {
  return json::Object().Add("id", payload.id).AddRaw("settings", ToJson(payload.settings)).Build();
}

std::string ToJson(const UpdateSequenceSettingsPayload& payload) {
  return json::Object().Add("id", payload.id).AddRaw("settings", ToJson(payload.settings)).Build();
}

std::string ToJson(const DeleteSequencePayload& payload) { return json::Object().Add("id", payload.id).Build(); }

std::string ToJson(const AddTrackPayload& payload) {
  return json::Object()
      .Add("id", payload.id)
      .Add("sequenceId", payload.sequence_id)
      .Add("order", payload.order)
      .Add("channelLayout", payload.channel_layout)
      .Add("name", payload.name)
      .Build();
}

std::string ToJson(const RemoveTrackPayload& payload) { return json::Object().Add("id", payload.id).Build(); }

std::string ToJson(const SetTrackStatePayload& payload) {
  return json::Object()
      .Add("id", payload.id)
      .Add("locked", payload.locked)
      .Add("muted", payload.muted)
      .Add("solo", payload.solo)
      .Add("gainDb", payload.gain_db)
      .Add("pan", payload.pan)
      .Add("name", payload.name)
      .Build();
}

std::string ToJson(const SetTrackRoutingPayload& payload) {
  std::string sends = "[";
  for (std::size_t index = 0; index < payload.sends.size(); ++index) {
    if (index != 0) sends += ",";
    sends += json::Object()
                 .Add("busId", payload.sends[index].bus_id)
                 .Add("gainDb", payload.sends[index].gain_db)
                 .Add("preFader", payload.sends[index].pre_fader)
                 .Build();
  }
  sends += "]";
  return json::Object()
      .Add("id", payload.id)
      .Add("isBus", payload.is_bus)
      .Add("outputBusId", payload.output_bus_id)
      .AddRaw("sends", sends)
      .Build();
}

std::string ToJson(const SetSpeedRampPayload& payload) {
  std::vector<std::string> segments;
  for (const auto& s : payload.segments) {
    segments.push_back(json::Object().Add("duration", s.duration).Add("startSpeed", s.start_speed).Add("endSpeed", s.end_speed).Build());
  }
  return json::Object().Add("clipId", payload.clip_id).AddRaw("segments", json::Array(segments)).Add("propagateLinks", payload.propagate_links).Build();
}
std::string ToJson(const ClearSpeedRampPayload& payload) {
  return json::Object().Add("clipId", payload.clip_id).Add("propagateLinks", payload.propagate_links).Build();
}

std::string ToJson(const CreateMulticamGroupPayload& payload) {
  std::vector<std::string> angles;
  for (const auto& a : payload.angles) {
    auto angle = json::Object().Add("id", a.id).Add("mediaId", a.media_id).Add("name", a.name);
    if (a.marker) angle.Add("marker", *a.marker);
    angles.push_back(angle.Build());
  }
  return json::Object().Add("id", payload.id).Add("name", payload.name).Add("duration", payload.duration).AddRaw("angles", json::Array(angles)).Build();
}
std::string ToJson(const SetMulticamSyncPayload& payload) {
  std::vector<std::string> offsets;
  for (const auto& o : payload.offsets) offsets.push_back(json::Object().Add("angleId", o.angle_id).Add("sourceOffset", o.source_offset).Build());
  return json::Object()
      .Add("groupId", payload.group_id)
      .Add("method", payload.method)
      .Add("referenceAngleId", payload.reference_angle_id)
      .Add("confidence", payload.confidence)
      .AddRaw("offsets", json::Array(offsets))
      .Build();
}
std::string ToJson(const RecordMulticamSwitchPayload& payload) {
  return json::Object().Add("groupId", payload.group_id).Add("at", payload.at).Add("angleId", payload.angle_id).Build();
}
std::string ToJson(const RemoveMulticamSwitchPayload& payload) {
  return json::Object().Add("groupId", payload.group_id).Add("at", payload.at).Build();
}
std::string ToJson(const RenameMulticamAnglePayload& payload) {
  return json::Object().Add("groupId", payload.group_id).Add("angleId", payload.angle_id).Add("name", payload.name).Build();
}
std::string ToJson(const DeleteMulticamGroupPayload& payload) { return json::Object().Add("id", payload.id).Build(); }
std::string ToJson(const FlattenMulticamGroupPayload& payload) {
  return json::Object()
      .Add("groupId", payload.group_id)
      .Add("videoTrackId", payload.video_track_id)
      .Add("audioTrackId", payload.audio_track_id)
      .Add("audioAngleId", payload.audio_angle_id)
      .Add("timelineStart", payload.timeline_start)
      .Add("idPrefix", payload.id_prefix)
      .Build();
}

std::string ToJson(const CreateGraphicPayload& payload) {
  auto values = json::Object();
  for (const auto& [name, value] : payload.values) values.Add(name, value);
  return json::Object()
      .Add("id", payload.id)
      .Add("name", payload.name)
      .Add("kind", payload.kind)
      .Add("documentBytes", static_cast<std::int64_t>(payload.document_json.size()))
      .Add("templateId", payload.template_id)
      .Add("templateVersion", payload.template_version)
      .AddRaw("values", values.Build())
      .Build();
}
std::string ToJson(const UpdateGraphicPayload& payload) {
  auto object = json::Object().Add("id", payload.id);
  if (payload.name) object.Add("name", *payload.name);
  if (payload.document_json) object.Add("documentBytes", static_cast<std::int64_t>(payload.document_json->size()));
  if (payload.values) {
    auto values = json::Object();
    for (const auto& [name, value] : *payload.values) values.Add(name, value);
    object.AddRaw("values", values.Build());
  }
  return object.Build();
}
std::string ToJson(const DeleteGraphicPayload& payload) { return json::Object().Add("id", payload.id).Build(); }
std::string ToJson(const InstallGraphicTemplatePayload& payload) {
  return json::Object().Add("packageBytes", static_cast<std::int64_t>(payload.package_json.size())).Build();
}
std::string ToJson(const RemoveGraphicTemplatePayload& payload) {
  return json::Object().Add("templateId", payload.template_id).Add("version", payload.version).Build();
}
std::string ToJson(const AddGraphicClipPayload& payload) {
  return json::Object()
      .Add("clipId", payload.clip_id)
      .Add("trackId", payload.track_id)
      .Add("graphicId", payload.graphic_id)
      .Add("timelineStart", payload.timeline_start)
      .Add("duration", payload.duration)
      .Add("name", payload.name)
      .Build();
}

std::string ToJson(const SaveTrackingDataPayload& payload) {
  return json::Object()
      .Add("id", payload.id)
      .Add("clipId", payload.clip_id)
      .Add("kind", payload.kind)
      .Add("name", payload.name)
      .Add("algorithm", payload.algorithm)
      .Add("sourceFingerprint", payload.source_fingerprint)
      .Add("parametersJson", payload.parameters_json)
      .Add("dataBytes", static_cast<std::int64_t>(payload.data_json.size()))
      .Build();
}

std::string ToJson(const DeleteTrackingDataPayload& payload) { return json::Object().Add("id", payload.id).Build(); }

std::string ToJson(const AddCaptionTrackPayload& payload) {
  return json::Object()
      .Add("id", payload.id)
      .Add("sequenceId", payload.sequence_id)
      .Add("name", payload.name)
      .Add("language", payload.language)
      .Add("styleJson", payload.style_json)
      .Build();
}
std::string ToJson(const RemoveCaptionTrackPayload& payload) { return json::Object().Add("id", payload.id).Build(); }
std::string ToJson(const UpdateCaptionTrackPayload& payload) {
  return json::Object().Add("id", payload.id).Add("name", payload.name).Add("language", payload.language).Add("styleJson", payload.style_json).Build();
}
std::string ToJson(const AddCaptionsPayload& payload) {
  // The journal records which cues, not the whole of their text: it is an audit trail, and a
  // subtitle file is as long as the film.
  std::vector<std::string> ids;
  std::int64_t bytes = 0;
  for (const auto& cue : payload.cues) {
    ids.push_back("\"" + json::Escape(cue.id) + "\"");
    bytes += static_cast<std::int64_t>(cue.text.size());
  }
  return json::Object().Add("trackId", payload.track_id).Add("cues", static_cast<std::int64_t>(payload.cues.size())).Add("textBytes", bytes).AddRaw("ids", json::Array(ids)).Build();
}
std::string ToJson(const UpdateCaptionPayload& payload) {
  return json::Object()
      .Add("id", payload.cue.id)
      .Add("start", payload.cue.start)
      .Add("end", payload.cue.end)
      .Add("textBytes", static_cast<std::int64_t>(payload.cue.text.size()))
      .Add("styleJson", payload.cue.style_json)
      .Add("speaker", payload.cue.speaker)
      .Build();
}
std::string ToJson(const RemoveCaptionsPayload& payload) {
  std::vector<std::string> ids;
  for (const auto& id : payload.ids) ids.push_back("\"" + json::Escape(id) + "\"");
  return json::Object().AddRaw("ids", json::Array(ids)).Build();
}

std::string ToJson(const InsertClipPayload& payload) {
  return json::Object()
      .Add("id", payload.id)
      .Add("trackId", payload.track_id)
      .Add("sourceKind", model::ToString(payload.source_kind))
      .Add("mediaId", payload.media_id)
      .Add("nestedSequenceId", payload.nested_sequence_id)
      .Add("sourceIn", payload.source_in)
      .Add("sourceOut", payload.source_out)
      .Add("timelineStart", payload.timeline_start)
      .Add("playbackRate", payload.playback_rate)
      .Add("reversed", payload.reversed)
      .Add("linkedGroup", payload.linked_group)
      .Add("name", payload.name)
      .Build();
}

std::string ToJson(const DeleteClipPayload& payload) {
  return json::Object().Add("id", payload.id).Add("propagateLinks", payload.propagate_links).Build();
}
std::string ToJson(const RippleDeleteClipPayload& payload) {
  return json::Object().Add("id", payload.id).Add("propagateLinks", payload.propagate_links).Build();
}

std::string ToJson(const MoveClipPayload& payload) {
  return json::Object()
      .Add("id", payload.id)
      .Add("trackId", payload.track_id)
      .Add("timelineStart", payload.timeline_start)
      .Add("propagateLinks", payload.propagate_links)
      .Build();
}

std::string ToJson(const SplitClipPayload& payload) {
  return json::Object()
      .Add("id", payload.id)
      .Add("newClipId", payload.new_clip_id)
      .Add("at", payload.at)
      .Add("propagateLinks", payload.propagate_links)
      .Build();
}

std::string ToJson(const TrimClipPayload& payload) {
  return json::Object()
      .Add("id", payload.id)
      .Add("sourceIn", payload.source_in)
      .Add("sourceOut", payload.source_out)
      .Add("timelineStart", payload.timeline_start)
      .Add("propagateLinks", payload.propagate_links)
      .Build();
}

std::string ToJson(const SetClipAudioRolePayload& payload) {
  return json::Object().Add("id", payload.id).Add("role", payload.role).Build();
}

std::string ToJson(const SetClipEnabledPayload& payload) {
  return json::Object()
      .Add("id", payload.id)
      .Add("enabled", payload.enabled)
      .Add("propagateLinks", payload.propagate_links)
      .Build();
}

std::string ToJson(const SetClipSpeedPayload& payload) {
  return json::Object()
      .Add("id", payload.id)
      .Add("playbackRate", payload.playback_rate)
      .Add("reversed", payload.reversed)
      .Add("propagateLinks", payload.propagate_links)
      .Add("maintainPitch", payload.maintain_pitch)
      .Build();
}

std::string ToJson(const LinkClipsPayload& payload) {
  std::vector<std::string> ids;
  for (const auto& id : payload.clip_ids) ids.push_back(std::string(1, '"') + json::Escape(id) + '"');
  return json::Object().AddRaw("clipIds", json::Array(ids)).Add("groupId", payload.group_id).Build();
}

std::string ToJson(const UnlinkClipsPayload& payload) {
  std::vector<std::string> ids;
  for (const auto& id : payload.clip_ids) ids.push_back(std::string(1, '"') + json::Escape(id) + '"');
  return json::Object().AddRaw("clipIds", json::Array(ids)).Build();
}

std::string ToJson(const AddTransitionPayload& payload) {
  return json::Object()
      .Add("id", payload.id)
      .Add("trackId", payload.track_id)
      .Add("kind", payload.kind)
      .Add("alignment", model::ToString(payload.alignment))
      .Add("fromClipId", payload.from_clip_id)
      .Add("toClipId", payload.to_clip_id)
      .Add("timelineStart", payload.timeline_start)
      .Add("duration", payload.duration)
      .Build();
}

std::string ToJson(const RemoveTransitionPayload& payload) { return json::Object().Add("id", payload.id).Build(); }

std::string ToJson(const SetTransitionTimingPayload& payload) {
  return json::Object()
      .Add("id", payload.id)
      .Add("timelineStart", payload.timeline_start)
      .Add("duration", payload.duration)
      .Build();
}

std::string ToJson(const EffectParameter& parameter) {
  return json::Object()
      .Add("id", parameter.id)
      .Add("name", parameter.name)
      .Add("dimension", static_cast<std::int64_t>(parameter.value.dimension))
      .Add("value", parameter.value)
      .Build();
}

std::string ToJson(const AddEffectPayload& payload) {
  std::vector<std::string> parameters;
  parameters.reserve(payload.parameters.size());
  for (const auto& parameter : payload.parameters) parameters.push_back(ToJson(parameter));
  return json::Object()
      .Add("id", payload.id)
      .Add("ownerKind", model::ToString(payload.owner_kind))
      .Add("ownerId", payload.owner_id)
      .Add("effectType", payload.effect_type)
      .Add("order", payload.order)
      .Add("intrinsic", payload.intrinsic)
      .Add("presetName", payload.preset_name)
      .AddRaw("parameters", json::Array(parameters))
      .Build();
}

std::string ToJson(const RemoveEffectPayload& payload) { return json::Object().Add("id", payload.id).Build(); }

std::string ToJson(const SetEffectEnabledPayload& payload) {
  return json::Object().Add("id", payload.id).Add("enabled", payload.enabled).Build();
}

std::string ToJson(const ReorderEffectPayload& payload) {
  return json::Object().Add("id", payload.id).Add("order", payload.order).Build();
}

std::string ToJson(const AddMaskPayload& payload) {
  return json::Object().Add("id", payload.id).Add("effectId", payload.effect_id).Add("order", payload.order)
      .AddRaw("document", effects::mask::ToJson(effects::mask::Parse(payload.document_json))).Build();
}

std::string ToJson(const UpdateMaskPayload& payload) {
  return json::Object().Add("id", payload.id)
      .AddRaw("document", effects::mask::ToJson(effects::mask::Parse(payload.document_json))).Build();
}

std::string ToJson(const RemoveMaskPayload& payload) { return json::Object().Add("id", payload.id).Build(); }

std::string ToJson(const SetParameterConstantPayload& payload) {
  return json::Object()
      .Add("parameterId", payload.parameter_id)
      .Add("dimension", static_cast<std::int64_t>(payload.value.dimension))
      .Add("value", payload.value)
      .Build();
}

std::string ToJson(const SetKeyframePayload& payload) {
  json::Object keyframe;
  keyframe.Add("time", payload.keyframe.time)
      .Add("dimension", static_cast<std::int64_t>(payload.keyframe.value.dimension))
      .Add("value", payload.keyframe.value)
      .Add("interpolation", anim::ToString(payload.keyframe.interpolation))
      .Add("outHandleX", payload.keyframe.out_handle.x)
      .Add("outHandleY", payload.keyframe.out_handle.y)
      .Add("inHandleX", payload.keyframe.in_handle.x)
      .Add("inHandleY", payload.keyframe.in_handle.y);
  return json::Object().Add("parameterId", payload.parameter_id).AddRaw("keyframe", keyframe.Build()).Build();
}

std::string ToJson(const RemoveKeyframePayload& payload) {
  return json::Object().Add("parameterId", payload.parameter_id).Add("at", payload.at).Build();
}

std::string ToJson(const AddMarkerPayload& payload) {
  return json::Object()
      .Add("id", payload.id)
      .Add("ownerKind", model::ToString(payload.owner_kind))
      .Add("ownerId", payload.owner_id)
      .Add("start", payload.start)
      .Add("end", payload.end)
      .Add("label", payload.label)
      .Add("kind", model::ToString(payload.kind))
      .Add("color", payload.color)
      .Add("metadataJson", payload.metadata_json)
      .Build();
}

std::string ToJson(const RemoveMarkerPayload& payload) { return json::Object().Add("id", payload.id).Build(); }

std::string ToJson(const UpdateMarkerPayload& payload) {
  return json::Object()
      .Add("id", payload.id)
      .Add("start", payload.start)
      .Add("end", payload.end)
      .Add("label", payload.label)
      .Add("kind", model::ToString(payload.kind))
      .Add("color", payload.color)
      .Add("metadataJson", payload.metadata_json)
      .Build();
}

// ------------------------------------------------------------- validators ----

void Check(const CreateProjectPayload& payload) { RequireIdentifier(payload.name, "Project name"); }
void Check(const RenameProjectPayload& payload) { RequireIdentifier(payload.name, "Project name"); }

void Check(const CreateBinPayload& payload) {
  RequireIdentifier(payload.id, "Bin id");
  RequireIdentifier(payload.name, "Bin name");
  Require(!payload.parent_id.has_value() || *payload.parent_id != payload.id, "A bin cannot be its own parent");
}

void Check(const RenameBinPayload& payload) {
  RequireIdentifier(payload.id, "Bin id");
  RequireIdentifier(payload.name, "Bin name");
}

void Check(const DeleteBinPayload& payload) { RequireIdentifier(payload.id, "Bin id"); }

void Check(const MoveBinPayload& payload) {
  RequireIdentifier(payload.id, "Bin id");
  Require(!payload.parent_id.has_value() || *payload.parent_id != payload.id, "A bin cannot be its own parent");
}

void Check(const MediaStream& stream) {
  Require(stream.stream_index >= 0, "Stream index must not be negative");
  Require(stream.bit_depth > 0, "Stream bit depth must be positive");
  RequirePositiveRate(stream.pixel_aspect, "Stream pixel aspect ratio");
  if (stream.kind == model::StreamKind::Video) {
    Require(stream.width > 0 && stream.height > 0, "Video stream must have a positive frame size");
    RequirePositiveRate(stream.frame_rate, "Video stream frame rate");
  }
  if (stream.kind == model::StreamKind::Audio) {
    Require(stream.sample_rate > 0, "Audio stream sample rate must be positive");
    Require(stream.channel_count > 0, "Audio stream must have at least one channel");
  }
}

void Check(const ImportMediaPayload& payload) {
  RequireIdentifier(payload.id, "Media id");
  RequireIdentifier(payload.display_name, "Media display name");
  Require(!payload.original_path.empty(), "Media path must not be empty");
  Require(!payload.fingerprint.empty(), "Media fingerprint must not be empty");
  Require(payload.duration.Compare({0, 1}) > 0, "Media duration must be positive");
}

void Check(const RemoveMediaPayload& payload) { RequireIdentifier(payload.id, "Media id"); }

void Check(const RelinkMediaPayload& payload) {
  RequireIdentifier(payload.id, "Media id");
  Require(!payload.original_path.empty(), "Media path must not be empty");
}

void Check(const SetMediaStreamsPayload& payload) {
  RequireIdentifier(payload.media_id, "Media id");
  Require(!payload.streams.empty(), "A media item must have at least one stream");
  for (const auto& stream : payload.streams) Check(stream);
  for (std::size_t outer = 0; outer < payload.streams.size(); ++outer) {
    for (std::size_t inner = outer + 1; inner < payload.streams.size(); ++inner) {
      Require(payload.streams[outer].stream_index != payload.streams[inner].stream_index,
              "Two streams share the same index");
    }
  }
}

void Check(const AttachProxyPayload& payload) {
  RequireIdentifier(payload.media_id, "Media id");
  Require(!payload.path.empty(), "Proxy path must not be empty");
  Require(!payload.fingerprint.empty(), "Proxy fingerprint must not be empty");
  Require(!payload.source_fingerprint.empty(), "Proxy source fingerprint must not be empty");
  Require(!payload.codec.empty(), "Proxy codec must not be empty");
  Require(payload.width > 0 && payload.height > 0, "Proxy frame size must be positive");
}

void Check(const DetachProxyPayload& payload) { RequireIdentifier(payload.media_id, "Media id"); }

void Check(const CreateSequencePayload& payload) {
  RequireIdentifier(payload.id, "Sequence id");
  ValidateSequenceSettings(payload.settings);
}

void Check(const UpdateSequenceSettingsPayload& payload) {
  RequireIdentifier(payload.id, "Sequence id");
  ValidateSequenceSettings(payload.settings);
}

void Check(const DeleteSequencePayload& payload) { RequireIdentifier(payload.id, "Sequence id"); }

void Check(const AddTrackPayload& payload) {
  RequireIdentifier(payload.id, "Track id");
  RequireIdentifier(payload.sequence_id, "Sequence id");
  Require(payload.order >= 0, "Track order must not be negative");
  Require(!payload.channel_layout.empty(), "Track channel layout must not be empty");
}

void Check(const RemoveTrackPayload& payload) { RequireIdentifier(payload.id, "Track id"); }

void Check(const SetTrackStatePayload& payload) {
  RequireIdentifier(payload.id, "Track id");
  Require(payload.pan >= -1.0 && payload.pan <= 1.0, "Track pan must be between -1 and 1");
  Require(payload.gain_db >= -96.0 && payload.gain_db <= 24.0, "Track gain must be between -96 and +24 dB");
}

void Check(const SetTrackRoutingPayload& payload) {
  RequireIdentifier(payload.id, "Track id");
  Require(payload.output_bus_id != payload.id, "A track cannot be routed to itself");
  std::vector<std::string> seen;
  for (const auto& send : payload.sends) {
    RequireIdentifier(send.bus_id, "Send bus id");
    Require(send.bus_id != payload.id, "A track cannot send to itself");
    Require(send.gain_db >= -96.0 && send.gain_db <= 24.0, "Send gain must be between -96 and +24 dB");
    Require(std::find(seen.begin(), seen.end(), send.bus_id) == seen.end(), "A track has one send to each bus");
    seen.push_back(send.bus_id);
  }
}

void Check(const SetSpeedRampPayload& payload) {
  RequireIdentifier(payload.clip_id, "Clip id");
  Require(!payload.segments.empty(), "A speed ramp needs at least one segment");
  Require(payload.segments.size() <= 1000, "A speed ramp has too many segments");
  for (const auto& s : payload.segments) {
    Require(s.duration.Compare({0, 1}) > 0, "A speed ramp segment must have a length");
    Require(std::isfinite(s.start_speed) && std::isfinite(s.end_speed), "A speed must be a number");
    Require(std::abs(s.start_speed) <= 100.0 && std::abs(s.end_speed) <= 100.0, "A speed ramp's speed is at most 100 times");
  }
}
void Check(const ClearSpeedRampPayload& payload) { RequireIdentifier(payload.clip_id, "Clip id"); }

void Check(const CreateMulticamGroupPayload& payload) {
  RequireIdentifier(payload.id, "Multicam group id");
  Require(payload.duration.Compare({0, 1}) > 0, "A multicam group must have a length");
  Require(payload.angles.size() >= 2, "A multicam group needs at least two angles");
  Require(payload.angles.size() <= 64, "A multicam group has at most 64 angles");
  std::set<std::string> seen;
  for (const auto& angle : payload.angles) {
    RequireIdentifier(angle.id, "Angle id");
    RequireIdentifier(angle.media_id, "Angle media id");
    Require(seen.insert(angle.id).second, "Two angles share an id");
    if (angle.marker) Require(angle.marker->Compare({0, 1}) >= 0, "An angle's sync marker cannot be negative");
  }
}
void Check(const SetMulticamSyncPayload& payload) {
  RequireIdentifier(payload.group_id, "Multicam group id");
  Require(payload.method == "timecode" || payload.method == "marker" || payload.method == "audio" || payload.method == "manual",
          "A multicam sync method is timecode, marker, audio or manual");
  Require(payload.confidence >= 0.0 && payload.confidence <= 1.0, "A sync confidence is between 0 and 1");
  Require(!payload.offsets.empty(), "A multicam sync needs at least one angle offset");
  std::set<std::string> seen;
  for (const auto& offset : payload.offsets) {
    RequireIdentifier(offset.angle_id, "Angle id");
    Require(seen.insert(offset.angle_id).second, "An angle appears twice in one sync");
  }
}
void Check(const RecordMulticamSwitchPayload& payload) {
  RequireIdentifier(payload.group_id, "Multicam group id");
  RequireIdentifier(payload.angle_id, "Angle id");
  Require(payload.at.Compare({0, 1}) >= 0, "A multicam cut cannot be before the start of the group");
}
void Check(const RemoveMulticamSwitchPayload& payload) { RequireIdentifier(payload.group_id, "Multicam group id"); }
void Check(const RenameMulticamAnglePayload& payload) {
  RequireIdentifier(payload.group_id, "Multicam group id");
  RequireIdentifier(payload.angle_id, "Angle id");
  Require(!payload.name.empty() && payload.name.size() <= 120, "An angle name must be 1 to 120 characters");
}
void Check(const DeleteMulticamGroupPayload& payload) { RequireIdentifier(payload.id, "Multicam group id"); }
void Check(const FlattenMulticamGroupPayload& payload) {
  RequireIdentifier(payload.group_id, "Multicam group id");
  RequireIdentifier(payload.video_track_id, "Video track id");
  RequireIdentifier(payload.id_prefix, "Clip id prefix");
  Require(payload.timeline_start.Compare({0, 1}) >= 0, "A flattened group cannot start before the timeline");
  Require(payload.audio_angle_id.empty() || !payload.audio_track_id.empty(), "An audio angle needs an audio track");
}

void Check(const CreateGraphicPayload& payload) {
  RequireIdentifier(payload.id, "Graphic id");
  Require(payload.kind == "graphic" || payload.kind == "template", "A graphic is a graphic or a template instance");
  if (payload.kind == "graphic") {
    Require(!payload.document_json.empty(), "A graphic needs a document");
    Require(payload.template_id.empty() && payload.values.empty(), "A plain graphic has no template");
    (void)render::graphics::ParseDocument(payload.document_json);
  } else {
    Require(payload.document_json.empty(), "A template instance has no document of its own");
    Require(!payload.template_id.empty() && payload.template_version >= 1, "A template instance names an installed template and version");
  }
}
void Check(const UpdateGraphicPayload& payload) {
  RequireIdentifier(payload.id, "Graphic id");
  Require(payload.name || payload.document_json || payload.values, "A graphic edit changes something");
  Require(!(payload.document_json && payload.values), "A graphic edit changes the document or the values, not both");
  if (payload.document_json) (void)render::graphics::ParseDocument(*payload.document_json);
}
void Check(const DeleteGraphicPayload& payload) { RequireIdentifier(payload.id, "Graphic id"); }
void Check(const InstallGraphicTemplatePayload& payload) {
  Require(!payload.package_json.empty(), "A template package is empty");
  (void)render::graphics::ParseTemplate(payload.package_json);
}
void Check(const RemoveGraphicTemplatePayload& payload) {
  Require(!payload.template_id.empty() && payload.version >= 1, "A template is named by its id and version");
}
void Check(const AddGraphicClipPayload& payload) {
  RequireIdentifier(payload.clip_id, "Clip id");
  RequireIdentifier(payload.track_id, "Track id");
  RequireIdentifier(payload.graphic_id, "Graphic id");
  Require(payload.timeline_start.Compare({0, 1}) >= 0, "A graphic clip cannot start before the timeline");
  Require(payload.duration.Compare({0, 1}) > 0, "A graphic clip must have a length");
}

void Check(const SaveTrackingDataPayload& payload) {
  RequireIdentifier(payload.id, "Tracking data id");
  RequireIdentifier(payload.clip_id, "Clip id");
  Require(payload.kind == "point" || payload.kind == "plane" || payload.kind == "stabilize", "Tracking data kind must be point, plane or stabilize");
  Require(!payload.algorithm.empty(), "Tracking data needs the algorithm that made it");
  // The analysis is JSON, and it must be: a stored record that cannot be read back is worse than none.
  for (const auto* text : {&payload.data_json, &payload.parameters_json}) {
    try {
      const auto value = json::Parse(*text);
      Require(value.is_object(), "Tracking data must be a JSON object");
    } catch (const json::ParseError& error) {
      throw std::invalid_argument(std::string("Tracking data is not valid JSON: ") + error.what());
    }
  }
}

void Check(const DeleteTrackingDataPayload& payload) { RequireIdentifier(payload.id, "Tracking data id"); }

void RequireStyleJson(const std::string& text) {
  try {
    const auto value = json::Parse(text);
    Require(value.is_object(), "A caption style must be a JSON object");
  } catch (const json::ParseError& error) {
    throw std::invalid_argument(std::string("A caption style is not valid JSON: ") + error.what());
  }
}

void CheckCue(const CaptionCuePayload& cue) {
  RequireIdentifier(cue.id, "Caption id");
  Require(cue.end.Compare(cue.start) > 0, "A caption must end after it starts");
  Require(cue.start.Compare({0, 1}) >= 0, "A caption cannot start before the sequence");
  Require(!cue.text.empty(), "A caption needs text");
  Require(cue.text.size() <= 8192, "A caption's text is longer than 8192 bytes");
  RequireStyleJson(cue.style_json);
}

void Check(const AddCaptionTrackPayload& payload) {
  RequireIdentifier(payload.id, "Caption track id");
  RequireIdentifier(payload.sequence_id, "Sequence id");
  Require(payload.language.size() <= 35, "A caption language is a BCP 47 tag of at most 35 characters");
  RequireStyleJson(payload.style_json);
}
void Check(const RemoveCaptionTrackPayload& payload) { RequireIdentifier(payload.id, "Caption track id"); }
void Check(const UpdateCaptionTrackPayload& payload) {
  RequireIdentifier(payload.id, "Caption track id");
  Require(payload.language.size() <= 35, "A caption language is a BCP 47 tag of at most 35 characters");
  RequireStyleJson(payload.style_json);
}
void Check(const AddCaptionsPayload& payload) {
  RequireIdentifier(payload.track_id, "Caption track id");
  Require(!payload.cues.empty(), "There are no captions to add");
  Require(payload.cues.size() <= 200000, "Too many captions in one edit");
  for (const auto& cue : payload.cues) CheckCue(cue);
}
void Check(const UpdateCaptionPayload& payload) { CheckCue(payload.cue); }
void Check(const RemoveCaptionsPayload& payload) {
  Require(!payload.ids.empty(), "There are no captions to remove");
  for (const auto& id : payload.ids) RequireIdentifier(id, "Caption id");
}

void Check(const InsertClipPayload& payload) {
  RequireIdentifier(payload.id, "Clip id");
  RequireIdentifier(payload.track_id, "Track id");
  Require(payload.source_out.Compare(payload.source_in) > 0, "Clip source range must be positive");
  Require(payload.playback_rate.Compare({0, 1}) > 0, "Clip playback rate must be positive");
  Require(payload.timeline_start.Compare({0, 1}) >= 0, "Clip cannot start before the sequence");
  // The source reference has to agree with the source kind, or the database
  // CHECK would reject it later with a far less helpful message.
  switch (payload.source_kind) {
    case model::SourceKind::Media:
      Require(payload.media_id.has_value() && !payload.nested_sequence_id.has_value(),
              "A media clip must reference exactly one media item");
      break;
    case model::SourceKind::Sequence:
      Require(payload.nested_sequence_id.has_value() && !payload.media_id.has_value(),
              "A nested clip must reference exactly one sequence");
      break;
    case model::SourceKind::Adjustment:
      Require(!payload.media_id.has_value() && !payload.nested_sequence_id.has_value(),
              "An adjustment clip must not reference a source");
      break;
  }
}

void Check(const DeleteClipPayload& payload) { RequireIdentifier(payload.id, "Clip id"); }
void Check(const RippleDeleteClipPayload& payload) { RequireIdentifier(payload.id, "Clip id"); }

void Check(const MoveClipPayload& payload) {
  RequireIdentifier(payload.id, "Clip id");
  RequireIdentifier(payload.track_id, "Track id");
  Require(payload.timeline_start.Compare({0, 1}) >= 0, "Clip cannot start before the sequence");
}

void Check(const SplitClipPayload& payload) {
  RequireIdentifier(payload.id, "Clip id");
  RequireIdentifier(payload.new_clip_id, "New clip id");
  Require(payload.id != payload.new_clip_id, "Split requires a new clip id distinct from the original");
}

void Check(const TrimClipPayload& payload) {
  RequireIdentifier(payload.id, "Clip id");
  Require(payload.source_out.Compare(payload.source_in) > 0, "Trimmed source range must be positive");
  Require(payload.timeline_start.Compare({0, 1}) >= 0, "Clip cannot start before the sequence");
}

void Check(const SetClipEnabledPayload& payload) { RequireIdentifier(payload.id, "Clip id"); }

void Check(const SetClipAudioRolePayload& payload) {
  RequireIdentifier(payload.id, "Clip id");
  Require(payload.role.empty() || payload.role == "dialogue" || payload.role == "music" || payload.role == "effects" || payload.role == "ambience",
          "An audio role is dialogue, music, effects, ambience or empty");
}

void Check(const SetClipSpeedPayload& payload) {
  RequireIdentifier(payload.id, "Clip id");
  Require(payload.playback_rate.Compare({0, 1}) > 0, "Clip playback rate must be positive");
}

void Check(const LinkClipsPayload& payload) {
  RequireIdentifier(payload.group_id, "Link group id");
  Require(payload.clip_ids.size() >= 2, "Linking needs at least two clips");
  for (const auto& id : payload.clip_ids) RequireIdentifier(id, "Clip id");
}

void Check(const UnlinkClipsPayload& payload) {
  Require(!payload.clip_ids.empty(), "Unlinking needs at least one clip");
  for (const auto& id : payload.clip_ids) RequireIdentifier(id, "Clip id");
}

void Check(const AddTransitionPayload& payload) {
  RequireIdentifier(payload.id, "Transition id");
  RequireIdentifier(payload.track_id, "Track id");
  RequireIdentifier(payload.kind, "Transition kind");
  Require(payload.duration.Compare({0, 1}) > 0, "Transition duration must be positive");
  Require(payload.timeline_start.Compare({0, 1}) >= 0, "Transition cannot start before the sequence");
  Require(payload.from_clip_id.has_value() || payload.to_clip_id.has_value(),
          "A transition must attach to at least one clip");
  Require(!payload.from_clip_id.has_value() || !payload.to_clip_id.has_value() ||
              *payload.from_clip_id != *payload.to_clip_id,
          "A transition cannot join a clip to itself");
}

void Check(const RemoveTransitionPayload& payload) { RequireIdentifier(payload.id, "Transition id"); }

void Check(const SetTransitionTimingPayload& payload) {
  RequireIdentifier(payload.id, "Transition id");
  Require(payload.duration.Compare({0, 1}) > 0, "Transition duration must be positive");
  Require(payload.timeline_start.Compare({0, 1}) >= 0, "Transition cannot start before the sequence");
}

void Check(const EffectParameter& parameter) {
  RequireIdentifier(parameter.id, "Parameter id");
  RequireIdentifier(parameter.name, "Parameter name");
  Require(parameter.value.dimension >= 1 && parameter.value.dimension <= 4,
          "Parameter dimension must be between 1 and 4");
}

void Check(const AddEffectPayload& payload) {
  RequireIdentifier(payload.id, "Effect id");
  RequireIdentifier(payload.owner_id, "Effect owner id");
  RequireIdentifier(payload.effect_type, "Effect type");
  Require(payload.order >= 0, "Effect order must not be negative");
  const auto asset_error = effects::ValidateAssetReference(payload.effect_type, payload.preset_name);
  if (!asset_error.empty()) throw std::invalid_argument(asset_error);
  for (const auto& parameter : payload.parameters) {
    Check(parameter);
    const auto parameter_error = effects::ValidateParameter(payload.effect_type, parameter.name, parameter.value);
    if (!parameter_error.empty()) throw std::invalid_argument(parameter_error);
  }
  for (std::size_t outer = 0; outer < payload.parameters.size(); ++outer) {
    for (std::size_t inner = outer + 1; inner < payload.parameters.size(); ++inner) {
      Require(payload.parameters[outer].name != payload.parameters[inner].name,
              "Two parameters on one effect share the same name");
      Require(payload.parameters[outer].id != payload.parameters[inner].id,
              "Two parameters on one effect share the same id");
    }
  }
}

void Check(const RemoveEffectPayload& payload) { RequireIdentifier(payload.id, "Effect id"); }
void Check(const SetEffectEnabledPayload& payload) { RequireIdentifier(payload.id, "Effect id"); }

void Check(const ReorderEffectPayload& payload) {
  RequireIdentifier(payload.id, "Effect id");
  Require(payload.order >= 0, "Effect order must not be negative");
}

void Check(const AddMaskPayload& payload) {
  RequireIdentifier(payload.id, "Mask id");
  RequireIdentifier(payload.effect_id, "Mask effect id");
  Require(payload.order >= 0, "Mask order must not be negative");
  (void)effects::mask::Parse(payload.document_json);
}

void Check(const UpdateMaskPayload& payload) {
  RequireIdentifier(payload.id, "Mask id");
  (void)effects::mask::Parse(payload.document_json);
}

void Check(const RemoveMaskPayload& payload) { RequireIdentifier(payload.id, "Mask id"); }

void Check(const SetParameterConstantPayload& payload) {
  RequireIdentifier(payload.parameter_id, "Parameter id");
  Require(payload.value.dimension >= 1 && payload.value.dimension <= 4,
          "Parameter dimension must be between 1 and 4");
}

void Check(const SetKeyframePayload& payload) {
  RequireIdentifier(payload.parameter_id, "Parameter id");
  Require(payload.keyframe.value.dimension >= 1 && payload.keyframe.value.dimension <= 4,
          "Keyframe dimension must be between 1 and 4");
}

void Check(const RemoveKeyframePayload& payload) { RequireIdentifier(payload.parameter_id, "Parameter id"); }

void Check(const AddMarkerPayload& payload) {
  RequireIdentifier(payload.id, "Marker id");
  RequireIdentifier(payload.owner_id, "Marker owner id");
  Require(payload.end.Compare(payload.start) >= 0, "Marker range must not be negative");
  Require(!payload.metadata_json.empty(), "Marker metadata must be a JSON document");
}

void Check(const RemoveMarkerPayload& payload) { RequireIdentifier(payload.id, "Marker id"); }

void Check(const UpdateMarkerPayload& payload) {
  RequireIdentifier(payload.id, "Marker id");
  Require(payload.end.Compare(payload.start) >= 0, "Marker range must not be negative");
  Require(!payload.metadata_json.empty(), "Marker metadata must be a JSON document");
}

}  // namespace

std::string ToString(CommandType type) { return std::string(Find(type).name); }

CommandType ParseCommandType(const std::string& name) {
  for (const auto& descriptor : Descriptors()) {
    if (descriptor.name == name) return descriptor.type;
  }
  throw std::invalid_argument("Unknown command type: " + name);
}

std::string Label(const CommandEnvelope& command) { return std::string(Find(command.type).label); }

bool TypeMatchesPayload(const CommandEnvelope& command) {
  return command.payload.index() == Find(command.type).payload_index;
}

std::string PayloadJson(const CommandPayload& payload) {
  return std::visit([](const auto& value) { return ToJson(value); }, payload);
}

void Validate(const CommandEnvelope& command) {
  RequireIdentifier(command.command_id, "Command id");
  RequireIdentifier(command.author_id, "Author id");
  RequireIdentifier(command.idempotency_key, "Idempotency key");
  Require(!command.timestamp_utc.empty(), "Command timestamp must not be empty");
  Require(command.base_revision >= 0, "Base revision must not be negative");
  // CreateProject is the one command that establishes project identity, so it is
  // also the only one allowed to arrive without one.
  if (command.type != CommandType::CreateProject) {
    RequireIdentifier(command.project_id, "Project id");
  }
  if (!TypeMatchesPayload(command)) {
    throw std::invalid_argument("Command " + ToString(command.type) + " carries the wrong payload type");
  }
  std::visit([](const auto& value) { Check(value); }, command.payload);
}

}  // namespace cutline::commands
