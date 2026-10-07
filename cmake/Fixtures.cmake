# Generates the media fixtures the FFmpeg decode tests read.
#
# Fixtures are generated rather than committed: a few seconds of video is large
# next to the source, and a file encoded by one FFmpeg build is not guaranteed
# byte-identical to one encoded by another, so committing them would make the
# repository large *and* still machine-dependent.
#
# They are produced by the ffmpeg binary that ships beside the libraries, which
# keeps the generator independent of Cutline's own encode path -- a decode test
# that read a file written by our encoder would pass even if both were wrong.
#
# Everything here is best-effort: when ffmpeg is unavailable the fixtures are
# simply absent and the tests that need them report themselves as skipped.

function(cutline_generate_fixtures output_dir)
  find_program(CUTLINE_FFMPEG_BINARY ffmpeg
               HINTS "${CUTLINE_FFMPEG_ROOT}/bin" PATH_SUFFIXES bin)
  if(NOT CUTLINE_FFMPEG_BINARY)
    message(STATUS "ffmpeg binary not found: media fixtures will not be generated")
    return()
  endif()

  file(MAKE_DIRECTORY "${output_dir}")

  # Each entry is "filename|arguments", with the arguments space separated.
  # A semicolon would be swallowed by CMake's list semantics, so the argument
  # string is split with separate_arguments instead.
  set(fixtures
    # Lossless RGB. Decoding this must reproduce the source colour exactly, so
    # it pins the non-YUV path with zero tolerance.
    "solid-rgb.mkv|-f lavfi -i color=c=0x3366CC:s=320x180:r=25:d=2 -c:v ffv1 -pix_fmt gbrp"
    # Lossless, but stored as Rec.709 limited-range YUV. This is the one that
    # catches a wrong colour matrix or range: the pixels are recoverable, so any
    # error is in the conversion rather than the codec.
    "solid-yuv709.mkv|-f lavfi -i color=c=0x3366CC:s=320x180:r=25:d=2 -c:v ffv1 -pix_fmt yuv444p -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv"
    # Lossy H.264 at 29.97, with long GOPs so seeking has to land on a keyframe
    # and decode forward from it.
    "bars-2997.mp4|-f lavfi -i testsrc2=s=320x180:r=30000/1001:d=4 -c:v libopenh264 -g 30 -pix_fmt yuv420p"
    # Lossless audio, for exact sample assertions.
    "tone-48k.mkv|-f lavfi -i sine=frequency=1000:sample_rate=48000:duration=2 -c:a pcm_s16le"
    # Video and audio together, for A/V sync checks.
    "av-sync.mkv|-f lavfi -i testsrc2=s=320x180:r=25:d=3 -f lavfi -i sine=frequency=440:sample_rate=48000:duration=3 -c:v ffv1 -pix_fmt yuv420p -c:a pcm_s16le"
    # ---- Reference fixtures for demux/decode accuracy (milestone 1B) ----------
    #
    # Each frame of the video is a flat colour that encodes its own index
    # (red = N mod 256, green = N / 256), stored losslessly, so a decoded frame
    # says exactly which frame it is. The audio is an analytic chirp whose
    # frequency rises continuously: it never repeats, so a read that is a single
    # sample early or late, or that came from the wrong place, cannot match the
    # closed-form value the test computes independently. The left channel is
    #   0.5 sin(2 pi (200 t + 1500 t^2)),  the right  0.4 sin(2 pi (300 t + 900 t^2)).
    # Stored as 32-bit float so the file adds no quantisation of its own.
    "av-ref.mkv|-f lavfi -i \"color=c=black:s=64x48:r=25:d=4,format=gbrp,geq=r='mod(N,256)':g='mod(floor(N/256),256)':b=77\" -f lavfi -i \"aevalsrc=0.5*sin(2*PI*(200*t+1500*t*t))|0.4*sin(2*PI*(300*t+900*t*t)):s=48000:d=4\" -c:v ffv1 -pix_fmt bgr0 -c:a pcm_f32le"
    # The same content with the audio stream starting half a second after the
    # video: the container's time zero is the video's, and the audio must land
    # half a second in.
    "late-audio.mkv|-f lavfi -i \"color=c=black:s=64x48:r=25:d=4,format=gbrp,geq=r='mod(N,256)':g='mod(floor(N/256),256)':b=77\" -itsoffset 0.5 -f lavfi -i \"aevalsrc=0.5*sin(2*PI*(200*t+1500*t*t))|0.4*sin(2*PI*(300*t+900*t*t)):s=48000:d=4\" -c:v ffv1 -pix_fmt bgr0 -c:a pcm_f32le"
    # Both streams start 1.4 s into the container's own timeline, as a clip cut
    # from a longer recording would. Source time zero is the first picture.
    "offset.mkv|-itsoffset 1.4 -f lavfi -i \"color=c=black:s=64x48:r=25:d=4,format=gbrp,geq=r='mod(N,256)':g='mod(floor(N/256),256)':b=77\" -itsoffset 1.4 -f lavfi -i \"aevalsrc=0.5*sin(2*PI*(200*t+1500*t*t))|0.4*sin(2*PI*(300*t+900*t*t)):s=48000:d=4\" -c:v ffv1 -pix_fmt bgr0 -c:a pcm_f32le"
    # A rate that is not the project's, so the resampler is exercised.
    "chirp-44k.wav|-f lavfi -i \"aevalsrc=0.5*sin(2*PI*(200*t+1500*t*t)):s=44100:d=4\" -c:a pcm_f32le"
    # The same chirp in Matroska, whose millisecond timestamps cannot place a
    # 44.1 kHz packet exactly: seeking into it needs the packet index.
    "chirp-44k.mkv|-f lavfi -i \"aevalsrc=0.5*sin(2*PI*(200*t+1500*t*t)):s=44100:d=4\" -c:a pcm_f32le"
    # Mono audio whose timestamps jump forward 0.1 s after 94 packets: the file
    # genuinely has no samples for that interval.
    "gap.mkv|-f lavfi -i \"aevalsrc=0.5*sin(2*PI*(200*t+1500*t*t)):s=48000:d=4\" -af \"asetpts='PTS+if(gte(N,96000),0.1/TB,0)'\" -c:a pcm_f32le"
    # 29.97 fps in Matroska: timestamps are milliseconds, so frames are 33 and 34 ms
    # apart. That is rounding, and the file is constant frame rate.
    "cfr-2997.mkv|-f lavfi -i \"color=c=black:s=64x48:r=30000/1001:d=2,format=gbrp\" -c:v ffv1 -pix_fmt bgr0"
    # A real variable-frame-rate file: 25 frames at 25 fps, then one every 0.1 s.
    "vfr.mkv|-f lavfi -i \"color=c=black:s=64x48:r=25:d=8,format=gbrp\" -vf \"settb=1/1000,setpts='if(lt(N,25),N*40,1000+(N-25)*100)'\" -fps_mode passthrough -enc_time_base 1/1000 -c:v ffv1 -pix_fmt bgr0"
    # Two frames stamped with the same presentation time: a file whose frames
    # cannot be indexed by timestamp.
    "dup-timestamps.mkv|-f lavfi -i \"color=c=black:s=64x48:r=25:d=2,format=gbrp\" -vf \"setpts='if(eq(N,10),PTS-1/25/TB,PTS)'\" -fps_mode passthrough -c:v ffv1 -pix_fmt bgr0"
    # Lossy audio with encoder priming, in a container that trims it with an edit list.
    "chirp-aac.m4a|-f lavfi -i \"aevalsrc=0.5*sin(2*PI*(200*t+1500*t*t)):s=48000:d=4\" -c:a aac -b:a 192k"
    # H.264 and AAC interleaved, the commonest real-world arrangement.
    "av-aac.mp4|-f lavfi -i testsrc2=s=160x90:r=25:d=4 -f lavfi -i \"aevalsrc=0.5*sin(2*PI*(200*t+1500*t*t)):s=48000:d=4\" -c:v libopenh264 -g 25 -pix_fmt yuv420p -c:a aac -b:a 192k"
  )

  foreach(entry IN LISTS fixtures)
    string(FIND "${entry}" "|" divider)
    string(SUBSTRING "${entry}" 0 ${divider} name)
    math(EXPR rest "${divider} + 1")
    string(SUBSTRING "${entry}" ${rest} -1 argument_string)
    separate_arguments(arguments NATIVE_COMMAND "${argument_string}")

    set(target "${output_dir}/${name}")
    if(EXISTS "${target}")
      continue()
    endif()
    execute_process(
      COMMAND "${CUTLINE_FFMPEG_BINARY}" -hide_banner -loglevel error -y ${arguments} "${target}"
      RESULT_VARIABLE result
      ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
      message(STATUS "Could not generate fixture ${name}: ${errors}")
      file(REMOVE "${target}")
    endif()
  endforeach()

  file(GLOB generated "${output_dir}/*")
  list(LENGTH generated count)
  message(STATUS "Media fixtures ready: ${count} file(s) in ${output_dir}")
endfunction()
