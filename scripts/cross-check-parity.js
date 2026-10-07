#!/usr/bin/env node
// Cross-checks the parity manifests against the code and the test suite.
//
// The manifests were hand-maintained and had no link to anything executable, so
// the report they generate measured intent rather than software. This script
// closes that gap in two directions:
//
//   --apply    writes the reassessed status and a `verified_by` list naming the
//              test cases that demonstrate each capability
//   (default)  verifies that every `verified_by` test name exists in the suite
//              and that every capability claiming `implemented` or `validated`
//              names at least one, exiting non-zero when it does not
//   --execute  also RUNS the built test binaries and requires every test a
//              capability names to have passed in that run: not skipped, not
//              failed, not missing from the output
//
// The rule it enforces: a capability may not claim to be implemented unless it
// names a test, and that test must exist. That is what stops the report drifting
// away from the software again. Existence is a weak check, though: a test can exist
// and be skipped (a fixture was not generated) or be failing. Without --execute
// the script says plainly that nothing was run.

const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const PARITY_DIR = path.join(__dirname, '..', 'parity');
const TEST_DIR = path.join(__dirname, '..', 'tests', 'native');

// Reassessment of what the code actually does, with the tests that show it.
// Only entries listed here are changed; everything else keeps its recorded
// status, because silence is not evidence either way.
const ASSESSMENT = {
  // --- media and codecs -----------------------------------------------------
  ffmpeg_media_abstraction: ['implemented', ['TheRegistryOpensSyntheticPaths', 'FFmpegProviderRegistersWhenAvailable'], ["media/Source.h","media/ffmpeg/FFmpegSource.cpp"]],
  software_decode_provider: ['implemented', ['LosslessRgbDecodesToAUniformFrame', 'SeekingBackwardsReturnsTheCorrectFrame', 'EveryFrameOfAReferenceFileIdentifiesItself', 'ReadOrderDoesNotChangeWhatIsRead'], ["media/ffmpeg/FFmpegSource.cpp"]],
  h264_decode: ['implemented', ['FractionalFrameRatesAreReportedExactly', 'SeekingBackwardsReturnsTheCorrectFrame'], ["media/ffmpeg/FFmpegSource.cpp"]],
  mp4_mov_demux: ['implemented', ['FractionalFrameRatesAreReportedExactly', 'InterleavedH264AndAacKeepTheirAudioWhileTheVideoSeeks', 'AlternatingAudioAndVideoReadsDoNotCorruptEachOther'], ["media/ffmpeg/FFmpegSource.cpp"]],
  common_audio_decode: ['implemented', ['AudioDecodesToTheRequestedRateAndLayout', 'AudioReadsAreContinuousAcrossBlocks', 'AudioMatchesTheReferenceSignalAtAnyStartSample', 'ConsecutiveAudioBlocksReproduceTheWholeSignal', 'AudioAtAnotherRateStaysAlignedWithTheSource', 'TheTailOfAResampledFileIsNotDiscarded'], ["media/ffmpeg/FFmpegSource.cpp"]],
  vfr_timestamp_map: ['implemented', ['TimestampMapIsBuiltFromRealPackets', 'VariableFrameRateSourcesHoldFramesForLonger', 'TimestampsAreRelativeToSourceTimeZero', 'ScanningTimestampsDoesNotDisturbAReadInProgress'], ["media/TimestampMap.cpp","media/ffmpeg/FFmpegSource.cpp"]],
  media_probe: ['implemented', ['ProbeDescribesARealFile', 'SyntheticProbeDescribesItsStreams'], ["media/Source.h","media/ffmpeg/FFmpegSource.cpp"]],
  media_fingerprint: ['implemented', ['FileFingerprintsAreStableAndDiscriminating', 'FingerprintsNoticeAChangeAnywhereInTheFile'], ["media/Ingest.cpp","core/util/Sha256.cpp"]],
  duplicate_detection: ['implemented', ['IngestingTheSameMediaTwiceDoesNotDuplicateIt'], ["media/Ingest.cpp"]],
  media_bins: ['implemented', ['DeletingABinUnfilesItsMediaRatherThanDestroyingIt', 'BinCyclesAreRejected'], ["core/project/Schema.cpp","core/project/ProjectStore.cpp"]],
  // FFmpeg will decode these; nothing exercises them, so they stay partial.
  hevc_decode: ['partial', []],
  still_image_decode: ['partial', []],
  probe_diagnostics: ['partial', []],
  relink_media: ['partial', ['RelinkUpdatesThePathAndClearsTheMissingFlag', 'FingerprintsIdentifyMediaForRelink'], ["core/project/ProjectStore.cpp","media/Ingest.cpp"]],

  // --- audio ----------------------------------------------------------------
  audio_master_clock: ['implemented', ['AudioClockDerivesTimeFromRenderedSamples', 'PlaybackAdvancesOnTheAudioClock'], ["audio/AudioClock.cpp","audio/AudioSink.cpp"]],
  audio_decode: ['implemented', ['AudioDecodesToTheRequestedRateAndLayout', 'LossyAudioWithEncoderPrimingIsPlacedOnTheSampleItBelongsTo', 'AStreamThatStartsLateIsPlacedAtItsOwnStartTime', 'SourceTimeZeroIsTheFirstPictureNotTheContainersOwnClock'], ["media/ffmpeg/FFmpegSource.cpp"]],
  audio_device_output: ['implemented', ['OpeningTheDefaultSinkAlwaysYieldsSomething', 'TheOfflineSinkPullsBlocksAndAdvancesTheClock'], ["audio/WasapiSink.cpp","audio/AudioSink.cpp"]],
  audio_buffering: ['implemented', ['TheOfflineSinkPullsBlocksAndAdvancesTheClock'], ["audio/WasapiSink.cpp"]],

  // --- retiming and graphics (engine) ---------------------------------------
  time_remapping: ['implemented', ['ASpeedRampSetsTheClipsLengthAndPlaysTheSourceThroughItsSegmentsExactlyAtTheirBoundaries', 'ASegmentThatRunsBackwardsReachesBackAndTheWindowGrowsToIt', 'SplittingARampedClipPlaysExactlyWhatItPlayedBeforeAndUndoPutsItBack', 'ARampKeepsTheAudioPartnerInStepAndCanBeTakenOffToLeaveSteadyPlayback', 'ARampIsRefusedWhereItCouldNotPlayAndTheEditsItWouldBreakAreRefused', 'ARampSurvivesClosingAndReopeningTheProject', 'AudioFollowsTheSameSpeedRampFreezeAndReverseCurveAsVideo', 'APitchKeepingClipRetimedByARampKeepsItsPitchWhereVarispeedFollowsTheSpeed', 'ARetimedPitchKeepingClipIsTheSameWhateverBlocksItIsAskedForInAndHoldsAndReversesFallBackAndSaySo'], ["core/project/ProjectStore.cpp","timeline/TimelineCompiler.cpp","audio/AudioMixer.cpp","audio/TimeStretch.cpp"]],
  shape_graphics: ['implemented', ['GraphicsDocumentsDrawShapesImagesAndRoundTripTheirVersionedFormat', 'AnchoredGraphicElementsKeepTheirMarginsAndSizeWhenThePictureChangesShape', 'AGraphicLivesInTheProjectAndItsClipDrawsItAnimatedWithNoFileOutsideTheProject', 'AGraphicElementTurnsAboutItsMiddleAndTheDocumentNeedsTheNewestSchemaOnlyWhenItUsesIt', 'AnOutlineIsDrawnInsideAShapeAndRoundedCornersCutTheCornersAway', 'AShadowFollowsTheElementInThePicturesOwnDirectionsAndSoftensWithItsBlur', 'ClickingTheCanvasSelectsTheTopElementAllowingForItsTurnAndGripsFollowTheTurn', 'DraggingMovesWithSnapResizesFromTheOppositeEdgeTurnsAndAlignsToTheDocument'], ["effects/GraphicsDocument.cpp","render/Graphics.cpp","ui/GraphicsDesigner.cpp"]],
  text_graphics: ['implemented', ['TextIsRasterisedAtTheSizeAskedWrappedAndAlignedAndAFontThatIsMissingIsSaid', 'TemplateControlsTakeColoursAndRangesAndFontListsFallBackToWhatIsInstalled', 'TextAndPicturesTakeOutlinesAndRoundedCornersToo', 'TheDesignerAddsDuplicatesStacksAndRemovesElementsAndChecksWhatItIsGiven'], ["render/TextRaster.cpp","render/Graphics.cpp","ui/GraphicsDesigner.cpp"]],
  graphics_keyframes: ['implemented', ['AnimatedGraphicPropertiesFollowTheirKeysAndEachInterpolationKeepsItsEndpoints', 'AGraphicLivesInTheProjectAndItsClipDrawsItAnimatedWithNoFileOutsideTheProject'], ["effects/GraphicsDocument.cpp","render/Graphics.cpp"]],
  essential_graphics_templates: ['implemented', ['MotionGraphicsTemplatesExposeVersionedReusableControls', 'TemplateControlsTakeColoursAndRangesAndFontListsFallBackToWhatIsInstalled', 'TemplatePackagesAreInstalledByVersionAndAnInstanceKeepsDrawingTheVersionItWasMadeWith', 'GraphicsAndTemplatesSurviveReopeningAndAProjectFromBeforeThemGetsEmptyTables', 'EveryBuiltInTemplateIsValidDrawsWithItsDefaultsAndMakesControlsForWhatAPersonChanges', 'ATemplateIsInstalledOnceMadeIntoTitlesAndItsValuesChangedInOneStep', 'GraphicsAreCreatedPlacedOnTheTopFreeTrackEditedAndOnlyDeletedWhenNothingShowsThem'], ["effects/GraphicsDocument.cpp","core/project/ProjectStore.cpp","ui/GraphicsDesigner.cpp"]],

  // --- timeline -------------------------------------------------------------
  sequence_records: ['implemented', ['InitializeCreatesSchemaAtCurrentVersion', 'LoaderBuildsASnapshotThatCompiles'], ["core/project/Schema.cpp","timeline/SequenceLoader.cpp"]],
  video_tracks: ['implemented', ['TracksCannotShareAnOrderWithinOneSequenceAndType', 'CompositeOrderFollowsTrackOrderNotTrackId'], ["core/project/Schema.cpp","timeline/TimelineCompiler.cpp"]],
  audio_tracks: ['implemented', ['AudioIsOrderedToo', 'TracksSumRatherThanOcclude'], ["core/project/Schema.cpp","audio/AudioMixer.cpp"]],
  clip_records: ['implemented', ['OverlappingClipsOnOneTrackAreRejected', 'SplitProducesTwoAdjacentClipsAtExactSourceTime'], ["core/project/ProjectStore.cpp"]],
  timeline_compiler: ['implemented', ['CompileResolvesTheActiveClipAndSourceTime', 'ClipRangesAreHalfOpen'], ["timeline/TimelineCompiler.cpp"]],
  markers: ['implemented', ['MarkersAttachToSequencesClipsAndMedia'], ["core/project/Schema.cpp","core/project/ProjectStore.cpp"]],
  mixed_rate_sampling: ['implemented', ['SourceTimeStaysExactAtFractionalRates', 'TickTimebaseIsLosslessForBroadcastRates'], ["core/time/RationalTime.cpp","timeline/TimelineCompiler.cpp"]],
  // The link group is stored and carried into plans, but nothing links,
  // unlinks, or moves a group as a unit.
  linked_av_groups: ['implemented', ['MovingALinkedClipMovesItsPartnerByTheSameTime', 'ALinkedMoveThatAnyMemberCannotTakeChangesNothing', 'TrimmingALinkedClipsEdgesTrimsItsPartnerByTheSameTime', 'CuttingALinkedClipCutsItsPartnersAndKeepsEachSideJoined', 'DeletingALinkedClipDeletesItsPartnersAndRippleClosesEachTracksGap', 'ARandomSessionOfLinkedEditsKeepsEveryTakeInStepAndUndoesCompletely'], ['core/project/ProjectStore.cpp', 'core/commands/Command.h']],

  // --- trimming -------------------------------------------------------------
  trim_start: ['implemented', ['TrimIsClampedToTheMediaDuration'], ["core/project/ProjectStore.cpp"]],
  trim_end: ['implemented', ['TrimIsClampedToTheMediaDuration'], ["core/project/ProjectStore.cpp"]],
  ripple_delete: ['implemented', ['RippleDeleteClosesTheGap', 'UndoReversesARippleDeleteIncludingTheShift'], ["core/project/ProjectStore.cpp"]],
  lift: ['implemented', ['UndoAndRedoRestoreExactState'], ["core/project/ProjectStore.cpp"]],
  extract: ['partial', ['RippleDeleteClosesTheGap'], ["core/project/ProjectStore.cpp"]],

  // --- render and effects ---------------------------------------------------
  program_playback: ['implemented', ['TheEngineRendersTheCorrectSourceFrame', 'PlaybackAdvancesOnTheAudioClock'], ["playback/PlaybackEngine.cpp","render/Compositor.cpp"]],

  // --- export ---------------------------------------------------------------
  timeline_export: ['implemented', ['ExportedVideoDecodesBackToWhatTheMonitorRendered', 'ExportWritesTheExpectedFrameCount'], ["exporter/ExportWorker.cpp","media/ffmpeg/FFmpegWriter.cpp"]],
  export_progress: ['implemented', ['ExportReportsProgressAndCanBeCancelled'], ["exporter/ExportWorker.cpp"]],

  // --- performance ----------------------------------------------------------
  // Underruns are counted and exposed, but no budget is measured or enforced.
  audio_callback_budget: ['partial', []],
  // Changesets are persisted for replay; nothing reads them back.
  crash_recovery: ['partial', []],
  // --- second pass: capabilities the manifests still called planned or
  // prototype although the render and timeline work implemented them ----------
  track_lock_mute_solo: ["implemented", ["MutedVideoTracksAreExcluded", "SoloSuppressesOtherTracksOfTheSameMedium", "LockedTracksRefuseEdits", "ALockedTrackRefusesEveryEditToItsContents", "ALockedTrackStillAllowsWhatTheLockIsNotFor", "MovingAClipOntoALockedTrackIsRefusedEvenFromAnUnlockedOne"], ["timeline/TimelineCompiler.cpp", "core/project/ProjectStore.cpp"]],
  sequence_nesting: ["implemented", ["NestedSequencesResolveThroughToTheirSources", "SelfNestingAndNestingCyclesAreRejected", "NestedSequencesAreComposedRecursively", 'NestedSequenceAudioIsMixed', 'NestedAudioFollowsTheOuterClipsRetimingAndDirection', 'EffectsStayInsideTheSequenceTheyBelongTo', 'TheEnginePlaysARealFileThroughANestedSequence', 'ANestedSequencesOwnEffectsDoNotBecomeTheOuterSequencesEffects'], ["timeline/TimelineCompiler.cpp", "playback/PlaybackEngine.cpp", "core/project/Schema.cpp"]],
  adjustment_layers: ["implemented", ["AdjustmentClipsNeedNoSourceAndOnlySitOnVideoTracks", "AnAdjustmentClipGradesEverythingBeneathIt"], ["render/Compositor.cpp", "core/project/Schema.cpp"]],

  transform_effect: ["implemented", ["ScalingShrinksTheImageAboutTheCentre", "PositionTranslatesTheImage", "GoldenTransformedPictureInPicture"], ["render/Compositor.cpp"]],
  opacity_effect: ["implemented", ["OpacityEffectScalesContribution", "KeyframedOpacityRampsOverTheClip"], ["render/Compositor.cpp"]],
  crop_effect: ["implemented", ["CropRemovesPartOfTheSource"], ["render/Compositor.cpp"]],
  motion_keyframes: ["implemented", ["ClipEffectsAreSampledInClipLocalTime", "AnimatedValueInterpolatesLinearly", "BezierHandlesShapeTheCurve", "SplitKeyframesReproducesTheOriginalCurveOnBothSides", "SplittingAnAnimatedClipPreservesItsAnimationForEveryInterpolation", "UndoingASplitRestoresPlaybackAndAnimationExactly"], ["core/anim/Keyframe.cpp", "render/Compositor.cpp", "core/project/ProjectStore.cpp"]],
  effect_bypass: ["implemented", ["DisabledEffectsAreDropped", "IntrinsicEffectsCannotBeRemoved"], ["render/Compositor.cpp", "core/project/ProjectStore.cpp"]],
  // A fixed pipeline, not a graph: no per-node caching and no branching.
  effect_graph: ["partial", [], []],

  transition_model: ["implemented", ["TransitionsJoinClipsOnTheSameTrack", "ATransitionReportsBothSidesAndItsProgress", "ATransitionBetweenTwoClipsRequiresThemToBeAdjacent", "SplittingTheOutgoingClipMovesItsTransitionToTheHalfThatNowEndsAtTheCut", "MovingAClipAwayDetachesItsTransitionInsteadOfLeavingItDangling", "ValidateDatabaseReportsATransitionThatNoLongerJoinsItsClips", "ARandomSessionOfEditsNeverLeavesTheProjectInvalidAndUndoesCompletely"], ["core/project/Schema.cpp", "core/project/ProjectStore.cpp", "timeline/TimelineCompiler.cpp"]],
  cross_dissolve: ["implemented", ["CrossDissolveMixesByProgress", "GoldenDissolveMidpoint"], ["render/Compositor.cpp"]],
  audio_crossfade: ["implemented", ["AudioTransitionsCrossFade", 'AConstantPowerTransitionHoldsLoudnessAcrossTheCut', 'ACrossfadePlaysTheMediaOnEitherSideOfTheCut', 'WhereTheMediaHasNoHandleTheCrossfadeIsSilenceNotAHeldSample', 'AFadeOnOneClipHasNoHandle'], ["audio/AudioMixer.cpp"]],
  // Alignment is recorded and honoured on playback, but nothing places a
  // transition from it or reports insufficient handles.
  transition_alignment: ["partial", ["TransitionAlignmentIsEnforcedNotJustRecorded", "ChangingATransitionsTimingIsValidatedLikeCreatingIt"], ["core/project/Schema.cpp", "core/project/ProjectStore.cpp"]],

  audio_mixer: ["implemented", ["TracksSumRatherThanOcclude", "TrackGainIsAppliedInDecibels", "TrackPanHoldsPowerAcrossTheImage", 'ClipEdgesFallOnTheirOwnSample', 'BlocksOfAnySizeTileToTheSameAudioAsOneLargeBlock', 'ARetimedClipReadsItsSourceAtItsRate', 'AReversedClipPlaysItsSourceBackwards', 'InterpolationBetweenSamplesIsCubicNotLinear'], ["audio/AudioMixer.cpp"]],
  audio_keyframes: ["implemented", ['AutomationIsEvaluatedAtEverySampleNotOncePerBlock', 'TrackEffectsApplyInTimelineTimeToTheWholeTrack', 'AutomationIsWrittenByTheRuleOfItsModeThinnedAndReplacingOnlyWhatItCovers'], ["audio/AudioMixer.cpp", "core/anim/Keyframe.cpp"]],
  // Volume, gain and pan only.
  audio_effects: ["partial", ['ARoleIsKeptOnTheClipCarriedBySplitsAndItsChainIsReplacedNotStacked'], ["ui/AudioWorkflow.cpp"]],
  wide_gamut: ["partial", ['AcesAndCameraGamutsRotateByTheirPublishedMatricesAndKeepWhiteWhite', 'SceneReferredSpacesAreNamedAndLogFootageReachesTheDisplayNeutralGreyAtGreyAndHighlightsUnclipped'], ["render/ColorManagement.cpp"]],
  color_managed_pipeline: ["partial", ['CameraLogAndAcesTransfersMatchTheirPublishedGreyPointsAndRoundTripAcrossTheirRange', 'AClipCanSayWhatItsPictureIsAndLogFootageIsReadAsLogEvenWhenTheFileSaysNothing', 'FramesInTheSequenceSpaceAreLeftAloneAndOtherSpacesAreConvertedUnderTheCurrentVersion'], ["render/ColorManagement.cpp", "render/Compositor.cpp"]],
  copy_link_ingest: ["implemented", ['ACopyIsOnlyACopyIfReadingItBackGivesTheSameBytesAndEverythingElseLeavesNothingBehind', 'AnIngestPlanNamesEveryCopyWithoutOverwritingAndSaysWhenThereIsNoRoom'], ["media/IngestWorkflow.cpp", "app/SessionIngest.cpp"]],
  checksum_ingest: ["implemented", ['ACopyIsOnlyACopyIfReadingItBackGivesTheSameBytesAndEverythingElseLeavesNothingBehind'], ["media/IngestWorkflow.cpp"]],
  cross_dissolve: ["implemented", ['CrossDissolveMixesByProgress', 'EveryTransitionStartsAsTheOutgoingPictureAndEndsAsTheIncomingOneAndNeverLeavesTheRange', 'TheCompositorDrawsTheNamedTransitionAtTheProgressOfTheFrameAndTellsOfOneItDoesNotHave'], ["render/Transitions.cpp", "render/Compositor.cpp"]],
  transition_alignment: ["implemented", ['ATransitionIsAddedAtTheCutKeptWithinTheHandlesAndRefusedWhereThereIsNoRoomOrAnotherIsThere', 'ATransitionIsChangedToAnotherKindAndLengthInOneStepAndTakenOffAgain'], ["ui/Transitions.cpp"]],
  loudness_metering: ["implemented", ['LoudnessIsMeasuredAsEbuR128Specifies', 'TruePeakSeesTheOvershootBetweenSamples', 'AMeterReportsLoudnessInLufs', 'AStreamingLoudnessMeterGivesWhatMeasuringTheWholeProgrammeGivesWhateverTheBlockSizes', 'LoudnessAdviceNamesTheGainAndWarnsWhenThePeakWouldPassTheCeilingAndGainsAreOrdinaryEdits'], ["audio/Dsp.cpp", "ui/AudioWorkflow.cpp"]],

  basic_color_correction: ["implemented", ["ExposureIsMeasuredInStops", "SaturationPivotsAroundRec709Luma", "ContrastPivotsAroundMidGrey", "GoldenGradedBars"], ["render/Compositor.cpp"]],
  // --- the GPU and the desktop application (2026-10-07) -------------------------
  gpu_compositor_d3d11: ['partial', ['LayersOpacityAndGradeMatchTheSoftwareCompositorOnPicturesWithEdgesAndTransparency', 'MotionCropAndAnchorPlaceAPictureTheSameWayOnBothCompositorsForAnySourceShape', 'DissolvesAdjustmentClipsGeneratorsAndSequenceEffectsMatchTheSoftwareCompositor', 'WhatTheGpuCannotRenderIsRefusedWithAReasonSoTheSoftwareCompositorDoesIt', 'AnEngineAskedForTheGpuComposesOnItAndRendersTheSamePicturesAsTheSoftwareEngine', 'AFrameTheGpuCannotRenderIsComposedInSoftwareAndCountedAndAnExportEngineNeverUsesTheGpu', 'EveryColourToolOnTheCardGivesTheSamePictureAsTheSoftwareCompositor', 'ALutOnTheCardGivesTheSamePictureAsTheSoftwareCompositorForCubesCurvesDomainsAndIntensity'], ["render/D3D11Compositor.cpp", "render/CompositorParams.h", "render/ColorOps.h", "playback/PlaybackEngine.cpp"]],
  // --- look-up tables, transcripts and text-based editing (2026-10-07) --------------
  cube_lut: ['implemented', ['CubeLutLoadsOnceAndBlendsByIntensity', 'MissingLutIsReportedAndBypassed', 'OneDimensionalCubeTablesAreCurvesPerChannelAndAdobeRangesAreRead', 'TheLutSamplerMapsEveryColourTheSameWhicheverCallIsUsedAndDecimalsDoNotDependOnTheLocale', 'ALutOnTheCardGivesTheSamePictureAsTheSoftwareCompositorForCubesCurvesDomainsAndIntensity'], ["render/CubeLut.cpp", "render/Compositor.cpp", "render/D3D11Compositor.cpp", "timeline/SequenceLoader.cpp"]],
  lut_management: ['partial', ['TheLookPackIsWrittenOnceReadsAsCubesAndEveryLookIsASaneMonotoneGrade', 'CubeLutLoadsOnceAndBlendsByIntensity', 'MissingLutIsReportedAndBypassed'], ["ui/LutLibrary.cpp", "ui/LookPack.cpp", "app/LutPreviewProvider.h"]],
  transcription: ['partial', ['TheEnginesWordsBecomeTimedWordsWithoutControlTokensAndWithTheSpokenEnd', 'TheTranscriptIsAFileThatComesBackWholeAndAWronglyShapedOneIsRefused', 'ARecordedSentenceIsTranscribedByTheRealEngineAndFollowsTheWordsSpoken', 'TheEngineIsFoundByExplicitPathOrBySearchingAndNeedsAModel'], ["speech/Transcript.cpp", "speech/Whisper.cpp"]],
  text_based_editing: ['partial', ['WordsAreMappedOntoTrimmedAndRepeatedClipsAtTheirTimelineTimes', 'DeletingWordsRemovesTheirTimeFromEveryTrackAsOneUndoStepAndTheRestStaysInSync', 'RemovingSeveralRangesPlansEachAgainstTheClipsTheEarlierOnesLeave', 'PausesBetweenWordsAreRemovedByLengthAndFillersAreFoundAsWordPositions', 'CaptionsAreMadeFromTheWordsOnTheTimelineAtTheirTimelineTimesInOneStep', 'FillersPausesAndPhrasesAreFoundIgnoringCaseAndPunctuation'], ["ui/TextEdit.cpp", "ui/EditPlanner.cpp", "app/SessionTranscript.cpp"]],
  hardware_decode_d3d11va: ['partial', ['HardwareDecodedPicturesAreComposedWhereTheyAreAndMatchTheSoftwareDecode', 'AnEngineDecodesH264OnTheGpuAndComposesItWithoutUploadingAPicture'], ["media/ffmpeg/FFmpegSource.cpp", "media/DeviceFrame.h", "render/D3D11Compositor.cpp"]],
  hardware_capability_discovery: ['partial', ['TheAdaptersAreListedWithCapabilitiesAndOneIsChosenByTheSelector', 'GpuSelectionIsCapabilityBasedAcrossAmdNvidiaIntelAndApple'], ["render/D3D11Compositor.cpp", "render/GpuDevice.cpp"]],
  render_backend_abstraction: ['partial', ['GpuSelectionUsesCpuForSupportedWorkAndRefusesImpossibleZeroCopy', 'WhatTheGpuCannotRenderIsRefusedWithAReasonSoTheSoftwareCompositorDoesIt'], ["render/GpuDevice.cpp", "render/D3D11Compositor.cpp"]],
  program_monitor: ['partial', ['ThePresenterRendersTheLatestRequestAndDropsWhatWasOvertaken', 'TheMonitorPicksARenderSizeFitsThePictureAndDrawsSafeMargins'], ["ui/Monitor.cpp", "app/MonitorItem.cpp"]],
  transport_jkl: ['implemented', ['TheTransportPlaysShuttlesStepsAndStopsOrLoopsAtTheEnds'], ["ui/Transport.cpp"]],
  frame_step: ['implemented', ['TheTransportPlaysShuttlesStepsAndStopsOrLoopsAtTheEnds'], ["ui/Transport.cpp"]],
  three_point_edit: ['implemented', ['ThreePointEditsResolveTheMissingPointAndRefuseWhatCannotBe', 'InsertingAndOverwritingASourcePlacesLinkedPictureAndSoundAndPastingCarriesEffects'], ["ui/EditPlanner.cpp"]],
  timeline_ruler: ['implemented', ['TheRulerChoosesReadableStepsAndLabelsThemAsTimecode'], ["ui/TimelineView.cpp", "app/TimelineItem.cpp"]],
  playhead: ['implemented', ['TheViewportMapsTimeToPixelsZoomsAboutAPointAndFollowsThePlayhead'], ["ui/TimelineView.cpp", "app/TimelineItem.cpp"]],
  // --- export (2026-10-07) --------------------------------------------------------
  export_presets: ['implemented', ['EveryPresetNamesRealEncodersAndResolvesToSettingsThatMatchItsPromises', 'ABitrateScalesWithThePictureAndIsClampedAndAProfileCodecIsLeftToItsProfile', 'HardwareEncodersAreUsedWhenTheyWorkSkippedWithAReasonWhenTheyDoNotAndSoftwareStaysAvailable', 'APresetRefusesASequenceItCannotDeliverAndCorrectsTheFileExtension', 'EveryPresetThatWorksOnThisMachineProducesAFileThatPassesTheCheck', 'TheHardwareAndFinishingPresetsThatNeedAnHdPictureExportAtOneAndPassTheCheck'], ["exporter/ExportPresets.cpp"]],
  export_queue: ['implemented', ['QueuedExportsRunOneAtATimeInOrderAndKeepTheirProgressAndOutcome', 'AFailedExportIsRecordedWithItsReasonAndCanBeRetriedAndACancelledOneStopsPromptly', 'WaitingJobsCanBeCancelledRemovedReorderedAndPausedWithoutDisturbingTheRunningOne', 'TheQueueSurvivesARestartAJobThatWasRunningIsInterruptedAndADamagedRecordIsKeptAside'], ["exporter/ExportQueue.cpp"]],
  export_validation: ['implemented', ['TheCheckOfAFinishedExportNamesWhatIsWrongWithATruncatedWrongOrMissingFile', 'EveryPresetThatWorksOnThisMachineProducesAFileThatPassesTheCheck'], ["exporter/ExportValidation.cpp"]],
};

