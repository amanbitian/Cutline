# Cutline parity report

Generated: 2026-10-08T18:07:33.516Z

No overall marketing percentage is reported; domains are intentionally unweighted.

| Domain | Tracked | Validated | Implemented | Partial | Prototype | In progress | Planned | Deferred |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| accessibility | 7 | 0 | 1 | 0 | 0 | 0 | 6 | 0 |
| ai | 9 | 0 | 0 | 2 | 1 | 0 | 0 | 6 |
| audio | 10 | 0 | 7 | 2 | 0 | 0 | 1 | 0 |
| camera-formats | 8 | 0 | 0 | 1 | 0 | 0 | 7 | 0 |
| captions | 8 | 0 | 5 | 1 | 0 | 0 | 2 | 0 |
| codecs | 8 | 0 | 5 | 2 | 0 | 0 | 1 | 0 |
| collaboration | 7 | 0 | 0 | 0 | 0 | 0 | 7 | 0 |
| color | 11 | 0 | 2 | 8 | 0 | 0 | 1 | 0 |
| editorial | 17 | 0 | 4 | 1 | 0 | 0 | 12 | 0 |
| effects | 16 | 0 | 14 | 1 | 0 | 0 | 1 | 0 |
| export | 8 | 0 | 6 | 0 | 0 | 0 | 2 | 0 |
| graphics | 8 | 0 | 5 | 1 | 0 | 0 | 2 | 0 |
| hardware | 12 | 0 | 1 | 5 | 0 | 0 | 2 | 4 |
| interchange | 10 | 0 | 4 | 1 | 0 | 0 | 5 | 0 |
| media | 14 | 0 | 7 | 2 | 3 | 0 | 2 | 0 |
| monitors | 7 | 0 | 3 | 2 | 0 | 0 | 2 | 0 |
| performance | 9 | 0 | 2 | 4 | 0 | 0 | 3 | 0 |
| plugins | 7 | 0 | 0 | 0 | 0 | 0 | 0 | 7 |
| review | 6 | 0 | 0 | 0 | 0 | 0 | 6 | 0 |
| timeline | 13 | 0 | 13 | 0 | 0 | 0 | 0 | 0 |
| transitions | 7 | 0 | 4 | 2 | 0 | 0 | 1 | 0 |
| trimming | 13 | 0 | 11 | 1 | 0 | 0 | 1 | 0 |
| vr | 5 | 0 | 0 | 0 | 0 | 0 | 5 | 0 |

## Capability inventory

### accessibility

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| caption_accessibility | Caption accessibility | P2 | planned | Provides authoring checks for reading speed, overlap, and safe area. |
| color_accessibility | Color accessibility | P1 | planned | Critical state is not communicated by color alone. |
| high_contrast | High contrast | P1 | planned | UI remains usable under supported system contrast settings. |
| keyboard_navigation | Keyboard navigation | P1 | planned | All primary editorial actions have discoverable keyboard operation. |
| screen_reader_semantics | Screen reader semantics | P1 | planned | Native UI exposes meaningful names, roles, values, and state. |
| shortcut_customization | Shortcut customization | P2 | implemented | Allows conflict-checked remapping of supported shortcuts. |
| ui_scaling | UI scaling | P1 | planned | UI supports documented DPI/text scale ranges without obscuring controls. |

### ai

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| auto_reframe | Auto reframe | P2 | deferred | Creates reviewable tracked framing commands, not hidden destructive edits. |
| generative_extend | Generative extend | P3 | deferred | Records provider, input, output, consent, and reversible insertion provenance. |
| llm_edit_control | LLM editing control | P1 | prototype | Converts constrained authorized requests to validated project commands with audit trail. |
| object_selection | Object selection | P2 | deferred | Stores masks with model/version, prompt/input, confidence, and review status. |
| scene_edit_detection | Scene edit detection | P2 | deferred | Creates reviewable cut candidates with source evidence. |
| semantic_search | Semantic search | P2 | deferred | Indexes approved embeddings with model/version and invalidation provenance. |
| speech_enhancement | Speech enhancement | P2 | deferred | Creates a reversible effect configuration with model provenance. |
| text_based_editing | Text-based editing | P2 | partial | Turns reviewed transcript selections into normal validated edit commands. |
| transcription | Transcription | P2 | partial | Stores timed transcript with provider/model, language, confidence, and review state. |

### audio

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| audio_buffering | Audio buffering | P0 | implemented | Meets bounded buffering and underrun diagnostics policy. |
| audio_decode | Audio decode | P0 | implemented | Decodes supported audio streams into float processing buffers. |
| audio_device_output | Audio device output | P0 | implemented | Streams audio to an OS device without callback allocation or I/O. |
| audio_effects | Built-in audio effects | P1 | partial | Runs declared effects in a bounded real-time audio graph. |
| audio_keyframes | Audio keyframes | P1 | implemented | Applies editable gain/pan automation at exact sample positions. |
| audio_master_clock | Audio master clock | P0 | implemented | Video presentation derives from actual audio-device sample progress. |
| audio_mixer | Audio mixer | P1 | implemented | Mixes track gain, pan, mute, solo, and routing sample-accurately. |
| audio_sync_alignment | Audio synchronization | P1 | partial | Produces reviewable alignment results with confidence/provenance. |
| loudness_metering | Loudness metering | P1 | implemented | Reports standards-defined peak and loudness measurements. |
| waveform_generation | Waveform generation | P1 | planned | Builds cached waveform summaries from decoded audio. |

### camera-formats

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| arri_format_support | ARRI format support | P2 | planned | Handles approved ARRI media through a licensed/proven decoder path. |
| braw_support | Blackmagic RAW support | P2 | planned | Uses the appropriate SDK path and preserves decode controls. |
| camera_format_capability_registry | Camera format capability registry | P1 | planned | Reports decode, metadata, proxy, and license capability per camera format. |
| canon_raw_support | Canon RAW support | P2 | planned | Handles approved Canon RAW media with documented limits. |
| prores_support | ProRes support | P1 | partial | Probes, decodes, and exports only supported/legal ProRes profiles. |
| raw_metadata_controls | RAW metadata controls | P2 | planned | Makes source decode controls explicit and non-destructive. |
| red_format_support | RED format support | P2 | planned | Handles approved RED media through an authorized decoder path. |
| sony_xavc_support | Sony XAVC support | P2 | planned | Probes and decodes supported XAVC variants with correct metadata. |

### captions

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| auto_caption | Automatic captions | P2 | partial | Stores model/provider provenance and requires human review. |
| caption_accessibility_qc | Caption accessibility QC | P2 | planned | Reports timing, length, and overlap violations deterministically. |
| caption_editing | Caption editing | P1 | implemented | Edits text and timing through commands with range validation. |
| caption_export | Caption export | P1 | implemented | Exports selected caption standards with timing verification. |
| caption_import | Caption import | P1 | implemented | Imports supported timed text while retaining parse diagnostics. |
| caption_styles | Caption styles | P1 | implemented | Applies reusable styles in preview and supported exports. |
| caption_track | Caption track | P1 | implemented | Stores timed caption cues as durable sequence entities. |
| caption_translation | Caption translation | P2 | planned | Produces reviewable translated cues without overwriting originals. |

### codecs

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| codec_license_reporting | Codec license reporting | P1 | planned | Build/runtime reports enabled codec provenance and license constraints. |
| common_audio_decode | Common audio decode | P0 | implemented | Decodes common audio formats into the audio engine format. |
| ffmpeg_media_abstraction | Production media abstraction | P0 | implemented | Provides legal/configured codec capability discovery and structured errors. |
| h264_decode | H.264 decode | P0 | implemented | Software decoder produces timestamped frames from supported H.264 media. |
| hevc_decode | HEVC decode | P0 | partial | Decodes available/licensed HEVC profiles or reports explicit support limits. |
| mp4_mov_demux | MP4/MOV demux | P0 | implemented | Extracts streams, timestamps, metadata, and packet data from MP4/MOV. |
| still_image_decode | Still image decode | P0 | partial | Loads supported still images with orientation and color metadata. |
| vfr_timestamp_map | VFR timestamp map | P0 | implemented | Maps decoded VFR frame timestamps without nominal frame-duration assumptions. |