function collectTestNames() {
  const names = new Set();
  for (const file of fs.readdirSync(TEST_DIR)) {
    if (!file.endsWith('.cpp')) continue;
    const source = fs.readFileSync(path.join(TEST_DIR, file), 'utf8');
    for (const match of source.matchAll(/CUTLINE_TEST\((\w+)\)/g)) names.add(match[1]);
  }
  return names;
}

function load() {
  return fs.readdirSync(PARITY_DIR)
    .filter(name => name.endsWith('.yaml'))
    .map(name => ({ name, data: JSON.parse(fs.readFileSync(path.join(PARITY_DIR, name), 'utf8')) }));
}

function apply(manifests) {
  let changed = 0;
  for (const { name, data } of manifests) {
    let dirty = false;
    for (const feature of data.features) {
      const entry = ASSESSMENT[feature.id];
      if (!entry) continue;
      const [status, verified, components] = entry;
      const tests = {
        unit: verified,
        // The native suites are integration tests in every sense that matters
        // here: they drive the real store, the real decoder, and the real
        // compositor rather than a stand-in.
        integration: verified.length > 0 ? ["cutline native test suite (ctest)"] : [],
        fixture: verified.length > 0 ? ["tests/golden", "build/*/fixtures (generated)"] : [],
      };
      const next = JSON.stringify({ status, tests, components: components || [] });
      const current = JSON.stringify({
        status: feature.status,
        tests: feature.tests || { unit: [], integration: [], fixture: [] },
        components: (feature.implementation && feature.implementation.components) || [],
      });
      if (next !== current) {
        feature.status = status;
        if (verified.length > 0) {
          feature.tests = tests;
          feature.implementation = {
            components: components || [],
            notes: "Verified by the named native tests; see IMPLEMENTATION_STATUS.md.",
          };
        }
        dirty = true;
        changed++;
      }
    }
    if (dirty) fs.writeFileSync(path.join(PARITY_DIR, name), JSON.stringify(data, null, 1) + '\n');
  }
  console.log(`updated ${changed} capability record(s)`);
}