### collaboration

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| bin_locks | Bin locks | P3 | planned | Coordinates exclusive operations while permitting safe reads. |
| conflict_resolution | Conflict resolution | P3 | planned | Reports and resolves concurrent edit conflicts without silent data loss. |
| production_history | Production history | P3 | planned | Records attributable collaborative command history and restorability. |
| project_permissions | Project permissions | P3 | planned | Authorizes read/edit/review/export actions by authenticated role. |
| sequence_locks | Sequence locks | P3 | planned | Protects conflict-prone sequence edits with explicit lock lifecycle. |
| shared_media_manifest | Shared media manifest | P3 | planned | Shares media identity/alias metadata without assuming identical paths. |
| shared_project_state | Shared project state | P3 | planned | Synchronizes command envelopes with conflict and revision rules. |

### color

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| basic_color_correction | Basic color correction | P1 | implemented | Applies correction values in a color-managed native render pipeline. |
| color_managed_pipeline | Color-managed pipeline | P1 | partial | Tracks input, working, display, and export transforms explicitly. |
| color_match | Color match | P2 | planned | Produces reviewable match parameters rather than destructive pixel changes. |
| color_scopes | Color scopes | P1 | implemented | Computes waveform, vectorscope, histogram, and parade from actual frames. |
| color_wheels | Color wheels | P1 | partial | Applies lift/gamma/gain style controls with documented math. |
| hdr_hlg | HDR HLG | P2 | partial | Maintains HLG transfer metadata and validates preview/export transform. |
| hdr_pq | HDR PQ | P2 | partial | Maintains PQ transfer metadata and validates preview/export transform. |
| look_presets | Look presets | P1 | partial | Stores non-destructive versioned grade presets. |
| lumetri_style_curves | Curves | P1 | partial | Evaluates editable channel and luma curves in the grade graph. |
| lut_management | LUT management | P1 | partial | Imports, validates, applies, and records LUT provenance. |
| wide_gamut | Wide-gamut workflows | P2 | partial | Preserves declared color primaries through decode, render, and export. |

### editorial

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| four_point_edit | Four-point edit | P1 | planned | Applies an explicit fit policy when source and target durations differ. |
| frame_step | Frame step | P0 | implemented | Steps exact sequence frames at the sequence timebase. |
| master_clip_properties | Master clip properties | P1 | planned | Edits supported media interpretation settings through commands. |
| match_frame | Match frame | P1 | planned | Finds the source frame used by the selected sequence clip. |
| program_monitor | Program Monitor | P0 | partial | Presents compiled sequence frames without browser-video authority. |
| replace_edit | Replace edit | P1 | planned | Replaces a target clip while preserving declared timing and links. |
| reverse_match_frame | Reverse match frame | P1 | planned | Finds each sequence use of the current source frame. |
| sequence_start_end | Sequence start/end navigation | P0 | implemented | Moves playhead to deterministic sequence boundaries. |
| source_clear_marks | Clear source marks | P0 | planned | Clears source in/out without changing media metadata. |
| source_mark_in | Source mark in | P0 | planned | Sets an exact RationalTime source in point. |
| source_mark_out | Source mark out | P0 | planned | Sets an exact RationalTime source out point. |
| source_monitor | Source Monitor | P0 | planned | Loads a selected source and presents decoded source frames. |
| source_patching | Source patching | P0 | planned | Maps source AV components to explicitly selected destination tracks. |
| subclip_creation | Subclip creation | P1 | planned | Creates a durable source range reference that survives reopen. |
| three_point_edit | Three-point edit | P0 | implemented | Resolves source and sequence marks into a valid insert or overwrite command. |
| track_targeting | Track targeting | P0 | planned | Restricts edit commands to intended sequence tracks. |
| transport_jkl | J/K/L transport | P0 | implemented | Shortcuts change the real transport state, including reverse playback. |

### effects

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| blur_effect | Blur | P1 | implemented | Applies a keyframeable, bounded blur without per-frame allocations. |
| crop_effect | Crop effect | P1 | implemented | Applies pixel-accurate crop including keyframed bounds. |
| cube_lut | 3D .cube LUT | P1 | implemented | Loads a project-relative .cube asset, interpolates it in 3D, and blends by intensity. |
| effect_bypass | Effect bypass | P1 | implemented | Bypasses an effect without discarding parameters or keyframes. |
| effect_graph | Effect graph | P1 | partial | Compiles ordered effect nodes into an immutable render request. |
| effect_presets | Effect presets | P1 | planned | Stores versioned reusable parameter sets. |
| lens_correction | Lens and wide-angle correction | P1 | implemented | Corrects radial distortion with centre, scale, and quadratic controls. |
| mask_tracking | Mask tracking | P2 | implemented | Stores reviewable tracking data with source and algorithm provenance. |
| masks | Masks | P2 | implemented | Renders serialized geometric masks with feather and expansion. |
| motion_keyframes | Motion keyframes | P1 | implemented | Evaluates supported interpolation deterministically at render time. |
| opacity_effect | Opacity effect | P1 | implemented | Applies alpha consistently in preview and export. |
| sharpen_effect | Sharpen | P1 | implemented | Raises local contrast while preserving alpha. |
| stabilization | Stabilization | P2 | implemented | Produces a reversible analysis result and deterministic transform application. |
| time_remapping | Time remapping | P2 | implemented | Maps output time to source time through explicit rational curves. |
| transform_effect | Transform effect | P1 | implemented | Applies position, scale, rotation, and anchor in composited output. |
| vignette_effect | Vignette | P1 | implemented | Provides keyframeable amount, midpoint, feather, and centre controls. |

### export

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| export_presets | Export presets | P1 | implemented | Stores versioned preset settings and resolves inheritance explicitly. |
| export_progress | Export progress | P0 | implemented | Reports phase, frames, time, error, and cancellation state. |
| export_queue | Export queue | P0 | implemented | Queues persistent, resumable jobs with frozen job settings. |
| export_validation | Export validation | P1 | implemented | Re-probes outputs and reports format/timing mismatches. |
| media_export | Media export | P1 | implemented | Transcodes selected source or timeline output with diagnostics. |
| render_and_replace | Render and Replace | P1 | planned | Creates a reversible rendered-media substitution with provenance. |
| smart_render | Smart render | P2 | planned | Reuses eligible encoded segments only when codec and GOP constraints permit. |
| timeline_export | Timeline export | P0 | implemented | Renders an exact selected sequence range to a validated output. |

### graphics

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| essential_graphics_templates | Reusable graphic templates | P2 | implemented | Packages exposed parameters, assets, and version metadata. |
| font_management | Font management | P1 | partial | Records required fonts and reports substitution in project and export. |
| graphics_keyframes | Graphics keyframes | P1 | implemented | Animates graphic properties through the shared keyframe model. |
| lower_thirds | Lower-third templates | P1 | implemented | Creates reusable lower-third templates with editable fields. |
| responsive_design_time | Responsive design time | P2 | planned | Retimes declared graphic regions without breaking protected segments. |
| shape_graphics | Shape graphics | P1 | implemented | Renders editable vector shapes in the composition pipeline. |
| svg_import | SVG import | P2 | planned | Imports a documented safe SVG subset with diagnostics. |
| text_graphics | Text graphics | P1 | implemented | Renders editable styled text with deterministic font fallback. |