// Runs every built test binary and records each test's outcome from its output
// ("  ok   Name", "  skip Name (reason)", "  FAIL Name"). Returns null when there is
// nothing built to run.
function runSuites() {
  const build = path.join(__dirname, '..', 'build', 'native');
  if (!fs.existsSync(build)) return null;
  const binaries = fs.readdirSync(build).filter(name => /^cutline_.*_tests\.exe$/.test(name));
  if (binaries.length === 0) return null;
  const environment = Object.assign({}, process.env, {
    CUTLINE_FIXTURE_DIR: path.join(build, 'fixtures'),
    CUTLINE_GOLDEN_DIR: path.join(__dirname, '..', 'tests', 'golden'),
  });
  const outcomes = new Map();
  for (const binary of binaries) {
    const run = spawnSync(path.join(build, binary), { env: environment, encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 });
    for (const line of (run.stdout || '').split(/\r?\n/)) {
      const match = /^\s+(ok|skip|FAIL)\s+(\w+)/.exec(line);
      if (match) outcomes.set(match[2], match[1] === 'ok' ? 'passed' : match[1] === 'skip' ? 'skipped' : 'failed');
    }
  }
  return outcomes;
}

function verify(manifests, tests, outcomes) {
  const problems = [];
  let claimed = 0;
  let evidenced = 0;
  let established = 0;
  for (const { name, data } of manifests) {
    for (const feature of data.features) {
      const verified = (feature.tests && feature.tests.unit) || [];
      for (const test of verified) {
        if (!tests.has(test)) {
          if (!test.startsWith("cutline native")) {
            problems.push(`${name}:${feature.id} names a test that does not exist: ${test}`);
          }
        }
      }
      if (feature.status === 'implemented' || feature.status === 'validated') {
        claimed++;
        if (verified.length === 0) {
          problems.push(`${name}:${feature.id} claims "${feature.status}" but names no test`);
        } else {
          evidenced++;
          if (outcomes) {
            const notPassed = verified.filter(test => !test.startsWith('cutline native') && outcomes.get(test) !== 'passed');
            if (notPassed.length === 0) {
              established++;
            } else {
              problems.push(`${name}:${feature.id} claims "${feature.status}" but these tests did not pass in this run: ` +
                            notPassed.map(test => `${test} (${outcomes.get(test) || 'not run'})`).join(', '));
            }
          }
        }
      }
    }
  }
  console.log(`${evidenced}/${claimed} capabilities claiming implementation name a test that exists`);
  if (outcomes) {
    console.log(`${established}/${claimed} have every named test passing in this run (${outcomes.size} tests executed)`);
  } else {
    console.log('NOT EXECUTED: existence of the named tests was checked, not whether they pass (use --execute)');
  }
  if (problems.length > 0) {
    console.error('\nParity cross-check failed:');
    for (const problem of problems) console.error('  ' + problem);
    process.exit(1);
  }
  console.log('parity cross-check passed');
}

const manifests = load();
if (process.argv.includes('--apply')) {
  apply(manifests);
} else {
  let outcomes = null;
  if (process.argv.includes('--execute')) {
    outcomes = runSuites();
    if (outcomes === null) {
      console.error('--execute needs the test binaries: run scripts\\build.bat first');
      process.exit(1);
    }
  }
  verify(manifests, collectTestNames(), outcomes);
}