### hardware

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| amf_provider | AMF provider | P2 | deferred | Adds AMD decode/encode only through the shared provider contract. |
| audio_device_recovery | Audio device recovery | P1 | partial | Recovers or stops safely after audio-device format/disconnect changes. |
| device_loss_recovery | Device loss recovery | P1 | planned | Recovers UI/project state and reports failed work after device reset. |
| gpu_compositor_d3d11 | GPU compositor (Direct3D 11) | P1 | partial | Renders the common editing subset of a plan on the GPU to within a level of the software compositor and hands every other frame back to it. |
| hardware_capability_discovery | Hardware capability discovery | P1 | partial | Reports GPU, codec, audio, and pro-I/O capabilities with driver evidence. |
| hardware_decode_d3d11va | Hardware decode into GPU textures (D3D11VA) | P1 | partial | Decodes supported H.264-class streams on the GPU and composes the pictures without copying them to memory; unsupported streams fall back to software decode. |
| multi_gpu_scheduling | Multi-GPU scheduling | P3 | deferred | Assigns workloads with measured transfer and memory constraints. |
| nvdec_provider | NVDEC provider | P2 | deferred | Adds NVIDIA decode only through the shared provider contract. |
| professional_video_io | Professional video I/O | P3 | planned | Outputs supported monitoring signals with device-state diagnostics. |
| qsv_provider | QSV provider | P2 | deferred | Adds Intel media acceleration only through the shared provider contract. |
| render_backend_abstraction | Render backend abstraction | P1 | partial | Selects a supported graphics backend with a CPU-safe diagnostic fallback. |
| software_decode_provider | Software decode provider | P0 | implemented | Provides a reliable baseline decoder independent of vendor hardware. |

### interchange

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| aaf_export | AAF export | P2 | planned | Exports a tested AAF subset with a loss report. |
| aaf_import | AAF import | P2 | planned | Imports a tested AAF subset with a loss report. |
| edl_export | EDL export | P2 | implemented | Exports conformable EDL with explicit track and effect limitations. |
| edl_import | EDL import | P2 | implemented | Imports EDL events with reel/timecode interpretation reporting. |
| fcpxml_export | FCPXML export | P2 | planned | Exports a documented FCPXML mapping with diagnostics. |
| fcpxml_import | FCPXML import | P2 | planned | Imports a documented FCPXML mapping with diagnostics. |
| omf_audio_export | OMF audio export | P2 | planned | Exports compatible audio media and edit decisions with a validation report. |
| project_archive | Project archive | P1 | partial | Packages project metadata, media manifest, and checksums without copying cache by default. |
| xml_export | XML export | P2 | implemented | Exports a documented XML mapping with unsupported-feature diagnostics. |
| xml_import | XML import | P2 | implemented | Imports documented XML subset with a loss report. |

### media

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| checksum_ingest | Checksum ingest verification | P1 | implemented | Reports checksum verification result without blocking editing. |
| copy_link_ingest | Copy/link ingest | P1 | implemented | Records whether media was copied, linked, and verified. |
| duplicate_detection | Duplicate detection | P0 | implemented | Flags likely duplicate imports without treating filenames as identity. |
| image_sequence_detection | Image sequence detection | P1 | planned | Recognizes numbered still sequences and preserves missing-frame evidence. |
| media_bins | Media bins | P0 | implemented | Creates ordered nested bins through project commands. |
| media_fingerprint | Versioned media fingerprint | P0 | implemented | Uses versioned sampled/content evidence plus stream metadata, size, and duration. |
| media_labels | Media labels | P1 | prototype | Persists label assignment as auditable metadata. |
| media_metadata | Media metadata | P1 | prototype | Stores queryable user and technical metadata with field provenance. |
| media_probe | Media probe | P0 | implemented | Persists real container, stream, timing, color, audio, and timecode metadata. |
| media_search | Media search | P1 | prototype | Searches persisted metadata and reports matching media identities. |
| path_aliases | Media path aliases | P0 | planned | Stores multiple resolvable locations per media identity. |
| probe_diagnostics | Probe diagnostics | P0 | partial | Returns actionable errors for unsupported or damaged media. |
| proxy_attach | Proxy attachment | P1 | implemented | Associates verified proxy media to a full-resolution identity. |
| relink_media | Relink media | P0 | partial | Relinks a missing alias after identity verification. |

### monitors

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| fullscreen_playback | Fullscreen playback | P1 | partial | Presents program output in a dedicated fullscreen surface. |
| monitor_overlays | Monitor overlays | P1 | partial | Shows timecode, dropped-frame, and playback diagnostics accurately. |
| monitor_safe_margins | Safe margins | P1 | implemented | Displays title/action safe overlays independent of source pixels. |
| monitor_zoom | Monitor zoom | P1 | implemented | Changes presentation scale without altering render resolution. |
| program_playback | Program playback | P0 | implemented | Presents compiled sequence output at the transport clock. |
| reference_monitor | Reference monitor | P2 | planned | Displays a selected still/frame for visual comparison. |
| source_playback | Source playback | P0 | planned | Plays, pauses, seeks, and frame-steps decoded source media. |

### performance

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| audio_callback_budget | Audio callback budget | P0 | partial | Verifies audio callback avoids allocation, I/O, and unbounded locks. |
| background_job_scheduler | Background job scheduler | P1 | partial | Schedules bounded background jobs with cancellation, retry, and durable state. |
| crash_recovery | Crash recovery | P0 | partial | Reopens the last valid state after interrupted write with user-readable recovery report. |
| media_cache | Media cache | P1 | planned | Caches conformed/indexed media with versioned invalidation and eviction. |
| memory_pressure_handling | Memory pressure handling | P1 | partial | Evicts reclaimable caches and degrades gracefully under declared thresholds. |
| playback_frame_budget | Playback frame budget | P1 | planned | Measures decode, render, queue, and present latency per playback frame. |
| proxy_generation | Proxy generation | P1 | implemented | Creates tracked proxy jobs without blocking interactive playback. |
| render_cache | Render cache | P1 | implemented | Caches render results with complete dependency invalidation. |
| telemetry_diagnostics | Diagnostics | P1 | planned | Captures opt-in local diagnostics with redaction and export controls. |

### plugins

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| ofx_hosting | OFX hosting | P2 | deferred | Hosts approved OFX effects out of process or in controlled isolation. |
| plugin_capability_registry | Plugin capability registry | P2 | deferred | Lists supported plugin formats, ABI versions, permissions, and isolation level. |
| plugin_crash_recovery | Plugin crash recovery | P2 | deferred | Recovers project state when a plugin process fails. |
| plugin_parameter_automation | Plugin parameter automation | P2 | deferred | Persists validated automation independent of transient plugin UI. |
| plugin_render_cache | Plugin render cache | P2 | deferred | Invalidates cached output by plugin/version/parameter/media dependency. |
| plugin_scan | Plugin scan | P2 | deferred | Scans plugins in an isolated worker and records failures safely. |
| vst3_hosting | VST3 hosting | P2 | deferred | Hosts approved VST3 audio plugins within real-time safety limits. |

### review

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| drawing_annotations | Drawing annotations | P3 | planned | Stores vector annotations separately from source media. |
| frame_comments | Frame comments | P3 | planned | Anchors comments to sequence time/frame and asset revision. |
| review_approvals | Review approvals | P3 | planned | Records approver identity, target version, decision, and timestamp. |
| review_link | Review link | P3 | planned | Creates a revocable scoped link to a versioned review asset. |
| review_notification | Review notifications | P3 | planned | Delivers configured notifications without embedding secrets in the project. |
| review_version_compare | Review version compare | P3 | planned | Compares declared output versions with source/provenance identifiers. |

### timeline

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| adjustment_layers | Adjustment layers | P1 | implemented | An adjustment layer applies its graph to eligible underlying clips. |
| audio_tracks | Audio tracks | P0 | implemented | Creates ordered, mute/solo capable audio tracks through commands. |
| clip_records | Timeline clip records | P0 | implemented | Stores source range, timeline position, rate, and link group exactly. |
| linked_av_groups | Linked A/V groups | P0 | implemented | Linked AV edits preserve sync unless explicitly unlinked. |
| markers | Sequence markers | P0 | implemented | Creates, removes, persists, and seeks marker ranges exactly. |
| mixed_rate_sampling | Mixed source frame rates | P0 | implemented | Maps source timestamps to sequence time without nominal-frame assumptions. |
| playhead | Playhead | P0 | implemented | Represents authoritative sequence RationalTime. |
| sequence_nesting | Sequence nesting | P1 | implemented | Sequence clips resolve recursively with cycle detection. |
| sequence_records | Sequence records | P0 | implemented | Stores name, frame rate, geometry, and sample rate transactionally. |
| timeline_compiler | Timeline compiler | P0 | implemented | Produces immutable active-clip playback requests for a sequence time. |
| timeline_ruler | Timeline ruler | P0 | implemented | Displays exact sequence timecode at current display scale. |
| track_lock_mute_solo | Track lock/mute/solo | P1 | implemented | Compiler and commands honor lock, mute, and solo state. |
| video_tracks | Video tracks | P0 | implemented | Creates ordered, lockable video tracks through commands. |

### transitions

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| audio_crossfade | Audio crossfade | P1 | implemented | Applies gain envelopes that avoid discontinuities. |
| cross_dissolve | Cross dissolve | P1 | implemented | Blends eligible adjacent clip frames across exact transition duration. |
| custom_transition_shaders | Custom transition shaders | P2 | planned | Loads validated portable shader transitions through a controlled interface. |
| dip_to_color | Dip to color | P1 | partial | Composites outgoing/incoming frames through a configurable color. |
| transition_alignment | Transition alignment | P1 | implemented | Supports center/start/end alignment with handle validation. |
| transition_handle_diagnostics | Transition handle diagnostics | P1 | partial | Explains insufficient media handles before applying a transition. |
| transition_model | Transition model | P1 | implemented | Persists transition type, alignment, duration, and parameters. |

### trimming

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| clip_selection | Clip selection | P0 | implemented | Selection maps to durable clip identities and command targets. |
| extract | Extract | P0 | implemented | Removes a marked range and ripples applicable tracks. |
| lift | Lift | P0 | implemented | Removes selected material without shifting unselected material. |
| rate_stretch | Rate stretch | P1 | planned | Changes clip duration with an explicit rational playback-rate transform. |
| ripple_delete | Ripple delete | P0 | implemented | Deletes selection and closes the intended gaps transactionally. |
| ripple_trim | Ripple trim | P0 | implemented | Adjusts adjacent downstream clips according to track ripple policy. |
| roll_trim | Roll trim | P1 | implemented | Moves a shared edit point while preserving combined duration. |
| slide_edit | Slide edit | P1 | implemented | Moves a clip while counter-trimming eligible adjacent clips. |
| slip_edit | Slip edit | P1 | implemented | Moves source range while keeping clip timeline placement unchanged. |
| snapping | Snapping | P0 | implemented | Snap candidates are resolved exactly at sequence time. |
| trim_end | Trim end | P0 | implemented | Changes an editable clip end while preserving valid source range. |
| trim_mode_feedback | Trim mode feedback | P1 | partial | UI reports active trim mode and affected edit sides accurately. |
| trim_start | Trim start | P0 | implemented | Changes an editable clip start while preserving valid source range. |

### vr

| ID | Capability | Priority | Status | Acceptance |
|---|---|---|---|---|
| vr_export | VR export | P3 | planned | Writes output with validated immersive projection metadata. |
| vr_monitor | VR monitor | P3 | planned | Displays equirectangular media with interactive view orientation. |
| vr_projection_metadata | VR projection metadata | P3 | planned | Reads and persists supported immersive projection metadata. |
| vr_reframe | VR reframe | P3 | planned | Applies keyframed view transforms non-destructively. |
| vr_spatial_audio | VR spatial audio | P3 | planned | Preserves/exports declared spatial audio metadata where supported. |

